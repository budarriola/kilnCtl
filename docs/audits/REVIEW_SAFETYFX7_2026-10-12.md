# Review: safetyfx7 (F5HOLDERFX4 MED-1 / LOW-1 / LOW-2 fixes), 2026-10-12

Adversarial safety review of these four dev commits:

| SHA | Subject |
|---|---|
| `7592416a0` | Fail closed on stale or missing Pico DIAG (MED-1). A cached TRIPPED counts at any age, a start needs a fresh DIAG by `diag_frame_age_ms`, and the watchdog uses it. Adds a post-lock re-sample (LOW-2). The resume refusal carries its reason (LOW-1). |
| `a7e404378` | Tests the resume refusal reason (LOW-1). |
| `7e433c067` | Pins `diag_since_reboot` and pins the watchdog any-age wiring exactly (negtest MISSED fixes). |
| `d78551461` | Audit doc: F5HOLDERFX4 MED-1, LOW-1 and LOW-2 marked fixed. |

The review was done in worktree `C:\wt\rvsafetyfx7_21qlow` at `origin/dev`, which contains `d78551461`. Line numbers below are at that tree.

**Verdict: 0 HIGH, 1 MED, 5 LOW, 7 INFO.**

The safety logic of the fix is sound. Every path that decides a trip or an authority now reads either the DIAG frame's own age (`diag_frame_age_ms`) or nothing age-based at all (a cached TRIPPED counts at any age). Nothing in the fixed code still uses the STATUS frame's `age_ms` for those decisions. A brief DIAG gap does not fault or abort a running firing. The new refusal string is bounded and lock-guarded, and the lock order is one that already existed.

The MED is a test break. `7592416a0` turned the KilnFW host-test suite red on dev: `test_autotune_engine_prestart` now fails because its safety-link stub never reports a DIAG.

## What was checked

1. **Every remaining user of STATUS `age_ms` for trip or authority decisions.** I grepped every `age_ms`, `cached_tick`, `diag_age_ms` and `SAFETY_LINK_STALE_MS` use under `firmware/KilnFW/App/drivers`.
   - None remain in `relay_authority.h` or `profile_executor*.c`.
   - Two pre-existing command-path users and three display-only users remain (LOW-2 and LOW-3 below).
   - `ct_leak_alarm_service.c:92` uses STATUS age for STATUS data (current readings). That use is correct.
2. **Stale-but-not-tripped DIAG and the non-K4 outputs** (aux, io segments, manual aux, zone convert). See LOW-1.
   - Zone convert (`zone_aux_convert_core.c`) never drives a relay.
3. **Can a DIAG gap wrongly wedge or abort a running firing?** No.
   - The watchdog's `safety_processor_tripped` is `relay_authority_pico_tripped()` at any age (`profile_executor.c:2360`), so only a cached TRIPPED faults.
   - A stale DIAG makes `safety_diag_valid` false (`profile_executor.c:2339-2340`). Its only effect is that `heat_enable_note_pico_state()` (`profile_executor.c:2563`) stops K4 timing. It never faults.
   - The start gate's freshness requirement (`relay_authority.h:390`) applies to start and resume only, never to a running tick.
   - A cached TRIPPED that outlives the trip because DIAG frames are lost keeps blocking starts and faulting a run until the next DIAG arrives. That fails safe, and it is the intended trade.
4. **Resume refusal reason: leak or overflow.**
   - The strings are fixed text plus one `%.32s` source word, and `snprintf` is bounded to `s_resume_refusal[128]` (`profile_executor_status.c:351`).
   - The buffer is written and cleared under `s_exec.lock` (`:361`, `:369`) and copied out under the lock (`:450`).
   - The HTTP copy is `why[128]` (`dashboard_exec_http.c:850-854`).
   - The text carries no secret: it names only a fault-source word.
5. **Reentrancy and lock order of the re-sample.**
   - The post-lock re-sample (`profile_executor.c:748-756`) and the retry re-sample (`:2089-2096`) call `safety_link_get_status()` while holding `s_exec.lock`. That takes `link->state_lock` inside `s_exec.lock`.
   - The same order already existed: the retry path did it, and resume calls `relay_authority_start_blocked()` under `s_exec.lock`.
   - `safety_link` calls nothing back into the executor while it holds `state_lock`, so there is no inversion.
   - The re-sample only ORs into the pre-lock value, so a race can make it more conservative, never less.
6. **Stack.** See LOW-4. Not measured: `check_executor_task_stack_budget` and `check_httpd_task_stack_budget` need a target ELF, and full target builds were out of scope for this review.

## Findings

### MED-1: `test_autotune_engine_prestart` is red on dev since `7592416a0`

- **Where:**
  - `autotune_engine.c:1102` calls `relay_authority_start_blocked()`, so the new fresh-DIAG requirement (`relay_authority.h:390`) now also gates every autotune start.
  - The stub `safety_link_get_status()` in `test_autotune_engine_prestart.c:280-293` zeroes `*out` and sets only `link_up`, so `diag_ever_received` is false.
- **What happens:** every positive-control autotune start in that executable is now refused with "waiting for the safety processor's status report".
- **How it was seen:** the baseline of a full `-Preset kilnfw-host` negtest run, on the unmodified tree at `d78551461`, failed with `RUN FAILURES (1): autotune_engine_prestart`. The first failures are at `test_autotune_engine_prestart.c:1584` ("positive control: SIMC must be accepted") and `:1426` ("autotune_engine_run() must accept a step test on zone 0"). Those are followed by about a dozen dependent failures (`:2371-2427`).
- **Why it was missed:** the fix agent's test runs were filtered with `-Only` to the executor and HTTP executables.
- **Still open:** no commit on `origin/dev` up to `9a6debca9` touches the file.
- **Safety:** the firmware behavior is correct and fails closed. Autotune now waits for a fresh DIAG exactly as a firing does. Only the test fixture is stale.
- **Fix:**
  - Default the stub to `diag_ever_received = diag_since_reboot = true` and `diag_frame_age_ms = 0` with an ARMED state.
  - Add one negative case: an autotune start with a stale DIAG is refused.
  - Run the full `kilnfw-host` suite, not `-Only`, before landing.

### LOW-1: a stale-but-not-tripped DIAG still lets non-K4 outputs switch ON mid-run, and manual writes never consult DIAG

- **Where:**
  - `profile_executor_relay_io.c:691` (`aux_apply_relay_pico`)
  - `:1003` (aux tick)
  - `:1222` (`io_seg_start_since_pico`)
  - `:1363-1375` (`io_segs_retry_on`)
- **What happens:** these gates refuse an ON only on `relay_authority_on_blocked() || pico_tripped`.
  - `relay_authority_on_blocked()` reads only ESP-local fault sources.
  - The STATUS frame's FAULT flag is masked out at `safety_link_frames.c:803`, so a Pico trip never reaches `fault_sources`.
  - So when STATUS frames keep arriving but DIAG frames are lost, the Pico's trip state never reaches these gates. A Pico that tripped after the last DIAG it delivered stays invisible.
- **Contrast with the start gate:** the start gate now requires a fresh DIAG (`relay_authority.h:390`), but these mid-run ON paths do not. Aux outputs are not behind K4, so they are the outputs this matters for.
- **Manual writes:** `dashboard_set_relay` reaches `kiln_io_owner`'s `relay_on_blocked`, which uses only `relay_authority_on_blocked()`. It has no Pico TRIPPED check and no DIAG freshness check.
  - A manual aux ON is dropped on the next executor tick only when a TRIPPED is cached (`profile_executor_relay_io.c:797-802`).
  - This part is pre-existing and was not touched by safetyfx7.
- **Likelihood:** low. It needs sustained loss of DIAG frames only, while STATUS frames keep arriving. Only a cached TRIPPED, at any age, stops the ON.
- **Fix:** pass `pico_tripped || !relay_authority_pico_diag_fresh(&st)` into the ON gates only. Keep the fault and drop paths on `pico_tripped` alone, so that a DIAG gap never drops an output already on and never aborts a run. The manual-write path in `kiln_io_owner` could take the same ON-only check.

### LOW-2 (pre-existing): `clear_trip` and `test_trip` measure "DIAG age" from the STATUS tick

- **Where:**
  - `safety_link_commands.c:132`, checked at `:139`
  - `safety_link_commands.c:1043`, checked at `:1046`
- **What happens:** `diag_age_ms` is computed from `link->cached_tick`, which is the STATUS frame's tick set at `safety_link_frames.c:898`. It is not taken from `diag_tick`, and it is compared against `SAFETY_LINK_STALE_MS`.
- **Why it matters:** this is the same bug class MED-1 fixed, in the two command paths. A command can be decided on a DIAG state that is up to one DIAG period plus a gap old, while the code believes it is no more than 1.5 s old.
- **Fix:** use `diag_frame_age_ms` and `SAFETY_LINK_DIAG_FRESH_MS`, as `relay_authority_pico_diag_fresh()` does.

### LOW-3 (pre-existing, display only): the LCD and UI derive "tripped live" from the STATUS age

- **Where:**
  - `dashboard_http.c:351` sets `out->diag_age_ms = sl.age_ms`, which is the STATUS age.
  - `ui_page_safety_logic.c:10` (`ui_safety_view_derive`)
  - `ui_page_home_refresh.c:243-244`
  - `screen_idle.c:223`
- **What happens:** these screens treat a cached TRIPPED as not live once the STATUS age passes 1.5 s. The authority now treats it as tripped at any age.
- **Why it matters:** `ui_safety_view_derive` also gates the LCD clear verdict.
- **Fix:** carry `diag_frame_age_ms` into `dashboard_status_t` and derive the view from the DIAG age, or show a cached TRIPPED as tripped at any age, matching the authority.

### LOW-4: unmeasured stack growth on four task stacks

- **Resume path:** `profile_executor_resume()` now passes a non-NULL `err` into the inline `relay_authority_start_blocked()`. With `err == NULL` the compiler could drop `words[160]` and the `snprintf` calls as dead code, and now it cannot. This frame runs on every resume caller's stack:
  - the UART bridge (`uart_bridge_ext_control.c:587`)
  - LVGL (`ui_page_home_actions.c:360`)
  - httpd (`dashboard_exec_http.c:851`), which also adds `why[128]`
- **Executor task:** gains one more block-scoped `safety_link_status_t` (`post_st`, `profile_executor.c:748-756`). GCC probably shares its slot with `pre_st` and `retry_st`, but that is not guaranteed.
- **Fix:** run `firmware/KilnFW/App/test/check_executor_task_stack_budget.ps1` and the httpd/LVGL/UART-bridge stack budget checks against the next target ELF before the next flash. Raise any stack that is too small; stack bumps are pre-authorized.

### LOW-5: the watchdog's any-age TRIPPED wiring is not behaviourally pinned (mutant M05 survives)

- **Where:**
  - `profile_executor.c:2360` (`bool safety_processor_tripped = safety_diag_tripped_any_age;`)
  - `test_profile_executor_prestart.c:3131-3134`
- **What happens:** the pins check only the text of the assignment to `safety_diag_tripped_any_age`, not how it is used. So mutant M05 (`= safety_diag_valid && safety_diag_tripped_any_age;`) survives. M05 reintroduces exactly the MED-1 bug: a stale cached TRIPPED no longer faults a running firing.
- **Commit claim:** commit `7e433c067` says it pins the watchdog any-age wiring exactly. It pins the input, not the use.
- **Why the result stands without a full-suite rerun:** only `test_profile_executor_prestart` compiles `profile_executor.c`, so the `-Only` subset result is authoritative. The full-suite recheck could not attribute results because of MED-1's red baseline.
- **Fix:** add a tick-level test. Drive the executor task body with a running firing, then supply a cached TRIPPED DIAG whose `diag_frame_age_ms` is above `SAFETY_LINK_DIAG_FRESH_MS`. Assert that the run faults. At minimum, add a `strstr` pin on the `safety_processor_tripped = safety_diag_tripped_any_age;` line.

### INFO

- **I-1. Stale doc comments.** These comments still say "fresh" for a value that now counts at any age:
  - `profile_executor.h:151` (wd_input `safety_processor_tripped`, "off a FRESH (non-stale) DIAG frame ... caller is responsible for the staleness gate (against SAFETY_LINK_STALE_MS)")
  - `profile_executor_relay_io.c:787` and `:993` ("a fresh Pico TRIPPED report")
- **I-2. Freshness comparison mismatch.** `dashboard_status_http.c:759` (`trip_is_test`) uses `diag_frame_age_ms < SAFETY_LINK_DIAG_FRESH_MS`, while `relay_authority_pico_diag_fresh()` uses `<=`. They disagree only at exactly 4500 ms.
- **I-3. Helper not reused.** The watchdog's `safety_diag_valid` (`profile_executor.c:2339-2340`) open-codes the freshness test without `diag_since_reboot`; the K4 call substitutes INIT for that case. It could call `relay_authority_pico_diag_fresh()` so the two cannot drift.
- **I-4. Concurrent resume race.** Two concurrent resumes (LCD and HTTP) can race. The HTTP handler may then read a reason the other caller just cleared and report "nothing paused to resume" for a refused resume. This is cosmetic.
- **I-5. Status code and reach of the reason.**
  - A gate refusal answers 400. A 409, like the system-mode gate, would fit better.
  - The LCD and UART resume callers never show the reason.
- **I-6. Zone current sweep start gate.** The zone current sweep start (`zones_current_sweep_task.c:2319`) and its engine (`zones_current_sweep_engine.c:1190`) check any-age TRIPPED or `fault_asserted`. Two gaps, both pre-existing and outside this change set:
  - Neither requires a fresh or since-reboot DIAG at start, as the executor start gate now does.
  - `zone_sweep_hw_trip_latched` returns false when the status read fails.
- **I-7. Pre-lock Pico sample is near-redundant (mutant M11 survives).** The pre-lock sample (`profile_executor.c:740-746`) is now almost redundant. The post-lock re-sample at `:748-756` ORs in the same cached, any-age TRIPPED. The pre-lock value matters only when the post-lock `safety_link_get_status()` read fails, so M11 surviving is not a coverage hole worth a test. The comment at `:738` ("read outside s_exec.lock") no longer describes the whole sampling, since the second read is under the lock. That under-lock read is a cheap cached copy, not a producer with I/O.

## Test pinning

- The any-age watchdog wiring and the LOW-2 re-sample position are pinned only by source-text `strstr` checks (`test_profile_executor_prestart.c:3131-3150`). There is no behavioural tick test for them.
- Mutation M05 shows the gap: a wrong rewrite that keeps the pinned text passes (LOW-5). The reverse also holds: a correct rewrite that changes the text would fail the test.

## Mutation table

Each mutant was run through `tools\negtest.ps1`. Every mutant was compiled from scratch in a throwaway worktree copy, and the unmodified baseline passed first.

- Runner: `-Command` with `firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT} -Only "profile_executor|dashboard_exec_http|relay_authority"`
- Expect pattern: `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`

For every CAUGHT row, I confirmed from the log that the mutant compiled and that a test assertion failed.

| # | File | Mutation | Verdict | Failing assertion (first) |
|---|---|---|---|---|
| M01 | `relay_authority.h:352` | `pico_tripped` drops `diag_ever_received &&` | CAUGHT | `test_profile_executor_prestart.c:12227` never-received status is not tripped |
| M02 | `relay_authority.h:359` | `pico_diag_fresh` `<=` becomes `<` | CAUGHT | `:13782` diag age == FRESH_MS is allowed |
| M03 | `relay_authority.h:359` | `pico_diag_fresh` drops `diag_since_reboot &&` | CAUGHT | `:13795` DIAG not since Pico reboot is refused |
| M04 | `relay_authority.h:390` | start gate skips the freshness check (`false &&`) | CAUGHT | `:2359` resume refused on a stale DIAG, plus 8 more |
| M05 | `profile_executor.c:2360` | watchdog `safety_processor_tripped = safety_diag_valid && safety_diag_tripped_any_age` | **MISSED** | none (LOW-5) |
| M06 | `profile_executor.c:754` | post-lock re-sample removed (`(void)post_st;`) | CAUGHT | `:3147` re-sample directly follows the lock take (text pin) |
| M07 | `profile_executor_status.c:369` | resume passes `NULL, 0` for the reason | CAUGHT | `:2356` refusal reason names the trip |
| M08 | `profile_executor_status.c:361` | reason not cleared at resume entry | CAUGHT | `:2366` a successful resume leaves no refusal reason |
| M09 | `dashboard_exec_http.c:854` | HTTP ignores the reason (`false &&`) | CAUGHT | `test_dashboard_exec_http_handlers.c:278` gate refusal carries its reason |
| M10 | `profile_executor.c:2093` | retry re-sample replaced by `false` | CAUGHT | `:3153` Pico state re-sampled before the retry (text pin) |
| M11 | `profile_executor.c:744` | pre-lock sample replaced by `false` | MISSED | none; equivalent while the post-lock read succeeds (I-7) |
| M12 | `profile_executor.c:2340` | watchdog validity uses STATUS `age_ms` | CAUGHT | `:3132` watchdog DIAG validity uses diag_frame_age_ms (text pin) |

About the results:

- **Clean compile:** none of the 12 logs contain `BUILD FAILURES`. Every CAUGHT log shows at least one `  FAIL file:line:` assertion and `RUN FAILURES (1)` naming the test executable.
- **Clean baseline:** the subset baseline passed. negtest reported `real_tree_unchanged: true` and `copies_removed: true`.
- **Full-preset recheck:** a recheck of M05 and M11 with the full `-Preset kilnfw-host` could not attribute results, because its baseline failed (MED-1). Only `test_profile_executor_prestart` compiles `profile_executor.c`, so the subset verdicts for M05 and M11 stand.
- **Text pins:** M06, M10 and M12 are caught only by source-text pins, not by behaviour.
