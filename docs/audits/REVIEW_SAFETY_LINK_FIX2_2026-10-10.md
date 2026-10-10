# Review: safety-link fix batch 2 (2026-10-10)

Review only; nothing was fixed. Reviewer: Opus.

## Scope

These commits on `origin/dev` answer `REVIEW_SAFETY_LINK_FIX_2026-10-10.md`:

- `ccbcb992e`
- `3ff645555`
- `5fa28ed3b`
- `8ee4a1723`
- `9121e2b9d`

They cover:

- MED-1: a fatal Pico reboot now pauses with `pico_fatal_reboot`, and the classify timeout is gone.
- MED-2: an unconfirmed grant pauses with `heat_grant_unconfirmed`.
- MED-3: the estop verification is cleared on a read-back match.
- MED-4: the `profile_exec_wdt` stack is now 6144 B.
- LOW-1..8 and LOW-10, which include the SaftyFW trip-snapshot seqlock and the heat-owner flag.

The review ran in worktree `C:\wt\rvsl2_jyeyf0` at `origin/dev` `1d3f7dac8`. All five commits are ancestors of that tip.

## Summary

| Severity | Count |
|---|---|
| HIGH | 0 |
| MED | 2 |
| LOW | 5 |
| INFO | 6 |

The owner-decision path (a benign Pico reboot auto-resumes heat) is correct. It cannot resume heat over a latched trip; see "Owner-decision path check" below.

## MED

### MED-A: a reboot hold withdraws the local grant but never releases it on the wire

**Where:** `firmware/KilnFW/App/drivers/control/heat_enable.c`, `heat_enable_note_pico_boot()`, the fatal branch at about lines 714-731.

**What happens.** The branch sets `reboot_hold = true`, `granted = false`, `pending = false` and `warned_pending = false`. It does not set `release_pending`.

The watchdog then calls `profile_executor_pause_with_reason("pico_fatal_reboot")`. The pause calls `heat_enable_release()`. With `granted` and `pending` both already false, the release path has nothing to send. No `REQUEST_ENABLE(false)` goes out.

**When it matters.** A real fatal reboot clears the Pico's grant by itself, so the common case is safe.

The hold can also fire without a real reboot. `safety_reset_stale_peer_info_if_link_down()` clears `pico_boot_id_known` on any link outage, and a failed status read reports seq 0. Either one makes the next DIAG look like a new boot. If `boot_reason` still shows the earlier fatal cause (an earlier fatal reboot that the operator resumed from), the hold fires.

In that case the Pico's grant is still in force and K4 stays energised. Meanwhile the ESP believes heat is withdrawn and the run is paused. Relays are forced off by the pause, so no heater is driven. Still, the K4 pilot interlock is left closed with nobody holding it.

**Test gap.** Mutation K1 (drop the withdrawal) is CAUGHT, but no test asserts that a release frame is sent when a hold arrives while granted.

**Suggested direction.** Set `release_pending` when `granted || pending` in that branch, and add a test that checks the wire send.

### MED-B: the watchdog-task pause uses an unbounded lock, bypassing guard 9's bounded take

**Where:**

- `profile_executor.c`, `watchdog_task_entry()`: the bounded `s_exec.lock` take at about line 2299, released at about line 2417. The new pause calls are at about lines 2462-2481.
- `profile_executor_status.c`, `profile_executor_pause_with_reason()` at about lines 278-334, which does `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`.

**What happens.** The watchdog loop takes the executor lock with a timeout and `continue`s on failure, so a wedged control task cannot stall guard 9. The new hold and unconfirmed-grant pauses run after that bounded section and call `pause_with_reason()`, which blocks forever on the same lock.

If the control task wedges while holding `s_exec.lock` in that window, the watchdog task blocks indefinitely. Guard 9's stale-tick check, the 30 s link-silence abort and the trip checks then all stop running. That is the exact case the bounded take was written to survive.

**Suggested direction.** Give the pause a bounded-wait variant for the watchdog caller, retried on the next tick. Alternatively, set a flag that the next bounded section acts on.

## LOW

### LOW-A: an undecided reboot leaves the firing RUNNING with K4 open and no reason shown

While `reboot_classify_pending` is set (a new boot seen but no new-boot DIAG yet), retries are suppressed and the grant is gone. Nothing pauses the run and no `pause_reason` is set, although the commit message says the executor shows this condition.

The firing sits RUNNING and cold. The only bounds are guard 1 during heating segments and the 30 s link-silence abort if the link is also silent. The classify timeout was removed deliberately (MED-1), but this window has no visible state.

### LOW-B: the code still calls the benign-reboot case an open owner decision

`heat_enable.c` (about line 744) has a TODO saying the benign case is an "open owner decision". `test_heat_enable.c` has a matching comment. The owner decided on 2026-10-10 that a benign reboot auto-resumes heat. The code implements that decision; the comments say otherwise.

### LOW-C: the MED-3 estop clear is skipped when the commit was ACKed but the read-back failed

`safety_cfg_write.c` clears the estop-verified flag only when `readback_matched` is true (about line 584; the flag is set at about line 316). A write that the Pico ACKed but whose read-back failed or timed out within `SAFETY_CFG_PERSIST_WAIT_MS` (5000) leaves the estop verification in place, even though the safety config may have changed.

This fails open in the "verified" direction. Clearing on any ACKed commit would be the conservative choice.

### LOW-D: autotune is not paused on a hold or an unconfirmed grant

The new pause calls cover `profile_executor` only. An autotune run that loses its grant the same way keeps running with no reason shown. It is bounded only by autotune's own timeouts and guard 1.

### LOW-E: a trip seen only by TRIP_EVENT, followed by a benign reboot, auto-resumes heat

If the ESP saw a trip only through a `TRIP_EVENT` frame, and the Pico then rebooted with a benign `boot_reason`, the ESP treats the reboot as benign and re-requests heat.

The Pico's own latch decides whether that request is granted: a latched trip survives only if it was persisted. This is an inference from the code paths, not a measured result.

Related point: the Pico never sets `SAFETY_LINK_DIAG_BOOT_BROWNOUT`. A brownout reports as POWERON and is therefore classed as benign. That matches the owner decision, but mutation K3 shows the BROWNOUT bit in the fatal mask is dead on today's Pico firmware.

## INFO

1. **Latched trip cannot be resumed (owner-decision check, below).**
2. **Contradictory `diag_state` comments.** `safety_link.h` and `safety_link_frames.c` disagree on whether `diag_state` reflects the current boot.
3. **Seqlock fallback.** In `safety_core.c`, the reader falls back after 64 retries to a possibly torn set (about line 1736). This is benign because of `trip_seq` dedup and because the writer publishes `s_trip_seq` inside the odd window. The writer and reader barriers (`HAL_DMB`) are correct on both Pico and MSVC.
4. **`pause_reason` nits.**
   - The `profile_executor_internal.h` comment on `char pause_reason[48]` says "Points at string literals only", which is wrong for an array.
   - `dashboard_exec_http.c` added the field. Its worst-case fixed part is about 825 B of the 960 B allowance, but the budget comment was not updated.
   - `test_dashboard_json.c` never fills `pause_reason`.
5. **Weak source-text tests.**
   - `test_link_staging.c` only checks that the flag string appears before the call.
   - The executor-wiring test in `test_heat_enable.c` is source-text only.
   - Mutations K9 and K11 survive (below).
6. **Lock order is fine.** The watchdog pause takes `s_exec.lock`, then the heat_enable lock. `heat_enable.c` never calls into the executor, so there is no inversion.

## Owner-decision path check (benign reboot auto-resumes)

The benign path is correct, and it cannot resume over a latched trip, for three reasons:

- The F1 K4 reconcile re-requests heat only when the Pico state is ARMED or WARN.
- A DIAG from the old boot is fed to `note_pico_state` as INIT (`!diag_since_reboot`), so stale ARMED cannot trigger a re-request.
- A TRIPPED DIAG faults the run.

`want_retry` excludes both `reboot_hold` and `reboot_classify_pending`. The remaining caveats are LOW-E (a brownout is classed as benign) and MED-A (the hold's missing release).

## Stack depth (fresh target-build ELFs)

The ELFs came from `C:\wt\checkbuild_c19ec8750b`, built from `e7e698c6e`. That build contains the whole batch; only `danger_mode.c` differs from the review tip.

| Task | Declared | Result | Status |
|---|---|---|---|
| `profile_exec_wdt` (KilnFW) | 6144 B | INDETERMINATE, lower bound 2736 B; re-pinned ceiling 2736 | ok |
| `profile_exec` executor (KilnFW) | 6144 B | naive free 2736 B, honest free 1516 B (24.7%) | LOW (ok) |
| `safety_core` (SaftyFW) | 6144 B | INDETERMINATE, 2208 B; ceiling 2208 | ok |
| `link_task` (SaftyFW) | 10240 B | 5080 B | ok |

MED-4's 6144 B for `profile_exec_wdt` leaves wide headroom over the static lower bound. Nothing measured is near its limit.

## Negative tests (`tools/negtest.ps1`, reviewer-authored mutations)

### SaftyFW (preset `saftyfw-host`, baseline PASS)

| Mutation | Verdict |
|---|---|
| S1 reader accepts the first read without retry | CAUGHT |
| S2 reader uses `\|\|` instead of `&&` | **MISSED** |
| S3 `s_trip_gen` not volatile | **MISSED** |
| S4 writer skips the odd generation bump | CAUGHT |
| S5 heat-owner flag argument forced to true | CAUGHT |

Logs: `%TEMP%\negtest_logs\20261010_073322_86na`.

### KilnFW (`build_host_tests.ps1 -Only heat_enable|safety_cfg|kiln_cfg_swap|profile_executor|dashboard_exec`, baseline PASS)

| Mutation | Verdict |
|---|---|
| K1 hold keeps the grant | CAUGHT |
| K2 retry allowed while classify is pending | CAUGHT |
| K3 fatal mask drops BROWNOUT | CAUGHT |
| K4 fatal mask drops ASSERT_FAILED | CAUGHT |
| K5 no K4 episode reset on reboot | **MISSED** |
| K6 GRACE keeps the K4 episode | CAUGHT |
| K7 WARN state not timed | CAUGHT |
| K8 stale reading keeps the timer | CAUGHT |
| K9 old-boot DIAG not fed as INIT | **MISSED** |
| K10 no `pico_fatal_reboot` pause | CAUGHT |
| K11 `pause_reason` not stored | **MISSED** |
| K12 no estop clear on read-back | CAUGHT |
| K13 read-back flag never set | CAUGHT |
| K14 persist wait 5000 -> 3000 ms | **MISSED** |

Logs: `%TEMP%\negtest_logs\20261010_073325_gza3`. Both runs reported the real tree unchanged and removed their copies.

### Notes on the misses

- **K9** is the guard that stops a stale ARMED from resuming heat after a reboot. It is the most important uncovered line in the batch.
- **K11** means no test checks that `pause_reason` reaches the status.
- **K5** means no test checks the K4 episode reset on reboot.
- **K14** is a tuning constant and is acceptable to leave untested.
- **S2 and S3** need a concurrency or fault-injection test of the seqlock reader. The host tests run it single-threaded.

## Tests run

- `negtest.ps1` as above. Each run built a fresh baseline; the two baselines passed.
- `check_kilnfw_*stack_budget*` and `check_saftyfw_task_stack_budgets.ps1`, run against the checkbuild ELFs. The SaftyFW check was run from the checkbuild tree's own copy, because the worktree copy refused the ELF as stale by source mtime.
- `run_all_checks.ps1` and full target builds were not run, as this task's rules require.
- The bench board was not touched.
