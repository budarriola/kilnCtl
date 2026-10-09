# sim_iter_tune bar-mismatch triage, 2026-09-14

## The claim

An agent implementing WI-1 of `docs/SCENARIO_SIMULATION.md` (opt-in
three-node plant model, landed as HEAD commit `7729f3c8`, `git cat-file -t
7729f3c8` -> `commit`) reported two things:

1. With WI-1 applied, `firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`
   reported **87 / 83 / 490** instead of the pinned **24 / 21 / 615**.
2. Rebuilding `sim_iter_tune.exe` from `sim_plant.c`/`.h` reverted to a
   pre-WI-1 state, against the tree's current `firing_score.c` /
   `firing_compare.c` / `iter_tune.c`, crashed with an access violation
   before printing anything, reproduced three times.

It concluded the pinned baseline itself was broken, and pointed at recent
`firing_score.c` commits `560cffe0` and `d41da85f` (both confirmed real
commits via `git cat-file -t`) as the cause.

## Why that diagnosis was doubted

- Other agents ran the full `tools/run_all_checks.ps1` at 94/94 green,
  including `check_sim_iter_tune_bars.ps1`, both before and after the two
  named `firing_score.c` commits.
- WI-1 added fields to `sim_plant.cfg_t`/`sim_plant_state_t` (both appended
  at the end of each struct -- `sim_plant.h`'s WI-1 comment block says so
  explicitly), changing `sizeof()` for both. Any two translation units built
  against different versions of that header and linked together produce
  undefined behaviour: silently wrong values (consistent with 87/83/490
  being garbage rather than a different, meaningful verdict) or a crash
  (consistent with the reported access violation), with no diagnostic
  naming the real cause.
- The repo had a poisoned-binary incident the same night (`8a12521b`) from
  exactly this class of bug: a stale build artifact surviving a source
  change.
- The described repro method for finding #2 -- "rebuild `sim_iter_tune.exe`
  from `sim_plant.c`/`.h` reverted to a pre-WI-1 state, against the tree's
  CURRENT `firing_score.c`/`firing_compare.c`/`iter_tune.c`" -- is by
  construction a deliberate mix of an old header/source with new
  consumers. That is not a defect in `firing_score.c`; it is the exact ABI
  mismatch this triage set out to test for.

## Step 1: clean-room the baseline

Deleted every build/object directory used by the two harnesses
(`firmware/KilnFW/App/test/build` and
`firmware/KilnFW/App/test/build_sim_iter_tune_bars_obj`), then ran
`check_sim_iter_tune_bars.ps1` fresh (it already builds `sim_iter_tune.exe`
from a single `cl` invocation naming every source file explicitly --
`sim_iter_tune.c`, `sim_plant.c`, `pid.c`, `heater_output.c`,
`zone_coupling_solve.c`, `firing_score.c`, `firing_compare.c`,
`iter_tune.c` -- so it is a full recompile every run, never an incremental
link against pre-existing `.obj` files).

**Result, clean-room, at HEAD (with the guard described below already
added):**

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
A1 bar: PINNED KNOWN-FAILURE CEILING <= 24/660 (3.6364%) -> PASS
A2 bar: worse-by-more-than-one-floor <= 1% -> PASS
A5 bar: termination within budget >= 95% -> PASS
A6 bar: 0 cage violations -> PASS
OVERALL: PASS
```

**Exactly 24 / 21 / 615 -- the pinned baseline is intact.** The 87/83/490
figure and the access-violation crash were both artifacts of stale or
mismatched build objects, not a defect in `firing_score.c`,
`firing_compare.c`, or the pin itself. No bisection was needed (step 1
already produced the pinned numbers), and `firing_score.{c,h}` /
`firing_compare.{c,h}` were left untouched, per this triage's scope.

## True cause

`sim_plant_cfg_t`/`sim_plant_state_t` grew across a header the reporting
agent's investigation workflow did not keep in lock-step with every `.c`
file that consumes them. Passing struct pointers across a translation-unit
boundary compiled against different header versions is undefined behaviour
in C -- there is no language-level check that would have caught it, and
nothing about `firing_score.c`'s own commits was responsible.

## Fix: an ABI-freshness guard, not a re-pin

The bar was never moved -- nothing needed restoring, and the ratchet guard
(pin only ever tightens by hand) was never touched. Per instruction 4, no
re-pin was made or considered.

Per instruction 5, added a guard so a future instance of this class fails
loudly instead of printing garbage or crashing silently:

- `firmware/KilnFW/App/test/sim_plant.h`: declared
  `sim_plant_assert_abi_fresh_impl(size_t caller_state_size, size_t
  caller_cfg_size, const char *caller_file)` and the
  `SIM_PLANT_ASSERT_ABI_FRESH()` macro, which passes the CALLING
  translation unit's own `sizeof(sim_plant_state_t)`/`sizeof(sim_plant_cfg_t)`.
- `firmware/KilnFW/App/test/sim_plant.c`: defined the impl. It compares the
  caller's sizes against the sizes `sim_plant.c`'s OWN compilation computes
  for the same two structs. Any mismatch -- a stale `.obj`, an incremental
  link against a leftover object file, or a partial manual revert of
  `sim_plant.c`/`.h` while other sources kept the post-WI-1 header -- means
  at least one linked object was built against a different `sim_plant.h`.
  On mismatch it prints both sizes and `abort()`s instead of letting the
  program run on a corrupted struct.
- Wired into the three host-side entry points that link `sim_plant.c`
  together with other `.c` files touching these structs:
  `sim_iter_tune.c`'s `main()`, `sim_wide_temp_sweep.c`'s `main()`, and
  `test_sim_plant_three_node.c`'s `run_test_sim_plant_three_node()`.

## Negative test

Built a standalone harness (scratch file, not committed) that included
`sim_plant.h`, linked against the real `sim_plant.c`, and called
`sim_plant_assert_abi_fresh_impl(sizeof(sim_plant_state_t) + 4,
sizeof(sim_plant_cfg_t), __FILE__)` -- simulating a caller compiled against
a header that disagreed on `sim_plant_state_t`'s size. Confirmed it printed
the FATAL diagnostic naming both sizes (284 vs. 280) and aborted
(process exit `-1073740791`, i.e. an abort trap) rather than continuing
silently. Restored to the real, matching call afterward (i.e. deleted the
scratch negative-test file; no production file was altered for the negative
test itself) and rebuilt/reconfirmed `check_sim_iter_tune_bars.ps1` still
reports the pinned 24/21/615 with the guard compiled in.

## Full check tally

`tools/run_all_checks.ps1`, clean-room, at HEAD (with this triage's changes
applied): **92 passed, 0 skipped, 2 failed**:

- `firmware\KilnFW\App\test\check_00_kilnfw_target_build.ps1` -- ESP-IDF
  `idf.py build` (ninja/cmake) failure. This project's own kilnlink/
  link_frame stack-margin work is mid-flight in this shared tree (build log
  shows `kilnlink_stack_margin.h`/`link_frame.h` additions in progress) --
  `kilnlink_version.h`, `safety_link*.{c,h}`, and
  `firmware/CommonFW/CMakeLists.txt` are explicitly flagged as owned by
  another agent for this session and were not touched here.
- `firmware\KilnFW\App\test\check_wire_protocol_fingerprint.ps1` -- same
  in-flight kilnlink/link_frame work (new `LINK_FRAME_GET_STACK_MARGIN_CMD`/
  `LINK_FRAME_STACK_MARGIN_CMD` fingerprint entries).

Both failures are outside this triage's scope (`sim_plant.{c,h}`,
`firing_score.{c,h}`, `firing_compare.{c,h}`) and unrelated to the WI-1/
sim_iter_tune question triaged here; not touched.

`build_host_tests.ps1` separately showed one pre-existing, unrelated link
failure (`test_safety_link_compile.obj: unresolved external symbol
kilnlink_stack_margin_decode`/`kilnlink_get_stack_margin_encode`) from the
same in-flight safety_link/kilnlink work; `sim_iter_tune.c`,
`sim_wide_temp_sweep.c`, and the `test_sim_plant_three_node` host test all
built and linked cleanly with the new guard in place.

## Bottom line

- Clean-room triple: **24 ACCEPT / 21 REJECT / 615 INSUFFICIENT** -- matches
  the pin exactly.
- Crash: did not reproduce from a clean build; only reproduces when
  translation units are deliberately or accidentally built against
  mismatched `sim_plant.h` versions.
- Bisection: not needed -- step 1 already restored the pinned numbers.
- True cause: stale/mismatched build objects (an ABI skew across
  `sim_plant.h` versions), not a `firing_score.c`/`firing_compare.c` defect.
- Fix: no production logic changed in `firing_score.c`/`firing_compare.c`/
  `sim_plant.c`'s existing behaviour; added an ABI-freshness runtime guard
  (`SIM_PLANT_ASSERT_ABI_FRESH()`) so a future stale-object mismatch aborts
  loudly, naming the mismatched sizes, instead of silently corrupting a
  verdict or crashing without explanation.
- Guard negative-tested: confirmed it fires and aborts on a deliberately
  wrong size, then confirmed the real (matching) build still passes at
  24/21/615.
- `tools/run_all_checks.ps1`: 92/94 green; the 2 failures are pre-existing,
  unrelated in-flight kilnlink/safety_link stack-margin work owned by
  another agent this session.
