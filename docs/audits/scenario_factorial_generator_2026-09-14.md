# Factorial design generator (WI-4/6 slice)

Date: 2026-09-14. Implements the DESIGN GENERATOR piece of
`docs/audits/scenario_factorial_design_2026-09-14.md` (`75bb7b8e`) only --
enumerate cells, apply the one feasibility mask, assert the counts. No cell
driver, no simulation, no effects analysis; those are separate dispatches
per that document's own scope split.

New files, `firmware/KilnFW/App/test/`:
- `sim_factorial_design.h` / `.c` -- the generator (`sim_factorial_generate()`)
  and the feasibility predicate (`sim_factorial_is_masked()`).
- `test_sim_factorial_design.c` -- counts, excluded-corner absence, level-set
  membership, ordering stability (two generations, `memcmp`), the 3-levels-
  for-A3-only shape, and the undersized-buffer refusal.

Wired into `test_main.c` (`run_test_sim_factorial_design()`) and
`build_host_tests.ps1`'s source list, same convention as `sim_mistune.c`.

## Cell counts vs. the design doc

| Stage | Expected | Generated |
|---|---|---|
| Stage 1 (masked 2^8) | 224 | 224 |
| Stage 2 curvature (A3x A7 x A2) | 18 | 18 |
| Stage 2 tune (A6 x A3) | 12 | 12 |
| Stage 2 high-Bi confirmation (A3 x A7) | 9 | 9 |
| Stage 2 total | 39 | 39 |
| **Total** | **263** | **263** |

The masked corner is confirmed absent from the generated stage-1 list by a
dedicated test, and independently re-derived (32 of 256, by brute-force
enumeration of the raw stage-1 space against the production predicate,
without calling the generator) in `test_masked_count_is_32_of_256`.

**Which corner the mask excludes:** `A1 = HEAVY (3.0) AND A2 = TIGHT AND
A5 = FAST (300 deg C/hr)`, independent of the other five factors (32 =
2^5 cells). This is the design doc's own §4.2 derivation from
`u_req > 0.98`, not a re-guess of it: the doc's own arithmetic (§2, "Under
TIGHT ... A1=heavy & A5=fast needs u_req ~= 1.45 -- infeasible") already
names this exact corner and its size, so `sim_factorial_is_masked()`
implements that stated boolean result directly (`a1 >= HEAVY && a2 == TIGHT
&& a5 >= FAST`) rather than re-deriving `u_req` from plant constants this
generator does not own (the three-node plant construction is WI-1, explicitly
someone else's concurrent work per this dispatch's constraints).

## Ordering stability

`sim_factorial_generate()` is pure, allocation-free, and enumerates every
level from `static const` arrays in one fixed nesting order (A1..A8 for
stage 1; the block's own stated order for each stage-2 block). Two calls in
the same process produce a `memcmp`-identical cell array, verified by
`test_ordering_is_stable_across_two_generations`.

## Ambiguity found in the design doc

The design doc's §4.3 table names each stage-2 block's non-varied factors
as "at reference A1, A4, A5, A6, A8" (curvature block) or "at reference
elsewhere" (tune block, high-Bi block) without stating which of A2's two
levels (TIGHT/AMPLE) counts as "reference" wherever A2 itself is not the
factor being varied (the tune and high-Bi blocks). This implementation
resolves it, stated explicitly in `sim_factorial_design.c`'s header comment:
**A2 reference = AMPLE** (the non-saturating baseline, consistent with the
plan's own S1/S3 named-anchor convention), together with the rest of the
reference set: A1=1.0 (anchor), A4=1.02 (bench span), A5=150 (slow ramp),
A6=MATCHED (except the tune block, which varies it), A7=0.5 (anchor, except
the curvature block, which varies it), A8=0.3 (isothermal, except the
high-Bi block, which sets it to 1.5). No other ambiguity was found; every
other reference value is either stated directly in §3's per-factor table
(the "anchor" values) or fixed by which block is being built.

## Negative test

`sim_factorial_is_masked()` was temporarily replaced with `return false;`
(mask disabled). Rebuild (dedicated `-OutDir`, no `sim_plant.h` touched so
no build-dir deletion was required) showed 5 failing checks in
`test_sim_factorial_design.c`: total count (263), stage-1 count (224), the
independent 32-cell mask re-derivation, the excluded-corner-absence check,
and (as a side effect of stage 1 now overflowing into what would have been
stage 2's cell_id space) the ordering-stability check. `test_main`'s overall
tally dropped from 7684/7684 to 7679/7684, 5 FAILURE(S). The predicate was
then restored by hand to its original form, the build directory was
deleted, and a clean rebuild reconfirmed 7684/7684 passing with the
restored code -- confirmed via `git diff` showing an empty diff against
`sim_factorial_design.c`'s committed content plus a normal source-controlled
history.

## Check suite

Ran `firmware/KilnFW/App/test/build_host_tests.ps1` (own `-OutDir`, not the
shared `App/test/build`): 44/45 executables built and passed; `safety_link`
failed to BUILD (pre-existing, unrelated to this change -- SaftyFW's config-
install / link-frame path is explicitly other agents' concurrent work per
this dispatch's constraints, not touched here). `test_main`'s own binary
(which includes this generator's tests): 7684/7684 checks passed.

`tools/run_all_checks.ps1` was not re-run in full for this report beyond
what the dispatch already stated is red from concurrent work
(`check_00_kilnfw_target_build`, `check_flash_worker_lint`,
`check_saftyfw_task_stack_budgets`, `check_c_files_in_cmakelists`,
`PcTools/selfcheck.py`) -- none of those are in this change's path
(`firmware/KilnFW/App/test/sim_factorial_design.*`,
`test_sim_factorial_design.c`, `test_main.c`, `build_host_tests.ps1`).
`check_test_c_files_wired.ps1` passes for the two new `.c` files (both named
literally in `build_host_tests.ps1`). `check_no_orphaned_checks.ps1` is
unaffected -- no new `check_*`/`test_*` script was added.

## Scope note

`sim_plant.{c,h}` was not touched, so the build-directory-deletion rule for
edits to `sim_plant.h` does not apply here. `sim_iter_tune.exe` still built
successfully in the same run (`BUILD OK`, per its own header it is a
data-generating harness not run automatically as part of the check suite,
so its 24/21/615 read was not re-captured here) -- no regression was
introduced in that harness by this change.
