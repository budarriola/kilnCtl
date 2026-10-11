# Safety review: saftyfx7 fixes for REVIEW_SAFTYFX6 (2026-10-10)

Reviewer: Opus, read-only on firmware. No board access.

Scope: origin/dev commits c9ffc21e7, a2ba68fa4, 8dd0e984f, 9d80a45ee and
633058e10. They fix `docs/audits/REVIEW_SAFTYFX6_2026-10-10.md` F1-F8 and I4.
Review base: 9c9600384.

Files read: `firmware/SaftyFW/src/safety_guards.c`,
`firmware/SaftyFW/src/tasks/safety_core.c`,
`firmware/KilnFW/App/drivers/control/heat_enable.c` (`heat_enable_note_pico_boot()`,
the claim and release paths, `acquire_since`), the profile_executor watchdog
caller, the new tests, and `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`.

## Verdict

F1, F4, F5, F7 and the heat_enable snapshot changes (F3, F6, F8, I4) are
tightenings: each one turns a case that used to grant into one that refuses or
holds. None of them loosens a guard.

There is one exception. F2's new S8 clear check accepts a post-trip measurement
as short as 5 s, so noise can make a kiln that is still rising fast look like a
plateau, and the clear is granted where the old frozen-window check refused it
(**MED-1**, shown by a probe test). The guard itself is not blinded, because S8
still re-trips afterwards. What weakens is the clear refusal, and that is the
property F2 was meant to strengthen.

## Findings

### MED-1: the S8 clear trusts a post-trip window as short as 5 s

`guard_condition_still_immediate()` case `SAFETY_TRIP_RATE` checks three things
in order:

1. If `s8_post_elapsed_s >= S8_POST_MIN_S` (5 s), it uses the partial window:
   `(tc - s8_post_start_c) / elapsed`.
2. If that does not apply but `s8_post_rate_valid` is set, it uses the last full
   post-trip window.
3. Otherwise it falls back to the frozen trip window.

The tripped branch of `safety_guards_tick()` rolls the post window over every
`rate_window_s` (60 s by default). So check 1 is live in every [5 s, 60 s) span
of every rolling window, not only right after the trip. It also takes priority
over a completed full window that may still show a fast rise.

Over 5 s, a thermocouple noise swing of about ±0.15 C is enough to move the
computed rate by several C/min. A kiln still rising at 12 C/min with
`max_rate_c_per_min` at 10 can read about 8.5 C/min at the clear instant, and
the clear is granted. After that clear, S8 needs two consecutive full windows
(120 s or more) over the limit before it trips again.

The probe test demonstrated this. It trips S8, rises at +0.02 C per 100 ms tick
(12 C/min) for 51 ticks (about 5.1 s), applies a -0.15 C noise sample, then
asserts that the clear is refused. The assertion failed (negtest CAUGHT), so the
clear was granted.

Before this change, the frozen trip window, which exceeded the limit by
construction, refused any clear until the window slid. That was too strict for
a real plateau (the reason for F2), but it was not noise-sensitive.

Suggested fix:

- Decide the clear only on completed full-window post rates
  (`s8_post_rate_valid`), and keep refusing on the frozen trip window until the
  first full post window completes. The F2 plateau test then needs a plateau of
  60 s or more.
- Alternatively, refuse when either the last full post rate or the partial
  window exceeds the limit, and use the partial window only once it is at least
  `rate_window_s / 2`.
- Either way, add a test with a noisy fast rise.

### LOW-1: no test covers the last-full-window branch

Disabling check 2 above (`s8_post_rate_valid` / `s8_post_last_rate_c_per_min`)
is MISSED by the SaftyFW host tests, because no test spends more than 60 s
post-trip. The MED-1 fix will need such a test anyway.

### LOW-2: GUARD_TEST_MATRIX S1/S8 row wording

The `CLEAR_TRIP` row's first clause still says "the tripping window still
exceeds the rate". After F2, past 5 s the check uses a post-trip window, not
the tripping window. The row's trailing note mentions the fresh post-trip rate,
so the row is ambiguous rather than wrong. Reword it when MED-1 is fixed.

The new S12 row (refused on a CJ over `cj_max_c` or unknown, even with a bad TC)
matches the code.

### INFO

- **I-1, K6 MISSED (redundant clear).** In the claim-less resolve of
  `heat_enable_note_pico_boot()`, removing `reboot_was_tripped = false` is not
  caught by any test. It is harmless: the snapshot OR on the next seq change is
  `(reboot_verdict_pending && reboot_was_tripped) || last_pico_tripped`, and
  `verdict_pending` is false after a resolve, which masks the stale value. Treat
  the clear as defence in depth only.
- **I-2, the snapshot cannot stay stuck.** `reboot_verdict_pending` and
  `reboot_was_tripped` are always cleared together, on both the claimed and the
  claim-less resolve. The release path no longer clears the snapshot (F8), so
  the snapshot cannot be lost across a claim release either.
  - The only "forever" case is a new-boot DIAG that never arrives. The verdict
    then stays pending and heat stays withheld. That behaviour predates this
    change and is fail-safe.
  - A claim-less fatal latch, set by a reboot that followed a TRIPPED DIAG,
    persists until the next claim. That claim pauses with `pico_fatal_reboot`,
    and one operator resume continues. This is fail-safe and matches the owner
    rule: a fatal reboot pauses.
- **I-3, aliasing (pre-existing).** If a TRIPPED DIAG, the reboot and the
  new-boot DIAG all land inside one executor watchdog period,
  `last_pico_tripped` never sees TRIPPED and the snapshot is not set. This is
  unchanged by saftyfx7.
- **I-4, permanently dead TC after an S1/S8 trip.** Because of F4, such a trip
  cannot be cleared until the TC reads valid again. This is fail-safe and as
  documented.
- **I-5, no log on the claim-less lost-trip resolve.** The claimed path logs a
  distinct lost-trip `ESP_LOGE` (F6). The claim-less path latches fatal without
  logging. This affects diagnostics only.
- **I-6, F5 is safe.** `cj_invalid = !cj_valid || !thermo_fresh` gates only the
  S12 evaluation and the S12 clear refusal. Stale thermo data is covered by S5,
  so S12 not advancing on stale data loses no protection.
- **I-7, F7.** The `s_last_good_tc_not_installed_declared` code already existed
  in 57addc36a. saftyfx7 added only the regression test
  `test_failed_cfg_read_keeps_tc_not_installed_declared`, and that test is
  negtest-proven (CAUGHT).
- **I-8, interaction with the pending rebootdet change (`safety_link.c`, not
  landed).** A spurious `reboot_seq` increment while the Pico is legitimately
  TRIPPED would now latch fatal through the snapshot OR. That fails safe: a
  pause, and one resume. If rebootdet changes when `diag_since_reboot` becomes
  true, or the order in which the executor watchdog calls
  `heat_enable_note_pico_boot()` and `note_pico_state`, re-check that the
  snapshot is still taken from the pre-reboot `last_pico_tripped`.

## Tests run

- SaftyFW host tests (full host suite, base 9c9600384): PASS.
- KilnFW `build_host_tests.ps1 -Only heat_enable`: PASS, 4/4.

## Negative tests (`tools/negtest.ps1`)

Each preset's baseline passed. Every mutated copy was removed, and the real tree
was unchanged afterwards.

### saftyfw-host

| Mutation | Verdict |
|---|---|
| F1: S12 clear regains the `tc_valid` short-circuit | CAUGHT |
| F2: partial post window branch removed | CAUGHT |
| F2: last full post window branch removed | **MISSED** (LOW-1) |
| F2: tripped-branch post measurement removed | CAUGHT |
| F4: S1 grants on a bad TC | CAUGHT |
| F4: S8 grants on a bad TC | CAUGHT |
| F5: `cj_invalid` no longer gated on thermo freshness | CAUGHT |
| F7: last-good TC-not-installed flag dropped | CAUGHT |
| PROBE: noisy 5 s fast rise must refuse the clear | CAUGHT, which demonstrates MED-1 |

### kilnfw-host

These runs used `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.

| Mutation | Verdict |
|---|---|
| K1: snapshot OR dropped (plain assignment) | CAUGHT |
| K2: claim-less verdict ignores the snapshot | CAUGHT |
| K3: release clears the snapshot again | CAUGHT |
| K4: claimed resolve uses the live flag, not the snapshot | CAUGHT |
| K5: snapshot taken only under a claim | CAUGHT |
| K6: claim-less resolve keeps `reboot_was_tripped` | MISSED (I-1, harmless) |

## Fix status

- MED-1, LOW-1, LOW-2: fixed in 055d34833/6e7470c95. The S8 clear is decided only from a completed full post-trip window; until then the frozen trip window refuses; afterwards it refuses if EITHER the last full rate or the partial window (>= 5 s) is over the limit. Negtests (partial shortcut restored, last-full check disabled, EITHER -> newest-only) all CAUGHT.
- I-5: FIXED in 4133ceb99 (ESP_LOGE on the claim-less lost-trip resolve; not host-tested).
