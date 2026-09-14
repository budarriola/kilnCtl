# `adaptive_tune_ki` effective-vs-reference ratchet, 2026-09-13

Follow-up to R6 in the opus review appended to
`docs/audits/gain_scheduling_design_2026-09-13.md` ("The `adaptive_tune`
interaction IS a ratchet"). That review raised the mechanism while auditing a
*proposed* gain schedule; this document verifies it independently in source,
establishes that it is live TODAY through a mechanism that already ships
(fuzzy), quantifies it, and fixes it. Scope: `adaptive_tune_ki.c` and its
tests only -- `pid_fuzzy.c`, its tests, `zones_http_post_parse.c`, and the
other audit docs listed as owned by concurrent sessions were read but not
edited.

## 1. The mechanism, confirmed in source

`adaptive_tune_refine_ki_locked()` (`firmware/KilnFW/App/drivers/control/adaptive_tune_ki.c`)
diagnoses from a trace and writes a correction relative to a stored gain:

- The diagnosis (`adaptive_tune_diagnose_ki()`) reads `z->trace_actual_c[]`/
  `z->trace_duty[]` -- the **effective**, closed-loop temperature/duty history
  this zone actually produced during its dwells. This is populated by
  `adaptive_tune_zone_tick()` from whatever `actual_c`/`duty` the control loop
  applied that tick, downstream of any per-tick gain rescale.
- `adaptive_tune_ki.c:245` (as of this audit; the write site inside
  `adaptive_tune_refine_ki_locked()`): `float new_ki = ki * (1.0f +
  capped_pct / 100.0f);`, where `ki` came from `zones_config_get_pid()` two
  lines earlier -- the **stored reference** gain.
- The result is written back with `zones_config_set_pid(zi, kp, new_ki,
  kd)` -- the same reference `ki` was just read from.

So the read and the write target the identical reference cell, but the
*evidence* used to decide the correction (the trace) reflects whatever the
control loop actually applied that tick, not necessarily that reference
value. Those two are the same only when nothing rescales Ki between the
reference read (once per control tick, inside `pid_update_terms()`'s caller)
and its effect on the plant.

**`ZONE_CONTROL_MODE_PID_FUZZY`'s `pid_fuzzy_adjust()` is exactly such a
rescale.** It multiplies the tick's applied `ki` by up to
`+/-MAX_NUDGE_FRACTION` (0.5) scaled by `strength_pct/100`, evaluated from
live error/rate, ahead of `pid_update_terms()` -- after the reference `ki` is
read for the tick, so the trace this zone produces reflects the *rescaled*
value, never the reference `adaptive_tune_ki` will later read and write back
to.

**Contrast, `adaptive_tune_model.c:204`** (the SIMC path,
`pid_autotune_tune_from_fopdt()`): this write is a function of a freshly
refitted `K_dc` (a steady-state duty-vs-temperature identification), not of
the previous Ki. A tick-level Ki rescale cannot bias a duty-vs-temperature
fit, so this path does not compound the way `adaptive_tune_ki.c`'s does. The
review's §5 composition argument in
`docs/audits/gain_scheduling_design_2026-09-13.md` is correct for this
writer and only this writer.

**Verdict: the mechanism is real, confirmed in source, not hypothetical.**

## 2. Conditions required

Two things must both be true for the loop to close:

1. The zone's `control_mode` is `ZONE_CONTROL_MODE_PID_FUZZY` (3) -- plain
   `ZONE_CONTROL_MODE_PID` (2) never rescales Ki between reference and
   effect, so `adaptive_tune_ki` observing and writing the same value is
   correct and closes nothing.
2. `fuzzy_strength_pct > 0` for that zone -- at `strength_pct == 0`,
   `pid_fuzzy_adjust()`'s own documented safety contract holds ("`strength_pct
   == 0` is the safety contract: reproduce the base gains", `pid_fuzzy.c`,
   confirmed passing by `test_pid_fuzzy.c`'s and
   `test_profile_executor_prestart.c`'s bit-exact-at-zero tests observed
   during this audit's host-test run), so effective == reference and nothing
   diverges.

`adaptive_tune` itself must also be enabled for the zone (unconditional
prerequisite for this file to run at all) -- not a new condition, just the
existing one.

**Today's bench configuration**: `fuzzy_strength_pct = 0.0` on all three
zones (per `project_fuzzy_ab_inert_control_mode` / this repo's live
`/api/zones` state) and `adaptive_tune` disabled on all three zones. Neither
condition holds today, so the loop is dormant -- exactly as
`docs/audits/gain_scheduling_design_2026-09-13.md` §5 already states for the
"nothing conflicts today" reason, which remains true. It stops being
dormant the moment either fuzzy is turned on for a zone with `adaptive_tune`
also enabled there, with **no code change and no gain schedule required** --
this is a defect in what already ships, not a preview of a hazard the
schedule would introduce.

## 3. Quantified: per-run compounding factor and runs to the cumulative bound

`adaptive_tune_diagnose_ki()`'s correction magnitude is **bang-bang, not
proportional to the divergence**: every triggering verdict
(`OFFSET_TOO_SMALL`, `LIMIT_CYCLE`, `OSCILLATING`) sets
`ki_correction_pct` to exactly `+/-(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE *
100.0f)` (0/`ADAPTIVE_TUNE_KI_FLOORED`/`ADAPTIVE_TUNE_KI_OK`/
`ADAPTIVE_TUNE_KI_INSUFFICIENT` are the only verdicts that do not), and
`ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE` is `0.20f`
(`adaptive_tune_internal.h:225`). The subsequent `capped_pct` clamp in
`adaptive_tune_refine_ki_locked()` is therefore a no-op on the diagnosis's
own output today -- it never actually reduces a raw value already at the cap.

**This means the per-run compounding factor is a fixed 1.20x once the
diagnosis fires, independent of `fuzzy_strength_pct`.** Strength 25 vs. 50
(measured centre-cell multipliers x1.125 and x1.25 respectively, per
`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`) changes *whether*
a given dwell's effective-vs-reference gap is large enough to cross
`ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C`/the limit-cycle amplitude floor and
therefore *whether* the diagnosis fires that run, and probably how many
consecutive runs it fires for a given plant -- but it does not change the
per-run step size once it does fire. A gain schedule with a *continuous*,
model-derived correction magnitude would behave differently; this bang-bang
classifier does not.

**Runs to the cumulative bound** (`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT =
5.0f`, `adaptive_tune_internal.h:266`), assuming the diagnosis fires every
run (worst case, both strengths): `1.2^n >= 5.0` => `n = ceil(ln(5)/ln(1.2))
= 9` runs. `1.2^8 = 4.2998`, `1.2^9 = 5.1598` -- so 8 runs of successful
growth land at 4.30x baseline, and the 9th attempted correction (which would
land at 5.16x) is the one the cumulative-bound refusal actually blocks. This
was reproduced exactly: the negative test below (guard disabled) measured
the reference Ki converge to **4.2998x baseline after 10 simulated runs**,
matching the closed-form prediction to 4 decimal places.

## 4. The refusal message

When the cumulative bound binds, `adaptive_tune_ki.c`'s refusal text (both
the growth ceiling and the symmetric floor) ends "`-- re-autotune this
zone`". Checked against the actual remediation semantics
(`adaptive_tune_refine_ki_locked()`'s own comment on `ki_baseline`, and
`adaptive_tune_model.c`'s re-latch behaviour): re-autotuning *does* clear and
re-latch `ki_baseline` to a freshly fitted value (proved by this file's own
`test_clear_ki_baseline_lets_the_next_run_relatch_fresh()`), so it is not a
non-sequitur -- it genuinely lifts the immediate refusal. **But it does not
address the cause** identified in §1: if fuzzy is still active at the same
strength on the same zone, the freshly re-latched baseline starts
diverging again on the very next dwell, and the same ~9-run walk to the
(newly higher) ceiling repeats. The message is not wrong, but it is
incomplete in exactly the way the review anticipated for a schedule -- it
points at the wrong root cause for a fuzzy-induced instance of this loop,
same as it would for a schedule-induced one. This audit did not change the
refusal wording, since the actual fix (§5) prevents the message from ever
firing for this cause -- a zone that hits the cumulative bound after this
fix is guaranteed not to have gotten there via the fuzzy divergence, so
"re-autotune" is back to being straightforwardly correct advice for it.

## 5. Fix: effective-vs-reference guard, scoped to the mechanism that is live today

Two shapes were on the table (per the review's R6): (a) divide the inferred
correction back out by the known rescale factor at the dwell's operating
point, so the diagnosis effectively corrects the reference; or (b) refuse
the correction outright whenever the observed trace cannot be trusted to
represent the reference. (a) requires reading `pid_fuzzy.c`'s tick-level
rescale, which lives in a file another session owns for this pass and is
not just one number (it is error/rate-dependent per tick, not constant
across a dwell, even though the centre cell dominates in practice) --
reconstructing it correctly from outside that module risks the exact
"transformed observable" mistake this whole class of bug is about. (b) is
what this audit implements: **`adaptive_tune_refine_ki_locked()` now reads
the zone's `control_mode`/`fuzzy_strength_pct` (read-only accessors,
`zones_config_get_control_mode()`/`zones_config_get_fuzzy_strength_pct()`)
immediately after the `FLOORED` check, and withholds any Ki correction --
verdict still recorded, `ki_applied` stays false, `ki_refusal_reason` names
fuzzy explicitly -- whenever the zone is `ZONE_CONTROL_MODE_PID_FUZZY` at a
non-zero strength.**

This is deliberately the "cheap and honest" shape the review named, not the
general one: it is scoped to the one mechanism that rescales Ki between
reference and effect *today*. It does **not** generalize automatically to a
future third mechanism (a temperature-keyed gain schedule, per
`docs/audits/gain_scheduling_design_2026-09-13.md` §5) -- that mechanism, if
it lands, needs its own equivalent check added to this same guard site, and
this document says so explicitly rather than implying the fix is
future-proof. What the fix *does* guarantee is that this file's own write
path can no longer close the loop through the mechanism that exists in this
codebase right now, and that the guard's home (one `if` block, one call
site, immediately ahead of every write in this function) is the natural
place to extend when that day comes.

## 6. Regression test

`test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy()`
(`firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c`, wired into
`run_test_adaptive_tune()` in `test_adaptive_tune.c`) drives a zone with
`control_mode = ZONE_CONTROL_MODE_PID_FUZZY`, `fuzzy_strength_pct = 50.0f`
through **the exact same 10-run, constant-offset trace** that
`test_ki_diagnosis_runaway_under_constant_error_is_capped_by_cumulative_bound()`
(same file, pre-existing) uses to *prove* a plain-PID zone's reference Ki
genuinely ratchets under that evidence -- same production functions
(`adaptive_tune_run_end()` -> `adaptive_tune_refine_ki_locked()` ->
`adaptive_tune_diagnose_ki()`), no reimplementation. It asserts the
PID_FUZZY zone's reference Ki is bit-for-bit unchanged (`1.0`, `+/-0.0`)
after all 10 runs, `ki_applied` is false every run, and the refusal reason
names fuzzy every run.

Two small test-fixture additions were needed to support this:
`s_fake_zone_cfg`'s `control_mode`/`fuzzy_strength_pct` fields and their
fake accessors (`test_adaptive_tune.c`), and an explicit
`#include "../drivers/persist/zones_config_accessors.h"` ahead of the fake
struct definition (the header was previously reached only transitively via
`adaptive_tune.c`'s own later `#include`, too late for `zone_control_mode_t`
to be visible at the point `s_fake_zone_cfg` is declared). Both default to
zero (`ZONE_CONTROL_MODE_OFF`, `0.0f`), so every pre-existing test in this
file that never sets them explicitly is unaffected by the guard.

Wired: `tools/check_test_c_files_wired.ps1` and
`tools/check_no_orphaned_checks.ps1` both passed in the full
`tools/run_all_checks.ps1` run this audit performed (see §7).

### Negative test

The guard's condition was disabled in production code
(`if (false && zones_config_get_control_mode(...` in
`adaptive_tune_ki.c`), confirming the new test fails without the fix: the
PID_FUZZY zone's reference Ki walked to **4.2998** (matching §3's closed-form
prediction) instead of staying at `1.0`, and both `ki_applied`/refusal-reason
assertions failed on every one of the 10 runs. The change was then reverted
BY HAND (the `false &&` clause removed, confirmed by re-reading the line),
`firmware/KilnFW/App/test/build/` was deleted, and the full host-test suite
was rebuilt from clean and re-run, confirming a clean pass (38/38 executables
built and passed) before anything was measured or committed -- avoiding this
repo's own `8a12521b` stale-binary hazard.

## 7. Verification performed

- Host tests: `firmware/KilnFW/App/test/build_host_tests.ps1`, clean rebuild,
  38/38 executables built and passed (exit 0) after the fix was restored.
  Two intermediate runs during this pass showed unrelated transient
  failures (`test_pid_fuzzy.c`/`sim_fuzzy_*`/`fuzzy_nine_cell_probe`
  strength-0 bit-exactness, then a `test_iter_tune.c` settle-time
  assertion) that cleared on the next rebuild with no code change on this
  session's part -- consistent with this repo's documented shared-tree
  concurrent-session hazard (other sessions are mid-edit on `pid_fuzzy.c`
  and its tests, which this session does not own or touch). No
  `adaptive_tune*` test failed in any of those runs.
- Target build: `idf.py -C firmware/KilnFW build` (via
  `Microsoft.v6.0.2.PowerShell_profile.ps1`), clean link, `KilnCtrl.bin`
  0x227730 bytes, 28% partition headroom -- unchanged shape from before this
  change (the added guard is a handful of branches, no new persisted
  fields).
- `tools/run_all_checks.ps1`: 93 of 94 checks passed. The one failure,
  `firmware\KilnFW\App\test\check_pid_fuzzy_drift.ps1`, failed with
  `'vswhere.exe' is not recognized as an internal or external command` --
  a pre-existing local toolchain-discovery problem in that check's own
  harness build step, reproduced identically in this session's earlier,
  unrelated `idf.py build`/`build_host_tests.ps1` output (the same
  `vswhere.exe` message appears verbatim there too, from `sim_fuzzy_*`'s
  MSVC toolchain lookup), and in a file this session does not own
  (`check_pid_fuzzy_drift.ps1` drives `pid_fuzzy.c`'s own drift harness).
  Not caused by, and not fixed by, this change.

## 8. Pattern note

Third instance in one day of the same generating fault: `97288659`
anchored the K_dc ratchet's plausibility check to the live adapted value
instead of the original autotune baseline; `36f88d62` found a zeroed
scratch struct (an ordinary page save) silently re-arming that same
ratchet; this is the same shape again, one level over -- **a correction
inferred from a transformed observable, written back to the untransformed
reference it was transformed from.** All three are instances of this
repo's own named "reset one side of a pair" bug class in spirit (two pieces
of state -- here, effective Ki and reference Ki -- joined by an implicit
equality contract that a third mechanism can silently break), except here
the "reset" is fuzzy's per-tick rescale rather than an explicit reset call,
and the break is persistent for as long as fuzzy stays enabled rather than
one-shot.
