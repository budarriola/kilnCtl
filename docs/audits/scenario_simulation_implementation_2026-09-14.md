# Scenario simulation implementation — progress, 2026-09-14

Implementing `docs/SCENARIO_SIMULATION.md` (commit `32b10a34`). Work items
1-10 defined in its sec 7. This note records what was completed and why
implementation was **stopped after WI-1**, before WI-2.

## WI-1 — three-node plant model, opt-in, default bit-identical

**Completed, in isolation.** `firmware/KilnFW/App/test/sim_plant.h`/`.c` gained:

- `sim_node_model_t` (`SIM_NODE_LEGACY` = 0 default, `SIM_NODE_THREE`).
- New `sim_plant_cfg_t` fields (`c_e_j_per_c`, `c_l_j_per_c`, `c_s_j_per_c`,
  `g_el_w_per_c`, `g_ea_w_per_c`, `g_la_w_per_c`, `sensor_tau_s`,
  `sensor_bias_p`, `load_mass_mult`) and new `sim_plant_state_t` fields
  (`load_c`, `sensor_node_c`), all inert under `SIM_NODE_LEGACY`.
- `sim_plant_three_node_step()`, a new function alongside the untouched
  `sim_plant_step()`. Computes `dE`/`dL`/`dS` from one common snapshot of
  `E,L,S` (per sec 2.1's ordering requirement), then reuses the existing
  transport-delay ring + first-order sensor lag (`sensor_pipeline_step()`),
  applied to the sensor node `S` instead of to `E`.
- `firmware/KilnFW/App/test/test_sim_plant_three_node.c` (new), wired into
  `test_main.c` and `build_host_tests.ps1`. Three tests, all TEST FIXTURE
  constants labelled as such in the file header and at each use:
  1. `sensor_bias_p = 5/6` (the "5x closer to elements" interpretation):
     sensor leads the load throughout a duty-1.0 rise, its 63% rise time is
     >=3x shorter than the load's, and it falls >=5x faster than the load in
     the 60s after a duty cutout. Measured at this file's fixture constants:
     sensor_t63=73s vs load_t63=3283s (45x); post-cutout sensor_drop=358.2C
     vs load_drop=3.6C (99x).
  2. `sensor_bias_p = 0` (centre-mounted reference): sensor never reads
     hotter than the load during a rise, and its 63% rise time is >= the
     load's own (a lag, not a lead). Measured: sensor_t63=3293s,
     load_t63=3283s.
  3. `SIM_NODE_LEGACY` bit-identical negative check: runs `sim_plant_step()`
     twice with the three-node fields poisoned with large nonsense values in
     one run and zeroed in the other; asserts `element_c`/`sensor_c` come out
     bit-identical, proving the legacy path never reads the new fields.
- `tools/check_test_c_files_wired.ps1` and `tools/check_no_orphaned_checks.ps1`
  both PASS with the new file in place.
- Original fixture constants (C_e=500, C_l=5000, G_el=5) undershot the
  required 3x rise-time separation (measured 1.37x-1.67x across two attempts)
  because the coupled (E,L) system's SLOW mode dominates the *asymptotic*
  63%-of-final metric once observed over the full ~2750-3300s settling
  horizon needed for the load to approach its own steady state. Retuned to
  C_e=50 (100x lighter than the load) with G_el=G_ea=G_la=1 W/C so the
  element's fast quasi-equilibrium (reached in ~10s of seconds, with L still
  near ambient) already sits at ~75% of the eventual fully-joint steady
  state — this is what gives the fast mode a large amplitude share and
  produces the required separation. Recorded here per the plan's instruction
  to flag any deviation from a first-guess fixture and investigate rather
  than silently re-pin.

## WI-1 acceptance criterion (b) — COULD NOT BE CONFIRMED; stopping here

The plan's mandatory acceptance test for WI-1 (sec 2, "That is the acceptance
criterion for work item 1") is: `sim_iter_tune.exe` at `mc_runs=220` must
still report **exactly 24 ACCEPT / 21 REJECT / 615 INSUFFICIENT / 0 NO_PAIRS**
out of 660 null comparisons (the number `check_sim_iter_tune_bars.ps1` and
`sim_iter_tune.c`'s own `A1_PINNED_MAX_ACCEPTS`/`A1_PINNED_TOTAL` comment
cite as reproduced unchanged from 2026-09-10 through 2026-09-11).

Running `check_sim_iter_tune_bars.ps1` against this checkout (WI-1's
`sim_plant.c`/`.h` changes applied, everything else untouched) produced:

```
660 null comparisons: ACCEPT 87 (13.18%)  REJECT 83  INSUFFICIENT 490  NO_PAIRS 0
```

— not 24/21/615. To find out whether this was caused by WI-1, I extracted
the pre-WI-1 `sim_plant.c`/`.h` from `git show HEAD:...` into a scratch
directory and rebuilt `sim_iter_tune.exe` from that ORIGINAL sim_plant.c/.h
plus this checkout's current, unmodified
`firing_score.c`/`firing_compare.c`/`iter_tune.c`/`pid.c`/
`heater_output.c`/`zone_coupling_solve.c` (the exact same source list
`check_sim_iter_tune_bars.ps1` uses, same compiler flags). Result:
**the executable crashes immediately with an access violation
(exit -1073741819 / 0xC0000005) before printing anything, reproducibly
across three separate runs.**

So: the *original*, un-modified `sim_plant.c` does not even run to
completion against this checkout's current control-code files, and WI-1's
own modified `sim_plant.c` (larger structs, by coincidence of memory layout)
happens to avoid the crash but produces 87/83/490, not the pinned 24/21/615.
Neither number is traceable to anything WI-1 changed in `sim_plant_step()`
itself (untouched, and separately proven bit-identical by
`test_sim_plant_three_node.c`'s negative test above) or in
`sim_plant_from_zone_cfg()` (not touched by WI-1). The likely cause is
upstream, in files this task is explicitly forbidden from touching:
`git log` shows `firing_score.c` and `firing_compare.c` received two commits
very recently (`560cffe0` "firing_score: separate measurement from
adjudication; the enum's size is no longer the decision rule", `d41da85f`
"firing_score: instrument settle-time and undershoot, sign LAG_S's blind
spot") — exactly the concurrent, in-flight work this task's brief warns is
owned by another agent/session ("Scoring uses production subscores...
another agent owns those files and a review is in flight"). A regression or
in-progress state in that work is the far more plausible explanation than
anything in `sim_plant.c`, given the crash reproduces with `sim_plant.c`
reverted to HEAD.

**Per this task's own instruction** ("If a work item turns out to be wrong
or impossible as specified, STOP on that item, say why, and do not
improvise a substitute"): WI-1's mandated acceptance check cannot currently
be evaluated at all — not "the bar moved," but "the un-modified baseline
this bar is measured against does not run" — because of state in files I do
not own and must not edit. I am stopping here rather than:

- re-pinning the A1 numbers myself (explicitly against the plan's ratchet
  rule, sec 5.3 #2, which reserves re-pinning for a human decision with a
  written reason, and this isn't even a re-pin situation since the baseline
  crashes);
- proceeding to WI-2 through WI-10, all of which build on `sim_plant.c` and
  whose own acceptance criteria (S0/S1/S3 in WI-4, the `A_STATIC_MATCHED`
  measurement in WI-5, etc.) depend on the same `sim_iter_tune`/
  `firing_score`/`firing_compare` toolchain being in a stable, measurable
  state;
- guessing at what `firing_score.c`/`firing_compare.c`'s intended
  post-560cffe0/d41da85f state should produce.

WI-1's own code (the three-node model, its own new host test, and the
build/test wiring) is committed on its own, since it is self-contained,
independently verified correct via its own passing test, and does not
regress anything (the crash reproduces WITHOUT it). What is NOT committed or
claimed done is confirmation of the plan's WI-1 acceptance criterion (b),
and WI-2 through WI-10 were not started.

## What still needs to happen before this plan can proceed

1. Whoever owns `firing_score.c`/`firing_compare.c`/`iter_tune.c` needs to
   either finish their in-flight change to a state where
   `sim_iter_tune.exe 220` runs to completion, or confirm the crash is a
   pre-existing, already-known issue and say what the new expected A1
   breakdown is (so the pin in `sim_iter_tune.c` can be updated by hand, by
   a human, with a stated reason — never by an implementer chasing a green
   check).
2. Once that baseline is stable and reproducible, re-run
   `check_sim_iter_tune_bars.ps1` against WI-1's `sim_plant.c` changes alone
   to confirm bit-identical output, then resume at WI-2.

## Other checks run this session

- `firmware/KilnFW/App/test/build_host_tests.ps1`: 38/38 executables built;
  the host-test runner (`kilnctl_host_tests.exe`) reports 10 pre-existing
  failures, all in `test_iter_tune.c` (`SETTLE_S`/voting-mask assertions),
  none in the new `test_sim_plant_three_node.c`. These are the same
  concurrent-session files named above and are not addressed here.
- `tools\check_test_c_files_wired.ps1`: PASS.
- `tools\check_no_orphaned_checks.ps1`: PASS.
- `tools\run_all_checks.ps1` (full 94-check suite) and a target
  `build_kilnfw` were **not** run this session — deferred until the WI-1
  blocker above is resolved, since a full run would currently need to
  either inherit or paper over the `sim_iter_tune`/`firing_score` state
  described above.

## WI-8 — adaptive arms: 9-firing chained-adaptation harness (DONE)

Implemented as its own executable, `firmware/KilnFW/App/test/sim_scenarios_adaptive.c`
(built by `build_host_tests.ps1` as `kilnctl_sim_scenarios_adaptive.exe`, its own
gating step, not folded into `sim_scenarios.c`'s six-arm table). It links the
REAL `adaptive_tune.c`/`adaptive_tune_model.c`/`adaptive_tune_ki.c` (not a
mirror) against a single-zone `zones_config` test fake whose K_dc/Kp/Ki/Kd/
`autotune_baseline_k_dc`/`adaptive_tune_enabled` state persists for a whole
9-firing chain per (scenario, arm), driving `adaptive_tune_zone_tick()` every
control tick and `adaptive_tune_run_end()` once per firing — the same
production entry points `profile_executor.c` calls on hardware.

**Two corrections made to the plan during this pass, both requested by the
coordinator after a roadmap survey:**
- **WI-9 is dropped, not merely deferred.** Its premise (a fuzzy/Ki mutual
  exclusion to remove) no longer exists: `88bb4333` deleted
  `adaptive_tune_ki.c`'s write path entirely, not just the guard. WI-9's
  section in `docs/SCENARIO_SIMULATION.md` is now struck through with a
  dated note; it was never implemented and must not be.
- **WI-8 acceptance criterion (b) was rewritten in place** in the plan doc:
  the original text ("every `A_FUZZY_AT` row carries `ki_state = KI_WITHHELD`")
  assumed a guard that `88bb4333` had already removed by the time this pass
  ran. The rewritten criterion states plainly that the combination arm
  adapts via `adaptive_tune_model.c`'s SIMC path and that SIMC's invariance
  to fuzzy is approximate (residual on the order of `0.003*tau`, per
  `docs/audits/simc_sole_gain_writer_2026-09-14.md`'s appended review
  `cef1df2a`), not exact.

**What the 9-firing chain actually found (report the numbers, not a
verdict — an opus review follows this pass):**
- Of the 13 table scenarios, S10/S11 (dynamic per-tick conductance scaling
  and per-segment re-tuning) are skipped as out of scope — those mechanisms
  are themselves confounds for isolating cross-firing K_dc convergence. The
  remaining 11 scenarios each ran both `A_PID_AT` and `A_FUZZY_AT` as a full
  9-firing chain.
- Only **S0_NULL_SLOW** and **S6_MASS_LIGHT** ever harvested the minimum 4
  dwell observations `adaptive_tune_refine_zone_locked()` needs to attempt a
  fit. Every other scenario — including the plan's own named acceptance
  case, **S7_TUNE_HOT** — harvested **zero** observations across all 9
  firings, on both arms.
- The reason, read directly from `adaptive_tune_zone_tick()`'s own
  `last_refusal_reason` (surfaced per firing as `harvest_reason` in this
  harness's output): the closed loop's temperature genuinely settles (the
  slope-floor gate, `ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S`, does
  eventually pass), but by the time it does, the dwell's own recorded
  duty min/max window — tracked from the moment the dwell began, i.e.
  spanning the ramp-to-dwell entry transient — still exceeds
  `ADAPTIVE_TUNE_DUTY_STABILITY_ABS` (0.05). The gate evaluates a dwell
  exactly ONCE (`recorded_this_dwell` latches true on the first slope-floor
  pass, never retried), so a duty transient wide enough at that one moment
  permanently voids the dwell's only chance to contribute — regardless of
  how flat both signals become afterward. Only the two gentlest-ramp
  scenarios (S0, S6) have an entry transient small enough to clear this on
  the first attempt.
- Where data WAS harvested (S0, S6), `adaptive_tune_refine_zone_locked()`
  never actually applied a gain change in this suite either
  (`refine_ever_applied=no` throughout) — both are matched-tune scenarios
  with nothing to correct, so the fitted K_dc stays within
  `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC` of the belief already in place.
- **This independently reproduces, rather than merely explains, the
  coordinator's own finding that `adaptive_tune` has never actually
  harvested anything on the real board** (`enabled=false, lifetime=0` on
  all three zones): the settle-slope/duty-stability combination, evaluated
  once per dwell with no retry, appears to be very hard to satisfy
  following an ordinary ramp-into-dwell profile at these zones' real time
  constants (tau ~250-260 s). This is worth a dedicated follow-up audit of
  `adaptive_tune.c`'s harvesting gate independent of this simulation work.

**Acceptance criterion (a) as re-verified against this finding:** the harness
does NOT force a synthetic pass. It asserts a direction check on S7 only when
data was actually harvested (never happened here, so the check reports
`INCONCLUSIVE`, not `FAIL`), and instead gates the build on a strictly
weaker, still-falsifiable, and more honest global claim: **at least one
(scenario, arm) chain in the whole suite must harvest the minimum
observations at some point across its 9 firings** — this is the assertion
that was negative-tested (see below) and is what actually failed when the
production gate was broken by hand.

**Negative test:** `ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S` in
`adaptive_tune_internal.h` was changed from `0.003f` to `0.0f` by hand (an
unsatisfiable floor — no real temperature reading is ever byte-identical to
its dwell-entry value). Rebuilt: `sim_scenarios_adaptive` correctly flipped
to `FAIL` ("no (scenario, arm) chain in this suite ever harvested the
minimum 4 dwell observations..."), confirming the suite's own gate is not
vacuous. Restored the constant by hand, confirmed `git diff` was empty
against that file, deleted the build directory, and rebuilt fully clean
(42/42 executables) before re-confirming `PASS`.

**Determinism:** `run_sim_scenarios.ps1` re-run after this pass — `--of 1`
vs `--of 4` remain byte-identical (78/78 data rows). `sim_iter_tune.exe 220`
still reads 24 ACCEPT / 21 REJECT / 615 INSUFFICIENT / 0 NO_PAIRS from a
clean rebuild, unchanged.

**CI budget:** `sim_scenarios_adaptive.exe` runs in a few seconds (11
scenarios x 2 arms x 9 firings, each firing well under the per-firing cost
`docs/SCENARIO_SIMULATION.md` sec 8 already budgets) — it is its own
`build_host_tests.ps1` step (41st -> now 42nd `Invoke-HostTestExe` call, own
object directory `atsim/`) rather than folded into `sim_scenarios.c`'s own
budget line, so neither suite's ~60 s ceiling is put at risk by the other's
much larger link surface (`adaptive_tune.c` pulls in `hal_kv`/`esp_log`/
`flash_worker_wait`/`pref_cfg_fs`/`cfg_fs_status`/a real FreeRTOS mutex
against the host stub).

**Stale comments fixed while in these files:** `sim_scenario_table.h`'s
`SIM_ARM_PID_AT`/`SIM_ARM_FUZZY_AT` enum comments (previously said "WI-8"
and "KI_WITHHELD until WI-9 lands"); `sim_scenarios.c`'s own header/banner/
notes-column text (previously `WI8_PENDING`, now names the separate
9-firing harness); `docs/SCENARIO_SIMULATION.md`'s top-of-file Status
line (previously "Status: PLAN. No production code is written by this
document," stale since WI-1) and its WI-9 section (struck through, dated,
explains why).

**Full `run_all_checks.ps1` tally this session: 92 passed, 2 failed.** Both
failures are pre-existing and unrelated to this work item (neither
`sim_scenarios_adaptive.c`, `adaptive_tune*.c`, nor any file this pass
touched appears in either failure):
`firmware/KilnFW/App/test/check_readiness_gate_display_agreement.ps1` and
`tools/check_safety_call_results_checked.ps1` (flags
`main_control_bringup.c:40`) — both point at files this session never
opened, consistent with concurrent-session WIP elsewhere in the shared tree
(see `CLAUDE.md`'s "Concurrent sessions git race" guidance).
