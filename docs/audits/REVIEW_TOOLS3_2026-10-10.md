# Review: tooling fix batches negfx2, benchfx2, mcpfx3 (2026-10-10)

Adversarial review of three tooling fix batches on origin/dev:

- negfx2 `8099b1829`: negtest.ps1 job-object kill, `Stop-JobMembers` spare list,
  `Add-Descendants` creation-time filter, `-RequireAssertion` nonzero exit,
  push_verify.ps1 fetch job object.
- benchfx2 `382a66875`: HP-05 "no previous-run record" acceptance, LCD-05 dim
  point 20, LCD-13 picker label lookup, `ota_*` pass-through in
  `bench_test_run`/`bench_test_start`.
- mcpfx3 `43cf6c767` + `6d5599f1e`: `write_memory` KCTL markers on `ok=False`,
  wrapped halt, `leave_halted`; aux board-identity check via `getaddrinfo`.

Reviewed at `29c52d6be` (worktree `C:\wt\rvtools3_indrak`). No code was changed
and the board was not touched. Line numbers are at that commit.

Baseline: `tools/check_negtest.ps1` and `tools/check_push_verify.ps1` PASS; the
relevant PcTools pytest files (debug write, bench_test start/host/suite-gate,
HP judgments/cases, LCD cases/judgments, aux, OTA cases) PASS (841 tests in the
13-file set used for the pytest negtests).

Negtests (`tools/negtest.ps1`, 12 mutations): pytest preset 7 CAUGHT, 1 MISSED
(`wrapper_drops_leave_halted`, M2); `check_negtest.ps1` 2 MISSED (L3);
`check_push_verify.ps1` 2 TIMEOUT (L4). The negtest runs ended with "REAL TREE
CHANGED" only because this review file was written into the worktree while they
ran; the copies were removed and no other file changed.

## Summary

| Sev | Count |
|-----|-------|
| HIGH | 0 |
| MED | 2 |
| LOW | 7 |
| INFO | 8 |

## Fix status

M1, M2, L1-L7 fixed in the toolfx7 commit (see git log subject "Tools review 3 fixes"); each pytest fix negtested CAUGHT (M1, L1, L5, L6); L3/L7 covered by check_negtest.ps1 group A2; L4 by check_push_verify.ps1 (bounded Run-PV).

## MED

### M1. `bench_test_run(suite="ota")` with image paths bypasses `ota_matrix_run`'s confirm gate and run-level preflight

- File: `tools/PcTools/src/kilnctrl/mcp_server_bench_test.py:119-131` (same loop in
  `bench_test_start`, line 253 on); docstring claim at `:90-94`.
- Scenario: before this batch, `bench_test_run(suite="ota")` could only run OT-B01;
  every image-needing case SKIPped, so the only way to push images, roll back a slot
  or reset the safety link was `ota_matrix_run`, which requires `confirm is True`
  exactly and runs its own fail-closed `_run_level_preflight`
  (`mcp_server_ota_matrix.py`: safety not ARMED / link up, executor idle or paused,
  OTA interlock ok, `capability_preflight` refusing an unacknowledged crash or a dead
  required task). Now any caller can pass `ota_image_path=...` etc. to
  `bench_test_run` (no `confirm` parameter at all) or to the background
  `bench_test_start`, and OT-E01/OT-G01..G06/OT-P* run with only their per-case
  gates (`_is_idle`, `_interlock_ok`, `_otg_gate` in `cases_ota.py`). The
  ARMED/link-up check, the crash-report refusal and the explicit confirm are gone on
  this path. The docstring's "Nothing here relaxes a gate" is false, and contradicts
  this function's own advice at `:65` to prefer `ota_matrix_run` for its gate.
- Negtest: mutation `bench_run_drops_ota_ctx` (`if value is not None:` -> `if False:`)
  was CAUGHT (`test_ota_image_arguments_reach_the_runner_ctx`), so the pass-through itself is
  tested; what is untested is any refusal on this path (there is none).
- Fix: either (a) refuse in `bench_test_run`/`bench_test_start` when any `ota_*` /
  `update_*_repo` argument is set unless `confirm is True` and the same
  `mcp_server_ota_matrix._run_level_preflight()` passes (reuse it, do not copy it),
  or (b) drop the pass-through and point callers at `ota_matrix_run` /
  `ota_matrix_start`. Add a test that `bench_test_run(suite="ota",
  ota_image_path=x)` without confirm refuses before `BenchTestRunner` is built.

### M2. `debug_write_memory(leave_halted=True)` is open to any caller, ignores the prior core state, and is undone by the read-back

- Files: `tools/PcTools/src/kilnctrl/mcp_server_debug.py:741-751`,
  `tools/PcTools/src/kilnctrl/debug_probe.py:862-884`.
- Scenario 1 (does not work as documented): after a successful write the wrapper
  calls `_write_readback_note()` (`:684`), which calls
  `debug_probe.read_memory(peer, address, 1, width)` with the default
  `leave_halted=False`. That read halts and then resumes the core. So the core ends
  up running after every successful write, whatever `leave_halted` said. The
  docstring (`:716-717`, "resumed afterwards ... unless leave_halted=True, for a
  core you halted on purpose") is false on the success path. It is honoured only
  when the write fails, which is when leaving the core halted is least useful.
- Scenario 2 (owner rule): the owner rule is "always resume after a halt"
  (2026-10-07 decision, abort stopwatch). `leave_halted=True` skips the resume
  regardless of whether the core was halted before the call: `write_memory` itself
  issues `halt` first, so on a running core it halts the core and leaves it
  halted. Any MCP caller may pass it, for `peer="pico"` included (only the ARMED
  read gates the Pico). Compare `debug_read_memory`/`debug_read_registers`
  (`:606`, `:640`, `:775`), which refuse `leave_halted=True` on ESP unless
  `allow_running=True`; the write path has no such refusal.
- Test coverage: `test_pico_write_resume_and_armed_gate.py` checks `leave_halted`
  only at the `debug_probe.write_memory` level. Mutation
  `wrapper_drops_leave_halted` (wrapper passes `leave_halted=False`) was
  MISSED by the 13-file pytest set (841 tests passed).
- Fix: record the pre-call state (`[target curstate]` before `halt`) and resume
  unless the core was already halted when the call started (so `leave_halted`
  only means "do not resume a core that was halted on entry"). Pass the same
  `leave_halted` through to the read-back. Apply the `allow_running` refusal used
  by the read tools. Add a wrapper-level test (fake `debug_probe`) asserting both
  calls receive the flag.

## LOW

### L1. HP-05 masks a failed `run_state` lock as PASS and never reads the card back

- Files: `tools/PcTools/src/kilnctrl/bench_test/judgments.py:1811`,
  `bench_test/cases_heat.py:852`; firmware `drivers/control/run_state.c:510-512`,
  `drivers/bridge/uart_bridge_ext_control.c:595`,
  `drivers/http/dashboard_exec_http.c:868`.
- Scenario: `run_state_acknowledge()` returns false for three causes: no boot
  record, a record already acknowledged, and `ensure_lock()` failure. Both
  transports map every false to the same text, "no previous-run record to
  acknowledge". The judge now treats that text as "already clear" and passes, so a
  broken run_state lock passes HP-05. HP-05 also no longer proves the ack path
  works at all within one boot, and nothing reads the card back.
- Negtest: mutation `hp05_any_refusal_clear` (accept every refusal) was CAUGHT (`StopTest::test_other_ack_refusal_fails`), but no test covers the lock-failure case because the text is identical.
- Fix: after the ack, read `GET /api/profile_exec` (the boot record and its
  `interrupted` flag, `dashboard_exec_http.c:65-80`) and pass only when the card is
  absent or acknowledged. Longer term, have firmware return distinct reasons
  (lock failure vs nothing to ack).

### L2. negtest: Assign failure now degrades to a root-only kill, and the message still says "taskkill"

- File: `tools/negtest.ps1:504`, `:404-407`.
- Scenario: when `AssignProcessToJobObject` fails (for example the caller is in a
  job that forbids nesting or breakaway), the script prints "falling back to
  taskkill", but no taskkill fallback exists any more. The job stays empty, so
  `Stop-Tree`'s `TerminateJobObject` is a no-op and only the root `cmd.exe` dies.
  The 1.5 s `Add-Descendants` polling plus `Stop-CopyProcesses`' command-line match
  are the only backstop, and a grandchild whose command line does not name the
  copy (e.g. `mspdbsrv`, a compiler launched via response file) escapes.
- Fix: correct the message, and on Assign failure set a flag that makes the timeout
  path kill the tracked descendants (`Stop-Tracked`) before `Stop-CopyProcesses`,
  or fail the run as untrusted.

### L3. negtest: `Stop-JobMembers` kills by PID without re-checking job membership, and nothing tests it

- File: `tools/negtest.ps1:393-402`.
- Scenario: `Members()` snapshots PIDs, then each is opened with
  `GetProcessById` and killed. If a member exits between the snapshot and the
  open and its PID is reused, an unrelated process is killed. The window is short,
  but the comment at `:347-348` claims a reused PID "can never add a stranger",
  which holds for the snapshot only, not for the kill.
- Test coverage: `check_negtest.ps1` only source-greps for
  `QueryInformationJobObject`. Mutations `stop_job_members_noop` (`$mp.Kill()`
  removed) and `members_always_empty` (`Members()` returns nothing) were both
  MISSED by `check_negtest.ps1` (236 assertions still pass).
- Fix: open the process handle first, then confirm `IsProcessInJob(handle, job)`
  before `Kill()` (or compare the creation time against the snapshot). Add a
  check group that leaves a straggler in a job after a normal exit and asserts it is
  gone while a spared name survives.

### L4. push_verify: ignored Assign failure leaves `git-remote-https` orphaned on timeout

- File: `tools/push_verify.ps1:125-129`.
- Scenario: `[void][PvJob]::Assign(...)` discards the result. If assignment fails
  (nested job), a timed-out fetch kills only `git.exe`; `git-remote-https` survives
  and keeps the temp files open (the delete retry at `:134-137` then gives up
  silently). The removed `taskkill /T` covered that case. git is also assigned
  after it starts, so a child spawned before the assign escapes (same residual gap
  negtest documents as 4b). `PvJob` sets no `KILL_ON_JOB_CLOSE`, so a crash of the
  script leaves the fetch running.
- Negtest: mutations `pv_no_job_kill` (no `TerminateJobObject` on timeout) and
  `pv_no_assign` (git never assigned to the job) both made
  `tools/check_push_verify.ps1` hang until negtest's 60 min TIMEOUT: the orphaned
  helper keeps the captured stdout pipe open, and `Run-PV` waits for it with no
  bound. So the job kill is covered, but only by a hang, and a regression here
  stalls `run_all_checks` instead of failing. (No orphaned git process was left
  after negtest's tree kill.)
- Fix: check the Assign result and, on failure, note it in the output and fall back
  to killing the direct children of `git.exe` by handle. Set `KILL_ON_JOB_CLOSE`
  like `NegJob` does. Give `check_push_verify.ps1`'s `Run-PV` a bounded wait (kill
  and FAIL after, say, 60 s) so a regression fails instead of hanging.

### L5. `write_memory` never reports `KCTL_HALT_ERR`

- File: `tools/PcTools/src/kilnctrl/debug_probe.py:869`, `:878-886`.
- Scenario: the halt is now catch-wrapped and prints `KCTL_HALT_ERR`, but the
  parser only looks at `KCTL_WRITE_ERR`, `KCTL_RESUME_ERR` and `KCTL_AFTER`. A
  failed halt followed by a successful write to a running core is reported as a
  plain success; the caller never learns the write raced live code. The opposite
  also happens: OpenOCD logs `Error:` for the caught halt failure, `_run` reports
  `ok=False`, and a write that landed is reported as failed.
- Negtest: mutation `write_mem_halt_check_skipped_when_not_ok` (it
  checks the `ok=False` marker path that this batch added) was CAUGHT; no test covers `KCTL_HALT_ERR`.
- Fix: parse `KCTL_HALT_ERR` and include it in the result as a warning (write
  went to a running core); treat the KCTL markers, not `ok`, as authoritative when
  they are all present.

### L6. aux identity check: name resolved twice (check and POST), first A record only

- File: `tools/PcTools/src/kilnctrl/mcp_server_aux.py:180-222`, `:260`.
- Scenario: `_host_ip()` resolves a host name with `getaddrinfo(AF_INET)[0]`; the
  POST then resolves the name again inside urllib. With DNS rebinding, a short TTL
  or several A records, the check and the write can reach different addresses. On
  this bench the host is normally the board's STA IP literal, so the practical
  exposure is a caller passing a host name.
- Negtest: mutation `aux_identity_ignored` was CAUGHT (`test_board_identity_mismatch_refuses_before_post`); no test covers a name resolving to several addresses.
- Fix: resolve once, compare, then send the POST and the read-back to the
  resolved IP literal (keep the name only for the Host header if needed), or refuse
  non-literal hosts.

### L7. negtest: `$script:liveJob` still holds a closed handle for a few lines

- File: `tools/negtest.ps1:518-528`, finally blocks at `:609`, `:942`.
- Scenario: the job handle is closed at `:523` but `$script:liveJob` is cleared
  only at `:528`. If `Stop-Tracked` or `Stop-CopyProcesses` throws in between (or
  Ctrl+C lands there), the finally calls `TerminateJobObject` on a closed handle
  value. Normally this just fails, but if the value was reused for another
  handle in this process it acts on that.
- Fix: set `$script:liveJob = $null` immediately before `[NegJob]::Close($job)`.

## INFO

- I1. `NegJob` struct layouts are correct on x64 and x86: the
  `JOBOBJECT_BASIC_PROCESS_ID_LIST` count at offset 4 and IDs from offset 8 at
  `IntPtr.Size` stride, the extended-limit size 144/112 and `LimitFlags` at offset
  16. `Members()` silently returns empty when the query fails, including
  `ERROR_MORE_DATA` past 4096 members (nothing killed, nothing reported); a warning
  would help.
- I2. On every timeout or abort the whole job is terminated, which also kills a
  shared `mspdbsrv.exe` started by a job member (pre-existing behaviour). Only
  the normal-exit path spares it.
- I3. `ccache.exe` is spared in `Stop-JobMembers`, `Stop-Tracked` and
  `Stop-CopyProcesses`, so a ccache process holding the copy directory is never
  killed and the worktree removal can fail. Pre-existing; it fails loudly.
- I4. `-RequireAssertion` needing a nonzero exit is covered by the
  `reqassert_exit0` group; the `Add-Descendants` creation-time filter looked right.
- I5. LCD-13 `_lcd13_row_label` (`cases_lcd.py:5965-5978`): a user profile whose
  name equals a builtin's title or code could match first. Unreadable route falls
  back to the name and gives INCONCLUSIVE, not a false PASS. Mutations
  `lcd13_no_fav_prefix` and `lcd13_no_title_lookup` were both CAUGHT.
- I6. LCD-05 dim point 20 is fine (mutation `lcd05_back_to_50` CAUGHT), but the
  snapshot is still named `lcd05_50.jpg` (`cases_lcd.py:5882`).
- I7. The `ota_*` image paths are not validated (any local path is read and
  uploaded). Same as `ota_matrix_run`; it only matters together with M1.
- I8. aux: in AP-only mode `control_set_aux_manual` always refuses (documented).
  With `host=None` the host comes from the UART `wifi.get_status().sta_ip`, so the
  comparison against `get_wifi_status().ip` is nearly tautological for the default
  path. IPv6 literals and unbracketed IPv6 hosts fail closed. `control_set_aux_output`
  has no identity check but reads back over the same HTTP host. urllib honours
  proxy environment variables, which could route the POST elsewhere.
