# Scenario simulation implementation — progress, 2026-09-14

Implementing `docs/SCENARIO_SIMULATION_PLAN.md` (commit `32b10a34`). Work items
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
