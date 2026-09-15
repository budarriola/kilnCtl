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

1. ~~**§6 kiln-scaled dead time/tau are not yet wired into
   `sim_factorial_driver.c`.**~~ **STALE — this was already done by
   `4891a6fb`**, which landed `KILN_L0_S 76.9f` / `KILN_SENSOR_TAU_S 28.6f`
   in the driver. The item above was written without re-checking the file
   and was wrong when written. Consequence of that commit: three kiln-span
   cells (`A2=TIGHT`, `A6=HOT`) legitimately refuse on bounds-exceeded,
   giving **260/263 usable cells** — expected, not a defect to "fix".
2. **§3's confidence gate (N1/N2/N3) is unbuilt.** No changes were made to
   `pid_fuzzy.c`, `profile_executor_pid_tick.c`, or `adaptive_tune.c`. The
   `cap_L` dead-time authority cap, the `c`-based rise-limited authority
   schedule, and the in-firing oscillation zero-crossing detector all
   remain to be written, each with its own negative test against the
   production function (not a test-local copy), hand-restored and rebuilt
   per the plan's standing procedure.
3. ~~**§5's two new factorial arms (`A_PID_AT`, `A_FUZZY_AT`) are
   unbuilt.**~~ **DONE — see "§5 implementation (this session)" below.**
4. **§7's three mechanical gates: one of three built.** The
   **floor-identity** gate is built and enforced (non-zero process exit,
   negative-tested). The **activity** gate and the **limit-cycle
   regression** gate on the seven pinned cells are **not** built — §7's raw
   instrumentation for both is now emitted per firing (`ADAPTIVE_DIAG`
   lines), but nothing adjudicates it yet. The driver still reports
   numbers and never verdicts.
5. **No factorial run against the adaptive arms has been performed**, so
   §8/§9's tallies (`D_adapt_combo`, per-cell improved/degraded counts,
   the four removal criteria, the seven pinned limit-cycle cells) are
   **not yet answerable**. Nothing in this document should be read as
   reporting those results — they do not exist yet.

## §5 implementation (this session)

Plan §5's two adaptive arms are lifted into `sim_factorial_driver.c`. No
§8/§9 campaign was run and **no tally was computed or is reported here**,
per the dispatch's explicit scope limit.

### What was built

- Arm enum extended to five. `CELL_ARM_SINGLE_COUNT` bounds the original
  three-arm loop, so adding adaptive arms cannot change how the first three
  are driven.
- **One tick loop, not two** (plan §5.1). `run_cell_firing()` gained a
  `cell_adaptive_ctx_t *ad` parameter; it is `NULL` for the three original
  arms and every new line of per-tick work sits inside an `if (ad)` guard.
  No fourth copy of the tick loop was written.
- `run_cell_chain()` runs a chain of **nine** sequential firings per
  adaptive arm, carrying `adaptive_tune` state across them, driving the
  real `adaptive_tune_zone_tick()` / `adaptive_tune_run_end()` seam against
  a single-zone `zones_config` test fake lifted from
  `sim_scenarios_adaptive.c`. Sensor readings are `q1()`-quantized to
  MAX31856 resolution, as that file and `test_adaptive_tune.c` do.
- Row shape: adaptive rows use the **same 23 columns**, with the firing
  index in the arm column (`A_PID_AT_F1` .. `A_FUZZY_AT_F9`). §7's extra
  instrumentation goes on a **separate `ADAPTIVE_DIAG` line** precisely so
  the pre-existing rows keep their exact byte shape. Per-CELL counting is
  therefore unaffected: the cell id is still column 1.
- **Refusal discipline** (per `3b6b9d31`): a chain that cannot start emits
  `CELL_REFUSED <cell> <arm>_F1` .. `_F9`; a chain that dies at firing *k*
  emits `CELL_REFUSED` for *k*..9. No firing can be silently dropped.
  §6.1's sticky `delay_truncated` REFUSE path is reached by the adaptive
  arms through the same `run_cell_firing()` code that serves the others.
- `run_sim_factorial.ps1` gained the nine additional compilation units the
  `adaptive_tune` link surface needs, and its determinism comparison now
  covers `ADAPTIVE_DIAG` lines as well as data rows.

### The §3 confidence gate: what was ASSUMED

The gate was being implemented concurrently by another agent and its files
were **not touched**. This driver **links the real
`pid_fuzzy_confidence.c`** and keeps **no local mirror** of the schedule (a
local mirror is how this project has previously shipped arms that silently
stopped tracking production). The assumed interface, taken from the
in-flight `pid_fuzzy_confidence.h` on disk, is:

- `float pid_fuzzy_confidence_cap_l(float dead_time_s, float tau_s)`
- `uint8_t pid_fuzzy_confidence_strength_pct(uint8_t c, float cap_l)`
- `pid_fuzzy_oscillation_state_t` carrying a `tripped_this_firing` field,
  plus `pid_fuzzy_oscillation_reset()` and
  `bool pid_fuzzy_oscillation_tick(state *, float error_c, float dt_s)`
- `adaptive_tune_get_fuzzy_confidence_c(zone)` and
  `adaptive_tune_fuzzy_confidence_floor_now(zone)`

and the call ORDER mirrors production `pid_fuzzy_prepare_gains()`:
harvest-freeze first (`dwelling && adaptive_tune_get_enabled(zi)` forces
strength 0 — the shipped "Option B" contract), then the gate, then the
oscillation backstop. **If that interface changes, this file must change
with it, and it will fail to compile rather than drift — by design.**

**Build dependency, resolved:** `pid_fuzzy_confidence.c/.h` were untracked
(the other agent's in-flight work) while this driver was being written, but
landed as `ee55af58` before this commit, and the working tree was clean for
both files when every number below was measured — so the measurements are
against the COMMITTED gate, not a private snapshot. `run_sim_factorial.ps1`
remains deliberately unwired from `build_host_tests.ps1` and CI, so the
blast radius is a manual target only.

### What was proven

- **Inertness for the three existing arms.** A clean-HEAD worktree's
  `factorial_of1.tsv`, restricted to `A_PID`/`A_FUZZY50`/`A_STATIC_MATCHED`,
  is **byte-identical** to the changed tree's: 780 rows, md5
  `1be73e04757beb9c9499e05875057f8f` on both. Re-verified after a forced
  full rebuild following the negative test below.
- **Sharding still consistent.** `--of 1` vs `--of 4`: PASS, 10140 rows
  compared byte-identical (780 existing-arm + 4680 adaptive data rows +
  4680 `ADAPTIVE_DIAG` lines).
- **Floor identity holds now, not "once the gate lands".** Firing 1 of
  `A_FUZZY_AT` is bit-identical to firing 1 of `A_PID_AT` for every cell,
  asserted by `memcmp` over the whole result struct with a non-zero process
  exit. This works because `c` starts at 0 and the oscillation detector is
  ticked for **both** adaptive arms, keeping their adaptation state
  identical while strength is 0.
- **The floor-identity gate was negative-tested.** Forcing `gated = 50` for
  the fuzzy arm made the run print `FLOOR_IDENTITY_FAIL` and exit 1. The
  edit was reversed **by hand** (no `git checkout` / `restore` / `stash` —
  other agents have live uncommitted work in this tree), the poisoned build
  directory was **deleted**, and every number above was re-measured from a
  forced full rebuild (per the "negative test left a poisoned binary"
  lesson).
- `tools/run_all_checks.ps1` (with `-ExecutionPolicy Bypass`): 94 passed,
  0 skipped, 0 failed.

### Honest status of the arms' BEHAVIOUR (not a tally)

Counts of the driver's own diagnostics, reported because they bear on
whether §8 can answer anything — **these are not results and no
improved/degraded comparison was computed**:

- 260/263 cells run; the 3 kiln-span refusals are the expected
  bounds-exceeded ones, and each emits 9 `CELL_REFUSED` lines per adaptive
  arm (54 total).
- **The adaptive arms are largely inert under the factorial's geometry.**
  Of 4680 firings, 4486 end with `confidence_c == 0`, and only 84 have any
  tick at non-zero fuzzy strength. 218 firings applied an `adaptive_tune`
  refinement.
- Cause, disclosed as a **design judgement, not an accident**: the
  factorial's dwell is `6 * model_tau_s` (min 1200 ticks), whereas
  `sim_scenarios_adaptive.c` needed `16 * tau` before `adaptive_tune` would
  harvest at all. The existing geometry was **kept deliberately** so all
  five arms' objectives stay comparable and `D_adapt` stays meaningful; the
  cost is that the adaptive arms mostly have nothing to learn from.
  Lengthening the dwell would make them active but would change every
  existing arm's row, destroying the inertness proof. **§8 should revisit
  this before concluding the arms do nothing.**
- `oscillation_tripped` is `yes` on 2314 of 4680 firings — about half. That
  is the N3 backstop firing far more often than the plan's narrative
  anticipates. **Not investigated here** (the detector is another agent's
  in-flight code); flagged as the single most likely thing to be wrong
  before §8 is run.

### What could NOT be verified

- The §7 activity and limit-cycle gates are unbuilt, so no claim is made
  about the seven pinned cells (`ST1-049/057/113/121/177/241/249`).
- No KilnFW target build was attempted for this change: it touches
  host-test-only files and adds no task, so nothing needs stack-margin
  registration.

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
