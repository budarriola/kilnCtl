# Adaptive fuzzy evaluation: progress against the plan (2026-09-14)

**Status: PARTIAL.** This session implemented and verified plan section
6.1 only (the `DT_S`-trap harness fix). Sections 3 (confidence gate), 5
(factorial arm lifting), and 7 (activity/floor/limit-cycle gates) are
**NOT implemented** — see "Remaining work" below. No board was touched, no
`.kicad_*` file was touched, no builtin schedule value was touched.

## What was done: §6.1, the ring-clamp fix

`sim_plant.h`/`sim_plant.c` (`firmware/KilnFW/App/test/`):

- `SIM_PLANT_DELAY_MAX_STEPS` raised 64 -> 128.
- Added `sim_plant_state_t::delay_truncated` (sticky bool). `sensor_pipeline_step()`
  now sets it whenever the requested delay exceeds ring capacity, instead of
  silently clamping.
- `sim_factorial_driver.c`'s `run_cell_firing()` and `sim_scenarios.c`'s
  `run_firing()` both now check `pstate.delay_truncated` after a run and
  **refuse the cell** (non-zero-signalling refusal path, same mechanism as
  the existing NaN/bounds refusals), naming the offending `sensor_delay_s`
  and the ring's capacity in seconds.

### Regression proof (mandatory per plan, done, not asserted)

Built `sim_factorial_driver.exe` twice from the real MSVC toolchain via
`run_sim_factorial.ps1 -Shards 1 -SkipDeterminism`:

- **Before**: from `C:\wt\checkbuild_origin_kilnfw` (a clean worktree
  already sitting at `main` HEAD `83e04785`, unmodified).
- **After**: from the working tree with the §6.1 changes above.

Both runs: `rows_emitted=789 cells_with_a_refusal=0`. The two
`factorial_of1.tsv` outputs are **byte-identical**
(md5 `de5743fda54bab70b61d32b7d6d06722` both sides, `cmp` exit 0). This
satisfies the plan's "bench cells must come out bit-identical" requirement
— and more strongly, ALL 789 existing rows (bench- and kiln-span alike) are
identical, because `sim_factorial_driver.c` as it stands today holds
`sensor_delay_s` at the bench value (40.3 s) for every cell (per its own
header comment: "Dead time is held at the bench value across both spans");
the kiln-scaled 76.9 s dead time from plan §6 has not been wired into the
driver yet (see "Remaining work"), so the new 128-step capacity and the
`delay_truncated` refusal path are currently **dormant but proven safe** —
they change nothing for any cell that exists today, and are ready for the
§6 kiln-scaling change to actually exercise them.

## Remaining work (plan §11, steps 2-5) — NOT done this session

This is a large, multi-part implementation and was not completed in this
pass. Left in the state described so a follow-on session does not have to
re-derive scope:

1. **§6 kiln-scaled dead time/tau are not yet wired into
   `sim_factorial_driver.c`.** Today's driver holds `sensor_delay_s`/`tau`
   at bench values for kiln-span (`A4=KILN_SPAN`) cells (a documented
   simplification in the current file, not this session's doing). Wiring
   in the 76.9 s / 28.6 s kiln-scaled values is a prerequisite for the
   ring change above to matter, and is itself plan work, not incidental.
2. **§3's confidence gate (N1/N2/N3) is unbuilt.** No changes were made to
   `pid_fuzzy.c`, `profile_executor_pid_tick.c`, or `adaptive_tune.c`. The
   `cap_L` dead-time authority cap, the `c`-based rise-limited authority
   schedule, and the in-firing oscillation zero-crossing detector all
   remain to be written, each with its own negative test against the
   production function (not a test-local copy), hand-restored and rebuilt
   per the plan's standing procedure.
3. **§5's two new factorial arms (`A_PID_AT`, `A_FUZZY_AT`) are unbuilt.**
   `sim_factorial_driver.c` still has exactly the three single-firing arms
   (`CELL_ARM_PID`, `CELL_ARM_FUZZY50`, `CELL_ARM_STATIC_MATCHED`). Lifting
   `sim_scenarios_adaptive.c`'s 9-firing chain mechanics (its zones-config
   fake, run-end wiring, per-firing print) into this driver, per plan
   §5.1's explicit instruction not to write a fourth copy of the tick loop,
   remains to be done.
4. **§7's three mechanical gates (activity, floor-identity, limit-cycle
   regression) are unbuilt** — there is nothing yet to gate, since the
   adaptive arms don't exist.
5. **No factorial run against the adaptive arms has been performed**, so
   §8/§9's tallies (`D_adapt_combo`, per-cell improved/degraded counts,
   the four removal criteria, the seven pinned limit-cycle cells) are
   **not yet answerable**. Nothing in this document should be read as
   reporting those results — they do not exist yet.

## Note on plan logic

No disagreement found with the plan's reasoning during the portion
implemented (§6.1) — the dead-time authority cap rationale (§1.4), the
bit-for-bit floor requirement (§2), and the ring-clamp fix design (§6.1)
are all internally consistent and the regression proof came out exactly as
the plan predicted (bench-span rows unaffected). No opinion is offered yet
on §3's confidence-gate design or §8/§9's decision rules, since they were
not exercised this session.

## Housekeeping confirmation

No board was flashed, no heating run was started, no `debug_*` tool was
called, no `.kicad_*` file was touched, no `target_c`/`ramp_c_per_hr`/
`dwell_min`/`segment_count` builtin schedule value was touched. The
pre-existing uncommitted `kiln_package.c` WIP in this working tree (another
session's, breaks the KilnFW target build with two `-Werror` errors) was
left untouched and is not related to this change — none of this session's
edits touch that file or KilnFW production code at all (only
`firmware/KilnFW/App/test/sim_plant.h`, `sim_plant.c`,
`sim_factorial_driver.c`, `sim_scenarios.c`, all host-test-only fixtures).
