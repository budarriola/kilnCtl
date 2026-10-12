# Review: SaftyFW guard fixes F1-F7 (2026-10-09)

Scope: the fix batch for `docs/audits/SAFTYFW_GUARD_REVIEW_2026-10-09.md`
F1-F7. The commits are `d53125fe` (code and tests), `a2d6d6c3` (thermo_task
forward declaration) and `e3909732` (docs). They were reviewed on origin/dev
`e7ff2656`. This is a review only; nothing was fixed.

## Findings

### HIGH-1: F1 leaves most trip-disabling fields installable while ARMED

**FIXED in 163980001** (fail-closed gate on heat-possible, see commits 163980001 and the earlier tests/gate commits on this branch).

`config_store_volatile_would_loosen_safety()` (`config_store_flash.c` ~1642)
now refuses changes to `safety_tc_installed`, `ct_installed`, the CT
fields_set bits, `ct_topology`, `zone_ct_channel`, `tc_offset_c`,
`cj_max_c`/`cj_warn_c`/`cj_time_s`, `overcurrent_pct`/`overcurrent_time_s`
and three margins. Its comment says "every other trip-relevant field is
refused on ANY change while ARMED". That claim is wrong.

`APPLY_CONFIG_VOLATILE` installs into the live record, and
`safety_core_load_guard_cfg()` rebuilds the guard config from that record on
every tick. So any field that is not on the list still changes guard
behaviour at once, with K4 energized.

These SET_PARAM ids are trip-relevant, unbounded or nonnegative-only, and
not on the list:

| Id | Field | Effect of a hostile or buggy value |
|---|---|---|
| 0x0403 | `link_dead_hard_s` | 65535 s effectively disables S6b |
| 0x0402 | `link_timeout_s` | Delays link-loss handling |
| 0x0401 | `context_max_age_s` | Lets stale context count as fresh |
| 0x0206 | `blind_grace_s` | 65535 s keeps S5 at WARN for about 18 h. S1/S11/S12/S8 are skipped on every bad read, so the Pico is blind for that time |
| 0x0103 | `tc_placement_mode` | Moving off CHAMBER_AGREED turns S2 and S10 off |
| 0x0101, 0x0102 | `tc_source`, `borrowed_zone_index` | Retarget or disable S13 |
| 0x0307 | `trip_verify_s` | Delays S9 escalation by up to 18 h |
| 0x0301 | `i_present_a` | A huge value makes `any_current_present` false, which blinds S3, S4, S9 and the S14 presence check |
| 0x031A-0x031C | `i_normal_a[]` | Raising it loosens S14 directly, even though `overcurrent_pct` is guarded. It also feeds the auto-derived `i_present_a` (`finalize_i_present_a`) |
| 0x0106-0x0108 | `ct_channel_map[]` | Only the fields_set bits are compared, so changing the values (which `safety_core.c` uses around line 1281) is not detected |
| 0x0308-0x030D, 0x0302-0x0304, 0x0310-0x0318 | `k_ct_v_per_a`, `gain[]`, `zero_counts`, `ct_cal` | Rescale or offset current, with the same blinding effect as `i_present_a` |
| 0x0203, 0x0205, 0x0207, 0x0209, 0x020A | `overshoot_time_s`, `rate_window_s`, `frozen_window_s`, `tc_disagreement_time_s`, `tc_expected_offset_c` | Stretch S2, S8, S11 and S10 |
| 0x0305, 0x0306 | `correlation_window_s`, `stuck_on_time_s` | Stretch S3 |
| 0x0404, 0x0502 | `mainfault_debounce_ms`, `estop_debounce_ms` | Delay S6a and S7 by up to 65 s |

Flipping `estop_active_level` (0x0212) while ARMED makes a healthy E-stop
read as asserted, so it self-trips S7. That is loud, not a bypass.

What F1 does close completely is the chain it was written for: setting
`safety_tc_installed` to 0, then INJECT_TC, then a fake reading. That chain
is now closed twice, by this list and by the new inject gate.

The other routes the caller asked about:
- The commit (persistent) path is not a bypass. `COMMIT_CONFIG` is refused
  outright while ARMED, except for the tc_type-only and single-channel
  ct_cal cases.
- SET_PARAM ordering is not a bypass. SET_PARAM only stages a candidate;
  nothing is live until it is installed.
- The volatile path through a different param id is the bypass this finding
  describes.

Suggested fix: invert the rule. Allow-list the few fields a package swap
legitimately changes while ARMED, and refuse every other field that differs.
Or compare the entire guard-config projection (`safety_core_load_guard_cfg`'s
inputs) except for an explicit, reviewed exception list.

### MED-1: F1 also blocks kiln package swaps whenever the board is idle

**FIXED in 163980001** (fail-closed gate on heat-possible, see commits 163980001 and the earlier tests/gate commits on this branch).

ARMED is the steady state that starts 60 s after boot, not "firing". So
`kiln_cfg_swap` (`safety_cfg_write.c`'s `volatile_install` path, whose
comment still says "never refused for ARMED") now fails for any package pair
that differs in a listed field, such as `tc_offset_c`, a CJ limit, the CT
topology or the overcurrent settings. It fails even with no firing and the
relays off.

`COMMIT_CONFIG` is also refused while ARMED. That leaves two ways to apply
such a package: reboot the Pico and apply inside the 60 s GRACE window, or
apply while TRIPPED. The swap does roll back cleanly, because the Pico
install is all-or-nothing.

There are two edge cases:
- A swap can install while the Pico is in GRACE. If a rollback is then
  needed after GRACE→ARMED, the rollback is refused, leaving the Pico
  running the new package while the ESP has the old one.
- A swap interrupted mid-transaction leaves a journal for the ESP to replay
  on its next boot. If only the ESP reboots, the Pico is still ARMED and
  refuses that replay.

Suggested fix: gate on "heat possible" (energized, or a profile/autotune
running) rather than on the ARMED state. Then update the stale ESP comment.

### LOW-1: The F2 boot-time bound rejects a whole legacy record, against the precedent next to it

**FIXED in 163980001** (fail-closed gate on heat-possible, see commits 163980001 and the earlier tests/gate commits on this branch).

`config_params_validate_ranges()` checks `|tc_offset_c| <= 50` without any
condition (`config_params.c` ~781). The comment directly above it, for
`max_rate_c_per_min`, explains why a bound added later must not reject a
stored record outright: one out-of-range field fails the whole record, and
`abs_max_temp_c` is lost along with it.

So a board that already stores `|tc_offset_c| > 50` boots on
`config_store_default` with `config_store_is_config_rejected()` latched. That
outcome is loud, not silent, and its effect is to refuse heat. It is also
unlikely: the bench value is -4.25 C. On SET_PARAM, the 50 C bound is fine
for real calibration, since thermocouple and CJ errors are a few degrees.

### LOW-2: INJECT_TC only works in the first 60 s after boot

**NOT CHANGED, documented (163980001):** thermo_task cannot see link_task's heat signal; relaxing risks an enable racing an active injection. The gate stays ARMED-or-energized (stricter, safe).

`thermo_inject_allowed()` requires `installed == 0` and not
ARMED-or-energized. Injection therefore works only during the 60 s boot
GRACE window or while TRIPPED. An active injection is dropped without notice
at GRACE→ARMED, because the snapshot path re-checks the gate.

This is safe, but undocumented:
- `LINK_PROTOCOL.md` ("Honoured only while `safety_tc_installed == 0`")
  does not mention the ARMED condition.
- The `thermo_task.h` header comment does not mention it either.

PcTools has no INJECT_TC sender, so the practical impact is low.

### LOW-3: Some of the tests do not prove the fix (negtest results below)

**FIXED in 163980001** (fail-closed gate on heat-possible, see commits 163980001 and the earlier tests/gate commits on this branch).

- The F1 terms for `zone_ct_channel` and `cj_time_s` survive removal. The
  fields_set XOR term has no test case at all.
- The F6 "S2/S10 hold while `!tc_usable`" behaviour is untested. Forcing
  `tc_usable` true, so that S2/S10 reset instead of holding, still passes.
- Two first-round mutations, F6 `return false` and the inject gate, were
  caught only by `/WX` unused-label and unused-parameter warnings. A
  semantic re-mutation of each was then caught by a real test.

### LOW-4: Docs out of date with the code

**FIXED in 163980001** (fail-closed gate on heat-possible, see commits 163980001 and the earlier tests/gate commits on this branch).

- `firmware/SaftyFW/docs/CONFIG_REFERENCE.md` line 47 says `tc_offset_c` is
  "Deliberately unbounded beyond finiteness". It is now limited to ±50 C.
- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` lines 325 and ~508 still list
  a `context_valid` gate on S9, which F3 removed.
- `firmware/SaftyFW/docs/SAFETY_MODEL.md` (S9 section, F1 sentence) says
  F1 refuses "any change to the thermocouple/CT/margin fields". That repeats
  the code comment's overclaim (HIGH-1).
- `LINK_PROTOCOL.md` and `thermo_task.h` omit the inject ARMED gate
  (LOW-2).
- No drift was found in `ARCHITECTURE.md`.

### INFO

**FIXED in 163980001** (backfill runs before the compare; mutation-tested).

- **F3 (S9 without context):** correct and complete. The debounce streak,
  the `current_sensing_commissioned` split and the CT-disabled branch are
  unchanged, so it causes no new nuisance trips.
- **F4 (unknown cold junction):**
  - What happens during bring-up: with the chip absent or not converting
    (DRDY silence), `tc_valid` is false, so the bad-read path skips the S12
    block. No new S12 warning appears.
  - When the WARN fires: only when the chip converts but the CJ half is
    NaN (CJRANGE), which is a real enclosure concern.
  - The one behaviour change: an S12 trip can no longer be cleared while
    the MAX31856 is not converting. It used to be clearable whenever
    `tc_valid` was false. This fails safe and requires the sensor to work
    before the clear.
  - Commissioning and calibration are not blocked.
- **F5 (non-finite context floats):** complete. `context_reduce_zones()`
  is the only consumer of the zone floats. `link_frame.c` only unpacks them,
  and `firing_max_c` already has its own isfinite check. If every zone is
  non-finite, the zone count is 0, so S2/S10 go inactive (their
  accumulators reset). A finite but absurd setpoint still widens S2. Both
  are the existing ESP trust boundary, not something F5 introduced.
- **F6 (bad read no longer skips context guards):** correct. S1, S11, S12
  and S8 are still skipped on a bad read, with their state held, which is
  the same as before. One side effect: on a board declared
  safety-TC-not-installed and permanently blind, S3/S4/S13/S14/S15 now run
  where they used to be suspended. Any S3 behaviour with an uncalibrated CT
  on `ct_installed != 0` boards is pre-existing and not new to F6.
- **F7 (reboot grace expires once):** the remaining caveat is safe.
  - A new announcement with an identical timestamp exactly 2^32 ms later
    is ignored. That only means no grace, so the worst case is a possible
    S6b trip during a planned ESP reboot.
  - Evaluation is skipped while the clock is stalled, and an announcement
    evaluated late just expires when it is evaluated.
  - The snapshot_is_fresh wrap is unchanged and out of scope.
- **Races:**
  - A GRACE→ARMED change between the ARMED check and the seqlock write in
    `config_store_write_volatile` is benign. Enable is refused while
    `installed == 0`, and inject is refused once ARMED.
  - TRIPPED accepts any install, but laundering an S1 clear through an
    injected reading leaves the board ARMED with enable refused, so it stays
    safe.
- `backfill_legacy_ct_topology()` runs after the would-loosen check, so a
  package carrying a stale legacy `ct_topology` byte could be refused when
  it should not be.

## Negtest spot-check (`tools\negtest.ps1 -Preset saftyfw-host`)

Baseline passed. The real tree was unchanged and the copies were removed.

| Mutation | Result |
|---|---|
| F1: drop `zone_ct_channel` memcmp term | MISSED |
| F1: drop `cj_time_s` term | MISSED |
| F2: drop validate_ranges bound | CAUGHT (test_config_store.c:3402) |
| F3: re-add `in->context_valid &&` to S9 | CAUGHT (test_safety_guards.c:4232) |
| F4: clear ignores unknown cj | CAUGHT (test_safety_guards.c:4267) |
| F4: tick does not hold on unknown cj | CAUGHT (test_safety_guards.c:4250/4253) |
| F5: drop isfinite eligibility | CAUGHT (test_snapshots.c:72-82) |
| F6: `goto` replaced by `return false` | CAUGHT only by C2220 (unused label) |
| F6: early return with label kept | CAUGHT (test_safety_guards.c:4291) |
| F6: `tc_usable` never false (S2/S10 reset, not hold) | MISSED |
| F7: drop expire-once check | CAUGHT (test_tick_timing.c:188) |
| Inject gate ignores ARMED | CAUGHT only by C2220 (unused parameter) |
| Inject gate ignores ARMED, parameter voided | CAUGHT (test exit 1) |

## Answers to the brief

1. **Is each hazard closed completely?** F2-F7: yes. F1 closes the
   INJECT chain but not the wider class it claims to close (HIGH-1).
2. **Bypass routes:**
   - ARMED-to-disarmed race: benign.
   - Commit path: closed.
   - SET_PARAM ordering: closed.
   - A different param id: open (HIGH-1).
   - A link_frame path that avoids `context_reduce_zones`: none exists.
3. **Nuisance trips and blocked commissioning:**
   - The tc_offset 50 C bound is fine.
   - The cj_invalid hold during bring-up causes no nuisance.
   - Package swaps while idle are now blocked (MED-1).
4. **Stored config bricking:** only a legacy record with
   `|tc_offset_c| > 50`. It falls back loudly to defaults with
   config_rejected (LOW-1).
5. **Are the tests real?** Mostly, apart from the gaps in LOW-3.
6. **F7 caveat:** safe.
