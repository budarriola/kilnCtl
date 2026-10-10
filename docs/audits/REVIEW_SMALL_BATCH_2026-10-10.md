# Review: two small SaftyFW batches on origin/dev (2026-10-10)

Reviewer: Opus. Base: origin/dev `8c7b78303`. Review only, no code changes.

- **Batch A (saftylow):** `dad356035`, `9f4cdfa92`, `02c09ae6d`. SaftyFW review LOW-1 (an
  uncalibrated CT keeps the heat-possible probe true) documented in
  `firmware/CommonFW/docs/LINK_PROTOCOL.md` with the check unchanged, a new
  `scenario_heat_probe_current_floor` in `firmware/SaftyFW/test/test_link_task_fuzz.c`, and a
  `GATE_NEGATIVE_TEST_EVIDENCE.md` row.
- **Batch B (tglow):** `dfd3a346f`, `8c7b78303`. `build_host_tests.ps1` registered-vs-queued count
  gate, "(did not run)" reporting, a task_harness nested-run guard (exit 2), a direct S5
  `try_clear` test, and NITs.

## Verification

- SaftyFW host tests on the dev tip (`C:\wt\rvsmall_4u50tf`, `build_host_tests.ps1`): exit 0,
  `SAFTYFW HOST TESTS: all passed`, `test_link_task_fuzz: 32226 checks, 0 failures`.
- `tools\negtest.ps1 -Preset saftyfw-host`, each run in a throwaway worktree. Every baseline
  passed, and every run reported `real_tree_unchanged: true`.

| Mutation | Verdict | Meaning |
|---|---|---|
| B1: remove the task_harness nested-run guard | MISSED | No test reaches the guard. That is expected, because no current test nests a run. |
| B2: nest `th_run_captured_task()` from the watchdog loop delay hook, guard present | CAUGHT | The guard fires: `task_harness: nested th_run_captured_task()`, `watchdog_task_tests.exe (exit 2)`. |
| B3: the B2 nesting with the guard removed | CAUGHT | The test crashes with a stack overflow, `exit -1073741571` (0xC00000FD). The guard turns a crash into a clear message. Both versions fail the run. |
| B4: `guard_condition_still_immediate()` S5 case returns false | CAUGHT | Caught by the new check at `test_safety_guards.c:3339`. The older checks at `:3412-3416` also catch it. |
| A1: heat probe `in.any_current_present = false` | CAUGHT | Caught by `link_task_fuzz_tests.exe`. This reproduces the evidence row's claim. |
| A2b: a malformed PUSH_CONTEXT is published (`boot_id=0xEE`) | MISSED | The existing check "malformed contexts never overwrite the last good snapshot" proves nothing (see A-LOW-2). |
| A3b: A2b, with `s_context_lock` set at the start of `scenario_push_context` | CAUGHT | The same check works once a lock exists. |

(The first A2/A3 attempt XORed fields in place. Four malformed frames reuse the same stack slot,
so the XORs cancelled out and the result proved nothing. It was rerun with plain assignments as
A2b/A3b.)

## Batch A findings

### A-LOW-1: the new scenario leaks link_task state into every later scenario

`scenario_heat_probe_current_floor` sets `s_context_lock = (SemaphoreHandle_t)1` and publishes a
fresh idle context. It restores only `g_any_current_present`. `reset_link_state()` resets neither
`s_context_lock` nor `s_context_published`/`s_context_snapshot`.

After this scenario, `trip_seq`, `unknown_commands`, `set_param_refusals` and `update_routing`
see a different environment. Before, they ran with "context never received", where the tc_type
gate and the volatile probe fail closed. Now they run with a fresh, valid, no-heat-flag context,
and `g_tick_ms` does not advance until `scenario_fuzz`. So
`link_task_heat_is_safe_for_tc_type_change()` now answers *safe* in those scenarios, and random
PUSH_CONTEXT payloads in `unknown_commands` now really publish.

No current assertion in those scenarios depends on the heat-safe answer: they assert
`g_enable_true == 0`, the staging counts, `g_cfg_writes == 0` on the SET_PARAM path, and update
routing. So nothing is masked today. But any refusal check added later to those scenarios would
run against the permissive state, which depends on scenario order. Suggest restoring
`s_context_lock = NULL; s_context_published = false;` at the end of the scenario. Better, set up
the lock once in shared setup, which also fixes A-LOW-2.

### A-LOW-2 (existing, found while checking the leak): the push_context "malformed never overwrites" check proves nothing

In `scenario_push_context`, `s_context_lock` is still NULL (task init is not run). So
`link_task_publish_context()` returns early and `link_task_get_context_snapshot()` always returns
false. `had == have == false`, and the check `had == have && (!had || ...)` passes for any code.
Negtest A2b (a rejected frame published) was MISSED. A3b (the same mutation with the lock set in
the scenario) was CAUGHT. The new scenario's own comment names the cause ("task init is not run
here; the gate needs a lock"), but the fix was applied only to the new scenario.

### A-INFO-1: LINK_PROTOCOL.md CT-order paragraph overstates one point and understates another

- Overstated: "on a CT chain marked fitted but not yet calibrated the op-amp offset floor reads
  as current present". With `k_ct_v_per_a <= 0`, `current_presence_is_flowing()` uses
  `delta_counts > CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS` (25 counts). The idle floor
  measured on this bench board's unfitted channels is ~16-17 counts (`config_store.h`
  commissioned-gate comment). So whether the floor reads as present depends on the hardware, not
  a certainty. "can read as present" would be accurate.
- Understated: "do not try to calibrate a fitted CT through the volatile path on an armed board"
  implies the volatile path works on a disarmed board. It may not. `link_task_heat_possible_probe()`
  does not check ARMED: current present, a stale or absent context, or a recent enable(true) each
  make it true on a disarmed board too. The instruction to use `SET_CT_CAL`/`COMMIT_CONFIG` while
  disarmed is correct: both reach `config_store_write()`, which refuses only while ARMED.
- Everything else matches the code: the probe is `!link_task_heat_is_safe_for_tc_type_change()`,
  `link_task_tc_type_gate_decide()` returns not-safe on `any_current_present`, and
  `config_store_write_volatile()` consults the probe. The evidence row's two new mutations: the
  probe one reproduces (A1). The push_context call-site skip was not rerun here.

## Batch B findings

### B-LOW-1: "(did not run)" also labels a test that timed out

`build_host_tests.ps1` now uses `-1` to mean "missing from `$exitCodes`" and prints
`(did not run)`. But `Invoke-HostTestProcess` (`tools/host_test_exec.ps1:37`) also returns `-1` on
a per-executable timeout. A hung executable, which did run, is therefore summarised as
`<name> (did not run)`. The exit code is still 1, and the earlier `FAIL <name>: timed out after N s`
line still prints, so this mislabels the result but does not hide a failure. Suggest a sentinel
that cannot collide, for example `$null` or a separate `$notRun` set.

### B-INFO-1: the registered-vs-queued inequality cannot fail

`$script:allHostNames += $Name` and `$script:queuedBuildCount++` are adjacent lines in
`Add-HostBuild`, so the two counts can never differ. Only the `$regCount -eq 0` arm protects
anything. It does cover the original concern: a dropped `$script:` qualifier leaves the list
empty. Deleting a whole `Add-HostBuild` call stays undetectable, as it was before; that would need
a static expected count. Not harmful. The message reads as if the inequality were an independent
check.

### B-INFO-2: the nested-run guard is correct but untested

The guard sits after the no-task check and before `setjmp`. `s_running` is cleared on both the
normal return and the longjmp return. B2 confirms it fires and B3 shows what it prevents, a stack
overflow crash. No standing test exercises it (B1 MISSED). That is acceptable for a harness
self-check. The TGFIX review already noted it changes no current behaviour.

### B-INFO-3: the new S5 try_clear test duplicates existing coverage

B4 is caught by the new block, and also by the existing checks at `test_safety_guards.c:3412-3416`
("S5 clear refused while the reading is STILL bad"). The new block is harmless but redundant.

### Checked, no finding

- `test_ui_page_home_rail.c`: the removed `-500 F` lower-clamp check really is a duplicate of
  line 90 (same call and expected value).
- `test_s5_fault_bit_terms` message change and the pytest `with` reformat (line continuations
  restored): correct.
- Removing the dead `$mainExit` and mapping a negative exit to `exit 1` are correct. A Windows
  crash code (negative) now exits 1 instead of passing a negative value to `exit`.
