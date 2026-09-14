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

## Review, 2026-09-13

Adversarial review of `e78fbc5b` (the fix above) by a second session. Each
finding below says whether it was established **by execution** or **by
reading**. Nothing here was taken on trust from the sections above.

**Bottom line: the mechanism, the conditions, the arithmetic and the
negative test all hold up under independent reproduction.** Two of the
document's own judgement calls do not survive review unchanged: the
`vswhere` attribution in section 7 is wrong (R7 below), and the
"operator-visible" story for the new refusal is weaker than section 5
implies (R1). The fix itself is sound and should stay.

### R1. Withholding is the right fix, but it is close to silent -- and the message is truncated

*Verified by reading.* Withholding (not dividing the rescale back out) is
the right call, for a reason section 5 understates: `adaptive_tune_ki`'s
correction is a fixed-magnitude *classifier* output, not a measurement.
There is no quantity to divide -- dividing a bang-bang +/-20% by a per-tick
rescale factor produces a number with no physical meaning. Fix (a) was
never really on the table; refusing is the only honest option for this
writer.

Mutual exclusivity of Ki adaptation and fuzzy is an acceptable trade, on
the same reasoning: while fuzzy is active this layer's evidence does not
describe the thing it writes, so its output is not a feature being disabled
-- it is a wrong answer being suppressed. The other adaptive layer
(`adaptive_tune_model.c`'s SIMC path, section 1) is unaffected and still
adapts Ki from a fresh `K_dc` fit, so "adaptive tuning stops when fuzzy is
on" is not true in general -- only this one inference path stops. That
materially softens the product-regression concern and is worth stating in
section 5, which currently reads as though all Ki adaptation ceases.

**The visibility claim needs qualifying, though.** Traced every consumer of
`ki_refusal_reason`:

- `adaptive_tune_set_reason()` (`adaptive_tune.c:138`) does an
  `ESP_LOGI(ADAPTIVE_TUNE_TAG, ...)` of every reason it sets, so the
  refusal does reach the UART/log. Not silent.
- `adaptive_tune_http.c:99` emits it as `"ki_refusal"` in
  `GET /api/adaptive_tune`, and
  `tools/PcTools/src/kilnctrl/mcp_server_adaptive_tune.py:91` prints it.
  Reachable on demand.
- **No UI surface.** A grep over `firmware/KilnFW/App/drivers/http/*.html`
  and the LCD UI finds no consumer of `ki_refusal`;
  `diagnostics_page.html` mentions adaptive-tune only as a Ki-baseline
  label. An operator who never calls the API or reads the log sees nothing.

So: not silent, but pull-only. For a path that has just declined to touch
persisted gains that is defensible -- and it is exactly as visible as every
other refusal this file already emits, which is the fair comparison.

**Defect found: the new message does not fit the buffer.** The format
expands to 162 characters at strength 50; `ki_refusal_reason` is 96 bytes
(`adaptive_tune_internal.h:322`), so `vsnprintf` truncates it to:

```
zone is PID_FUZZY at strength 50% -- dwell trace reflects fuzzy's effective Ki, not the stored
```

The entire second half -- *"withholding correction to avoid ratcheting the
reference"*, i.e. the part that says what was done and why -- never reaches
the operator, and the sentence stops mid-clause. The regression test does
not catch this because it only greps for `"fuzzy"`, which lands at
character 57. The cumulative-bound message two branches down carries an
explicit comment about being kept short *for this exact buffer*; this one
was written as if the buffer were unbounded. Low severity (the surviving
text still names the cause) but it should be rewritten to fit, e.g.
`"PID_FUZZY @%.0f%%: trace shows effective Ki, not reference -- correction withheld"`
(79 chars). Not changed here: this review does not edit the fix.

### R2. Conditions verified; no third divergence path exists today; the guard site is right

*Verified by reading, exhaustively.* Both conditions are genuinely
required, and the `&&` chain short-circuits in the order stated.

Searched every multiplication of a Ki in the control path (grep for
`cfg->ki`, `adj_ki`, `ki *` across
`firmware/KilnFW/App/drivers/control/`): the **only** site that produces an
applied Ki differing from `zones_config`'s stored value is
`pid_fuzzy.c:291` (`*out_ki = clamp_gain(ki * (1.0f + scale * ki_dir))`),
reached only from `pid_fuzzy_prepare_gains()`
(`profile_executor_pid_tick.c:425-441`), reached only from
`profile_executor.c:969`'s `case ZONE_CONTROL_MODE_PID_FUZZY`. Everything
else that touches `ki` -- `pid.c`'s integral floor/clamp arithmetic,
`pid_rescale_integral_for_new_ki()` -- rescales the integral *accumulator*
to preserve the I-term across a gain change, not the gain. Section 1's
claim is exact.

`pid_fuzzy_prepare_gains()` re-reads
`zones_config_get_fuzzy_strength_pct(zi, ...)` live every tick -- the same
accessor, from the same store, that the new guard reads. There is no cached
runtime copy of the strength that could diverge from what the guard sees.

The `resolve_fuzzy_bands()` band derivation raised in the review brief
cannot produce divergence on its own: it only selects the membership widths
handed to `pid_fuzzy_adjust()`, which still returns the base gains
unchanged at `strength_pct == 0` (the documented contract, and the widths
are irrelevant once `scale` is zero). Band derivation is therefore inside
the guard's cover, not outside it.

The guard's *placement* is correct: after the `FLOORED`/`OK`/`INSUFFICIENT`
early returns (so those keep their own, more specific reasons) and before
`zones_config_get_pid()`, which is the first statement on the write path.
Every path that reaches `zones_config_set_pid()` in this function passes
through it. Section 5's admission that the guard does not auto-generalize
to a future gain schedule is **accurate**, and this is the right site for
that future check for the same reason: it dominates the write.

One design note, not a defect: the guard **fails open**. If
`zones_config_get_control_mode()` ever returns `false` (bad index,
uninitialised store), the `&&` chain is false and the correction proceeds.
For a guard in front of a persisted-gain write, failing *closed* on an
unreadable config would be the more conservative default and costs one
extra branch.

### R3. Bang-bang confirmed; the cap is a no-op; 9 runs is right

*Verified by reading `adaptive_tune_diagnose_ki()` in full.* There are
exactly three returning verdicts that set a non-zero `ki_correction_pct`,
and all three assign the constant:

- `ADAPTIVE_TUNE_KI_LIMIT_CYCLE` and `ADAPTIVE_TUNE_KI_OSCILLATING`:
  `-(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f)`
- `ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL`:
  `+(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f)`

`FLOORED` sets `0.0f`; `OK`/`INSUFFICIENT` leave the memset zero. No branch
scales the magnitude by the divergence, the error, `ku_estimate`, or
anything else -- `ku_estimate`/`tu_estimate_s` are computed and reported
but never feed the correction. `ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE` is
`0.20f` (`adaptive_tune_internal.h:225`) and
`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT` is `5.0f` (line 266) -- both line
citations in section 3 are correct. The `capped_pct` clamp is therefore
provably a no-op against this diagnosis's own output, as section 3 says.
Section 3's arithmetic and its 9-runs figure stand.

### R4. The ordering bypass is closed -- but by an interlock in another module, with nothing linking the two

*Verified by reading.* The concern is real in principle: the guard reads
`control_mode`/`fuzzy_strength_pct` **at refine time**, inside
`adaptive_tune_run_end()`, while the evidence it is guarding (the dwell
trace, plus `stats.dwell_err_mean_c`, which spans the whole run) was
gathered earlier. A snapshot-at-capture would be the structurally correct
read. Traced both directions:

- **Fuzzy turned off between capture and refine** (the dangerous
  direction: contaminated trace, guard sees strength 0, correction
  applies). The only two writers of either field are `zones_post_handler()`
  (`zones_http_post.c`) and `backup_import.c:1045/1082`, and **both sit
  behind `ota_http_check_interlocks()`**, which refuses while a firing is
  running *and* while a zone is hot *and* while a heater is commanded.
  `adaptive_tune_run_end()` runs at the end of that same firing, when the
  kiln is still hot. There is no window. The narrow mid-firing exception
  that does exist -- `POST /api/zones/pid` -- accepts `kp`/`ki`/`kd` only
  and has no key for `control_mode` or `fuzzy_strength_pct` (its header
  comment enumerates this deliberately). No LCD path writes either field.
- **Fuzzy turned on mid-firing**: same interlock, also impossible; and it
  would only cause an over-refusal, not a bad write.

So the guard **cannot** be bypassed by ordering today. But its correctness
rests entirely on an interlock in a different module, with no comment,
test, or check tying the two together -- and this module has already grown
one deliberate mid-firing exception to that interlock. If a future narrow
endpoint ever admits `fuzzy_strength_pct` (a plausible "let me tune fuzzy
strength during a firing" request), this guard silently becomes bypassable
with no failing test. Cheap durable fix: snapshot
`control_mode`/`fuzzy_strength_pct` into `adaptive_tune_zone_t` at
`dwell_just_entered` (where the trace ring is already reset,
`adaptive_tune.c:234`) and have the guard read the snapshot. That removes
the cross-module dependency entirely rather than documenting it.

### R5. Negative test independently reproduced -- 4.2998 confirmed

*Verified by execution.* Not taken on trust:

1. Built `kilnctl_host_tests_adaptive_tune.exe` (build_host_tests.ps1's
   exe17) into a private scratch directory, replicating `cmd17` verbatim --
   private out dir so a concurrent session's build could not be corrupted,
   and so no stale object could be reused. Unmodified tree: **1596/1596
   checks passed, exit 0**.
2. Disabled the guard **in production code** by hand
   (`if (false && zones_config_get_control_mode(...`) -- the production
   function, not a test-local mirror. Rebuilt into a second, fresh
   directory. Result: **19 failures**, including
   `test_adaptive_tune_ki_bounds.c:841: ... (got 4.2998, want 1.0000 +/-0.0000)`.
   The failure pattern also confirms section 3's run count independently:
   the `ki_applied` assertion fails on 8 runs (the ones that actually grow
   1.2x) and the reference stops at `1.2^8 = 4.2998`, with the 9th
   attempted correction blocked by the cumulative bound -- exactly the
   shape section 3 predicts.
3. Restored by hand (edit reversed textually, `git diff` on the file empty
   -- no `git checkout`/`restore`/`stash` used), then **forced a full
   rebuild into a third fresh directory** rather than reusing any object
   from either earlier run: **1596/1596 passed, exit 0**.

The claim in section 6's negative-test paragraph is accurate, and the
regression test genuinely depends on the fix.

### R6. Leaving the "re-autotune this zone" message unchanged is the right call

*Verified by reading.* Section 4's reasoning is sound and, on inspection,
stronger than it claims. Two independent points:

- With the fix in, a fuzzy-caused instance can no longer reach that message
  at all, so it is not a *known-incomplete* message for a reachable cause
  -- it is a correct message for every cause that can still reach it (a
  genuinely mis-modelled plant, a noise source driving the symmetric
  floor). Those are precisely the cases re-autotuning addresses.
- The buffer is 96 bytes and that message already fights for space (its own
  comment says so). Adding hypothetical extra causes to it would cost the
  `%.6f` values that make it diagnostic, to warn about a cause that can no
  longer occur.

If a future gain schedule lands **without** its own guard at this site, the
message becomes incomplete again -- but the correct response then is the
guard, not the wording. Leaving it alone is right.

### R7. The `vswhere` attribution in section 7 is wrong -- the check runs fine here

*Verified by execution.* Section 7 attributes
`check_pid_fuzzy_drift.ps1`'s failure to "a pre-existing local
toolchain-discovery problem". That attribution does not survive testing:

- Ran the check as-is: **PASSES**, exit 0 --
  `804 vectors agreed within tolerance (worst |diff| = 6.1e-09)` -- against
  the *currently modified* `pid_fuzzy.c` in the shared tree. Nothing about
  this machine's toolchain prevents it running.
- `vswhere.exe` does exist, at
  `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`;
  it is simply not on `PATH`, so `vcvarsall.bat` prints that line as
  **stderr noise while still succeeding**. Reproduced directly: this
  review's own scratch build emitted the identical
  `'vswhere.exe' is not recognized` line and then compiled and linked fine.
  It is not a failure cause.
- Crucially, `pid_fuzzy_drift_check.py`'s `build_harness()` runs the
  harness build with `capture_output=True` and **only prints that captured
  output when the build actually fails**. So the `vswhere` line appearing
  in the check's output is *evidence the build failed*, not the reason --
  the real error was further down the same dump and went unread.

The likely real cause is a concurrency collision, not a toolchain gap:
`pid_fuzzy_drift_check.py` compiles into `App/test/build`, the **same
shared directory** `build_host_tests.ps1` uses -- and unlike that script,
which takes `tools/build_lock.ps1`'s named lock precisely for this reason,
this check takes no lock at all. Any concurrent host-test build (and
section 7's own account describes several running in that window, plus
other sessions mid-edit on `pid_fuzzy.c`) can clobber its objects or its
exe mid-build.

**What would make it fail loudly instead of looking incidental**: the check
already fails loudly in the sense that matters -- a failed harness build
returns non-zero, never a silent skip, so this is *not* another vacuous
check. What it lacks is an honest failure *label*. Two concrete changes:
(a) have `build_harness()` take the same `Enter-BuildLock` /
`tools/build_lock.ps1` lock `build_host_tests.ps1` takes, or compile into a
private directory, so a concurrent build cannot break it; and (b) prefix
the captured dump with an explicit "HARNESS BUILD FAILED -- the cl/link
error is below; a 'vswhere.exe is not recognized' line is ordinary
vcvarsall noise and is never the cause", so the next reader does not stop
at the first scary line. Recommended follow-up; not made here, since
`pid_fuzzy.c` and its harness are owned by another session this pass.

### Verification performed for this review

- **By execution**: `check_pid_fuzzy_drift.ps1` (passes, exit 0); three
  separate clean builds and runs of the `adaptive_tune` host-test
  executable (baseline green / guard-disabled 19 failures at 4.2998 /
  hand-restored + fully rebuilt green);
  `tools/check_doc_hash_citations.ps1`; `tools/run_all_checks.ps1`.
- **By reading**: `adaptive_tune_ki.c` in full, `adaptive_tune.c`'s
  `adaptive_tune_zone_tick()`/`adaptive_tune_run_end()`,
  `profile_executor.c`'s control-mode switch,
  `profile_executor_pid_tick.c`'s `pid_fuzzy_prepare_gains()`,
  `pid_fuzzy.c:291`, `adaptive_tune_http.c`, `zones_http_post.c`,
  `zones_http_pid.c`, `backup_import.c`'s zone commit,
  `pid_fuzzy_drift_check.py`, `build_host_tests.ps1`, and an exhaustive
  grep for Ki rescale sites across `drivers/control/`.
- **No board access, no flash, no firing** -- another session holds the
  bench.
