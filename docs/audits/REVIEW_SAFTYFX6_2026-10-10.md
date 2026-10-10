# Review: saftyfx6 (57addc36a), SaftyFW trip path fixes T1-T4

Date: 2026-10-10. Reviewer: Opus, read-only plus host negtests. No board access.

Scope: commit 57addc36a on origin/dev, which fixes T1-T4 from
`docs/audits/REVIEW_SAFTYFW_TRIP_PATH_2026-10-10.md`:

- T1: `guard_condition_still_immediate()` refuses an S1 or S8 clear while the condition holds.
- T2: `safety_core.c` keeps the last good guard config when `config_store_get_full_record()` fails.
- T3: `heat_enable.c` treats a Pico reboot after a TRIPPED DIAG as fatal.
- T4: S12 runs on a bad TC read when the cold junction is valid.

SaftyFW line numbers are for origin/dev 96e4d56d9 (unchanged since). `heat_enable.c` line
numbers are for origin/dev 08161c36f, which adds 1fcf3148e and a3c6475c1 (MED-5 reboot
verdict that survives pause/resume) on top of the reviewed commit. Negtests ran on 96e4d56d9.

## Summary

| ID | Sev | Item | Short |
|----|-----|------|-------|
| F1 | MED | T4 | S12 clear is granted while the enclosure is still hot if the TC read is bad |
| F2 | MED | T1 | S8 clear compares against the frozen tripping window; a stuck or offset reading refuses the clear forever |
| F3 | LOW | T3 | A second Pico reboot before the first boot's DIAG overwrites the tripped snapshot; the POWERON boot is then benign |
| F4 | LOW | T1 | S1/S8 clear is granted when the TC read is bad (unknown treated as a pass) |
| F5 | LOW | T4 | `cj_invalid` is not gated by `thermo_fresh`; S12 now runs on a stale CJ on the bad-read path |
| F6 | LOW | T3 | Log text and pause reason call a lost-trip POWERON reboot a "fatal cause" |
| F7 | LOW | T2 | The `tc_not_installed` last-good path is untested; S13 still reads the default record on a failed read |
| F8 | LOW | T3 x MED-5 | The claim-less reboot verdict (1fcf3148e) ignores the TRIPPED snapshot, so a lost trip is laundered across a pause/resume |
| I1 | INFO | T2 | Fail-safe at boot; stale-config window is bounded |
| I2 | INFO | T3 | Fits the owner decision; matters for autotune only; clear and per-boot reset are correct |
| I3 | INFO | T3 | Overlap with the rebootfx work in `heat_enable_note_pico_boot()` |
| I4 | INFO | tests | T3 test is weak (snapshot vs live flag not distinguished) |

No HIGH findings. Nothing here makes the guards looser than before 57addc36a; F1 and F2
are a gap and an availability problem in the new clear refusal, not regressions in the
trip path.

## Findings

### F1 (MED) T4: S12 clear granted while the cold junction is still over cj_max on a bad TC read

`firmware/SaftyFW/src/safety_guards.c:246` (S12 case of `guard_condition_still_immediate()`):

```c
return in->cj_invalid || isnan(in->cj_c) ||
       (in->tc_valid && in->cj_c > effective_f(cfg->cj_max_c, CJ_MAX_C_DEFAULT));
```

Before T4, S12 only ran when `tc_valid`, so the `in->tc_valid &&` term was consistent.
T4 (`safety_guards.c:684`) now lets S12 trip on a bad TC read with a valid CJ. The
clear check was not updated, so the same input that tripped S12 is the input that
grants the clear.

Scenario: enclosure at 90 C (cj_max 85), TC open or faulted, CJ valid. S12 trips through
T4. Operator sends CLEAR_TRIP. The CJ is still 90 C, but `tc_valid` is false, so the
check returns false and the clear is granted. `safety_guards_clear()` zeroes
`s12_over_max_elapsed_s` and the S5 blind-grace accumulator. The board is ARMED again
with a hot enclosure and a blind TC. S12 needs `cj_time_s` (60 s) again to re-trip; S5
needs `blind_grace` (60 s). The ESP side may re-request heat in that window.

Negtest proof: adding `TEST_CHECK(!safety_guards_try_clear(&s, &cfg, &bad), ...)` right
after the T4 trip in `test_try_clear` fails at `test_safety_guards.c:3410` on the
current code (CAUGHT).

Fix: drop `in->tc_valid &&` from the S12 case. The CJ is a separate measurement and
`cj_invalid`/NaN are already covered. Add the assertion above as a real test.

### F2 (MED) T1: S8 clear test is frozen at trip time and can refuse forever

`firmware/SaftyFW/src/safety_guards.c:189-198`:

```c
if (cfg->max_rate_c_per_min > 0.0f && in->tc_valid && state->s8_window_active &&
    state->s8_window_elapsed_s > 0.0f) {
    float elapsed_min = state->s8_window_elapsed_s / 60.0f;
    return ((in->tc_c - state->s8_window_start_c) / elapsed_min) > cfg->max_rate_c_per_min;
}
```

While tripped, `safety_guards_tick()` returns early (only S9 runs), so
`s8_window_start_c` and `s8_window_elapsed_s` keep their trip-time values. The S8 tick
trips before the window slides, so `elapsed_s` is at least the window threshold (60 s by
default). The check is therefore "is the reading still more than
max_rate x window above the tripping window's baseline". It ignores the time that has
passed since the trip. This is not the guard's own measure (a rate).

Scenarios where the clear is refused forever:

- A real fast rise that then holds at a plateau (kiln at equilibrium after the
  firmware cut power is unlikely, but the ESP can keep the kiln warm through another
  zone or residual heat). Reading stays above baseline + max_rate x window.
- A TC step offset (connector shift, a reseated probe) that caused the trip and stays.
  Rate is 0 after the step, the clear is still refused.
- A spurious trip from a transient read that settled at a higher steady value.

Only a power cycle recovers, and that loses the latch, which is exactly what the clear
path is meant to avoid. ARCHITECTURE.md's "only once the condition is false" is met in
the letter (the frozen window still "exceeds") but not in intent: the rate is no longer
exceeded.

The `elapsed <= 0` edge cannot happen at trip time (the guard returns false for it), so
there is no divide by zero.

Negtest proof: in the S8 block of `test_try_clear`, holding the reading constant at
60 C for 36000 ticks of 0.1 s (one hour, rate 0) and then asserting the clear is
granted fails at `test_safety_guards.c:3393` on the current code (CAUGHT).

Fix: make the refusal bounded by real time. Either keep a `s8_tripped_elapsed_s` that the
tripped branch of the tick advances by `dt_s`, and divide by
`s8_window_elapsed_s + s8_tripped_elapsed_s`, or measure a fresh post-trip rate over the
last window from the reading history. Either way a flat reading converges to rate 0 and
the clear is granted. Add the constant-reading test above.

### F3 (LOW) T3: a second Pico reboot before the first boot's DIAG loses the tripped snapshot

`firmware/KilnFW/App/drivers/control/heat_enable.c:761`:

```c
s_he.reboot_was_tripped = s_he.last_pico_tripped;
```

This overwrites the snapshot on every reboot_seq change. `last_pico_tripped` is updated
by any fresh `heat_enable_note_pico_state()` call (`heat_enable.c:848`), and the
profile executor's watchdog substitutes `SAFETY_LINK_DIAG_STATE_INIT` as a fresh state
while `!diag_since_reboot` (`profile_executor.c`, watchdog block before the
`pico_fatal_reboot` pause at line 2514).

Sequence: claim held, DIAG TRIPPED, reboot 1 detected (snapshot = true, pending), the
next watchdog tick feeds INIT (`last_pico_tripped` = false), reboot 2 detected before any
DIAG of boot 1 (snapshot overwritten with false), boot 2's DIAG says POWERON. Classified
benign, the grant is re-requested, the lost trip is never surfaced.

Two back-to-back reboots inside one link-up window are rare (brownout chains, a reset
loop), but a reset loop is exactly the case that most needs the hold.

Fix: `s_he.reboot_was_tripped = (s_he.reboot_classify_pending && s_he.reboot_was_tripped) || s_he.last_pico_tripped;`
or only let a real DIAG (not the substituted INIT) clear `last_pico_tripped`. Add the
double-reboot test (see Negtests).

### F4 (LOW) T1: S1/S8 clear granted when the TC reading is unknown

`firmware/SaftyFW/src/safety_guards.c:188` and `:193`. Both cases need `in->tc_valid`;
with a bad TC read they return false and the clear is granted. S12's own F4 rule in the
same function treats unknown as "still active" (`cj_invalid || isnan`). Granting an S1
clear while the TC is open leaves the board ARMED and blind until S5's blind grace
(60 s) re-trips.

Fix: in the S1 and S8 cases return true (refuse) when `!in->tc_valid`. The operator can
clear once a valid reading arrives, which is normally one tick.

### F5 (LOW) T4: S12 on the bad-read path uses a stale cold junction

`firmware/SaftyFW/src/tasks/safety_core.c:1360-1363`:

```c
.tc_valid = thermo.valid && thermo_fresh,
...
.cj_invalid = !thermo.cj_valid,
```

`tc_valid` is gated by `thermo_fresh`, `cj_invalid` is not. A stale snapshot (thermo
task stuck) is now a bad TC read, and T4 runs S12 on the frozen CJ value. A frozen
below-max CJ resets `s12_over_max_elapsed_s`; a frozen above-max value can trip S12 on
old data. S13 still catches the stuck publish, so this is not a missed trip, but S12
should not reason about data the core already rejected.

Fix: `.cj_invalid = !thermo.cj_valid || !thermo_fresh`.

### F6 (LOW) T3: misleading reason for a lost-trip reboot

`firmware/KilnFW/App/drivers/control/heat_enable.c:789` logs the hold as a fatal boot
cause with the boot_reason byte (0x01 POWERON for this case), and the executor pauses
with `pico_fatal_reboot` (`profile_executor.c:2514`). An operator or a bench judge reading
"fatal cause (boot_reason 0x01)" sees a contradiction.

Fix: log "reboot followed a TRIPPED DIAG (trip latch lost)" when `reboot_was_tripped`
decided it, and consider a distinct pause reason such as `pico_reboot_lost_trip`.

### F7 (LOW) T2: partial coverage

`firmware/SaftyFW/src/tasks/safety_core.c:1081-1082` restores
`safety_tc_not_installed_declared` from the last good read. Removing that line leaves
every test green (negtest MISSED). `test_failed_cfg_read_keeps_last_good_guard_cfg`
(`test_safety_core_host.c:710`) checks only S1.

Separately, on a failed read the S13 inputs and the borrowed-mismatch check still use
the default record (`fields_set` 0, so no borrowed index), so S13 counts that tick as
not advancing. Failures only come from seqlock contention during a write, about one
tick, so this is negligible today.

Fix: extend the T2 test to assert the declared-not-installed flag survives a failed
read. Optionally reuse the last good record for the S13 inputs too.

### F8 (LOW) T3 combined with MED-5 (1fcf3148e): lost trip not carried across a release

`firmware/KilnFW/App/drivers/control/heat_enable.c:768-775` (origin/dev 08161c36f). When no
claim is held, the new MED-5 path decides the verdict from the boot reason alone:

```c
s_he.reboot_was_tripped = false;
...
s_he.reboot_fatal_latched = (boot_reason & fatal_u) != 0u;
```

The T3 term is missing here, and `reboot_was_tripped` is only snapshotted when a claim
is held (`:761`). When a verdict that is still pending is re-armed on the next acquire
(`heat_enable_acquire_since()`), `reboot_was_tripped` was already cleared by the
release (`:577`), so the later POWERON DIAG classifies benign.

Scenario: autotune holds the claim, the Pico trips, the operator pauses or the claim is
released, the Pico reboots (POWERON, trip latch lost). Either the DIAG arrives while no
claim is held (verdict benign, nothing latched) or it arrives after the next acquire
(re-armed pending, snapshot false, benign). Heat is re-requested on a Pico that lost its
trip.

Fix: snapshot `last_pico_tripped` into a per-reboot-seq flag on every seq change
(claim or not, alongside `reboot_verdict_pending`), do not clear it in
`he_release_common()`, and OR it into both `reboot_fatal_latched` and the claimed-path
check at `:789`. Test: TRIPPED, release, reboot, POWERON DIAG, acquire, assert hold; and
the same with the DIAG after the acquire.

### I1 (INFO) T2 is fail-safe at boot

`config_store_get_full_record()` returns false only when the store is not loaded yet
(`s_loaded`, set once at boot before `safety_core` runs) or when both seqlock attempts
exhaust their retries. Before the first good read, `s_have_good_guard_cfg` is false and
`safety_core.c:1129` loads the default record (`abs_max` 0, `fields_set` 0), which the
commissioning gate refuses to arm. After a good read, the stale-config window is one
failed tick during a concurrent write. No stale-config hazard beyond that. The reset at
`safety_core.c:1438-1439` clears both statics with the rest of the core state.

### I2 (INFO) T3 fits the owner decision of 2026-10-10

- Benign reboot auto-resumes, fatal pauses with `pico_fatal_reboot`, undecided withholds
  heat until a DIAG of the new boot arrives: unchanged, T3 only adds "followed a
  TRIPPED DIAG" to the fatal set.
- Cleared after an operator trip clear: yes. The next fresh DIAG (ARMED/GRACE)
  sets `last_pico_tripped` false (`heat_enable.c:848`); the existing test covers it.
- Per Pico boot: the snapshot is taken on each reboot_seq change (`:761`), cleared when
  no claim is held (`:768`) and on release (`:577`). `he_k4_reset_locked()` runs on the
  same edge. No reset-one-side issue beyond F3.
- Reach: the profile claimant never gets here, because the executor FAULTs and
  releases its claim on a fresh TRIPPED DIAG before `heat_enable_note_pico_state()`
  runs. Autotune does not abort on a Pico trip and keeps its claim, so T3 is what holds
  autotune. A new firing started after a reboot that lost the trip is still allowed;
  that waits for the next protocol bump (item F6 in the owner decision, not F6 here).

### I3 (INFO) Overlap with later reboot work

1fcf3148e (firing audit MED-4/MED-5) and a3c6475c1 (SL3-R2) landed on origin/dev after
the reviewed commit and edit the same `heat_enable_note_pico_boot()` block
(`heat_enable.c:752-800`). They keep the T3 term on the claimed path but do not carry it
into the new claim-less verdict (F8). The rebootfx agent is still working in this
area; whoever fixes F3 and F8 should do it in one change to that block.

### I4 (INFO) T3 test does not pin the snapshot

`test_pico_reboot_after_tripped_holds` (`test_heat_enable.c:1118`) feeds each reboot as
one `heat_enable_note_pico_boot()` call with `diag_since_reboot` true, with no
intermediate INIT tick. Replacing `s_he.reboot_was_tripped` with `s_he.last_pico_tripped`
at `heat_enable.c:789` is therefore not caught, although the real watchdog wiring would
break. Add a step with `note_pico_boot(seq, false, ...)` then
`note_pico_state(true, INIT, ...)` before the POWERON DIAG.

## Negtests

SaftyFW, `tools\negtest.ps1 -Preset saftyfw-host -Mutations`, base 96e4d56d9, baseline PASS:

| Mutation | Expected | Result |
|----------|----------|--------|
| S8 case of `guard_condition_still_immediate()` disabled | CAUGHT | CAUGHT |
| S1 case returns false | CAUGHT | CAUGHT |
| T4 bad-path `s12_evaluate()` call disabled | CAUGHT | CAUGHT |
| T2: always reload guard cfg | CAUGHT | CAUGHT |
| T2: `tc_not_installed` last-good restore removed | MISSED (F7) | MISSED |
| Demo F1: assert S12 clear refused with hot CJ, bad TC | CAUGHT (bug) | CAUGHT, `test_safety_guards.c:3410` |
| Demo F2: assert S8 clear granted after 1 h at constant reading | CAUGHT (bug) | CAUGHT, `test_safety_guards.c:3393` |

KilnFW, `tools\negtest.ps1 -Preset kilnfw-host -Mutations`, base 96e4d56d9, baseline PASS:

| Mutation | Expected | Result |
|----------|----------|--------|
| `heat_enable.c:789` uses `last_pico_tripped` instead of the snapshot | MISSED (I4) | MISSED |
| `heat_enable.c:789` drops the `reboot_was_tripped` term | CAUGHT | CAUGHT |
| Demo F3: double reboot (TRIPPED, reboot, INIT tick, reboot, POWERON) asserts hold | CAUGHT (bug) | CAUGHT, `test_heat_enable.c:1143` |
| Sanity: single reboot with an INIT tick before the POWERON DIAG asserts hold | MISSED (passes) | MISSED, real wiring holds |

The KilnFW run ended with negtest's "REAL TREE CHANGED" error only because this audit
doc was written into the reviewing worktree during the run; every mutation verdict was
recorded before that check, and the copy was removed.
