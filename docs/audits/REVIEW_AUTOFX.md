# Review: AUTOFX, fixes for REVIEW_AUTOAPPLY: 0 HIGH, 1 MED, 5 LOW, 5 INFO (2026-10-13)

Adversarial review of the commits that fix `REVIEW_AUTOAPPLY.md`, as landed on
origin/dev:

- `14ff316e0`: recovery glue (`recovery_install_supersede()`, atomic apply claim,
  poll-task stack counted, poll-task create failure recorded) and codec tests.
- `d26785f9a`: prepare tests (empty version, zero length).
- `9241f5362`: MCP `update_install_staged` (one 270 s deadline, POST transport
  failure is UNKNOWN).
- `8e0a53497`: REVIEW_AUTOAPPLY fix status.
- `d43ef4b4a`: check_recovery_wifi_policy mutation anchor.

Reviewed by reading the code and by negative tests. The board was not touched.

## Summary

The codec and prepare fixes (M1-M4) are correct and pinned by real assertions.
The MCP deadline (LOW-5) holds: the worst case is about 285 s, which happens
when the last poll has to log in again (5 s + 5 s login + 5 s retry). That is
under the 300 s abort. The recovery-side fixes are right in direction but incomplete:

- The new UNKNOWN path accepts an older `ok` result as proof that this install
  worked (MED-1).
- One exit route from recovery, `ota_esp_post`, still leaves the request
  (LOW-2).
- The atomic claim covers only apply against apply (LOW-4).
- No test covers any recovery-side fix (LOW-5).

Nothing found bricks the board or applies an unverified image. Every path
still goes through the stage re-hash and the app verify.

## Findings

### MED-1: after a POST transport failure, a stale `ok` result can be reported as this install's success

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:595-611`

REVIEW_AUTOAPPLY's summary relied on one guarantee: prepare erases the old
result before it writes the new request, so an older result cannot pass as
this one. That guarantee holds only if prepare ran. A POST with no HTTP answer
does not show that prepare ran. Before `9241f5362`, that case returned FAILED.
Now it falls into the polling loop. The loop accepts any
`running_version == want` with a recorded `ok`.

Scenario:

1. The board installed 1.0.1 earlier through `update_install_staged`. It runs
   1.0.1, and `last_install` reads `recorded/ok/1.0.1`.
2. The operator force-stages 1.0.1 again (policy `ALLOW_REINSTALL` with
   `force`). `staged_version` is now 1.0.1.
3. `update_install_staged(confirm=True)`. The POST fails at the transport
   level before the handler runs: a connection refused or reset, or a timeout
   while connecting. `exc.status` is None, so `post_unknown = True`.
4. The first poll sees `running_version == "1.0.1"` and `last_install` ok. The
   tool returns `ok - installed '1.0.1'`. Nothing was installed.

The preconditions are narrow: a same-version reinstall plus a transport
failure. The result, though, is a false success from a confirm-gated install
tool. That is the one verdict this tool must never produce.

Fix: on `post_unknown`, require evidence that a new install ran before
accepting `ok`. Either see at least one failed poll (the board went down), or
refuse up front when `before` already shows `running_version == want` with a
recorded `ok` for `want`. A test belongs with the fix: stale ok, transport
failure, expect UNKNOWN.

### LOW-1: LOW-6's UNKNOWN path mislabels a definite auth refusal

Locations:

- `tools/PcTools/src/kilnctrl/mcp_server_update.py:595`
- `tools/PcTools/src/kilnctrl/update_http_client.py:88-96`

`_request` wraps every exception from `http_auth.urlopen`. That includes
`HttpAuthError`, raised when the session is stale and the relogin is refused
(wrong credential, a lockout 429/403) or no credential is set. The wrapped
error is an `UpdateHttpError` whose `status` is None. The tool then reports
`UNKNOWN ... The install POST got no HTTP answer (possibly applied)` and polls
for about 255 s. In fact the board answered 401 and the handler never ran.

Scenario: the admin session expires between the initial GET and the POST, and
`KILNCTL_WEB_PASSWORD` was rotated. The login is refused with HTTP 401, and
the tool says the install was possibly applied.

The result is not dangerous, because the poll never sees an `ok` unless
MED-1's stale case also holds. But the verdict is wrong and the operator waits
four minutes. Fix: treat status None as UNKNOWN only when `__cause__` is a
`URLError`, a socket timeout or a connection reset. Report an `HttpAuthError`
cause as FAILED.

### LOW-2: `ota_esp_post` (recovery_push_esp_image) leaves the install request in place

Location: `firmware/KilnFW_recovery/main/recovery_http.c:237-247`

The fix supersedes the request in `recovery_exit_post` and in the manual
apply. The third route out of recovery is the direct image push. It clears
boot_guard, writes `app`, selects it, and erases the stage header (best
effort). It never calls `recovery_install_supersede()`.

Scenario:

1. An auto-install fails before the copy (APPLY_FAILED or NO_MEM). The request
   stays with attempts=1.
2. The operator pushes an image with `recovery_push_esp_image`. The board boots
   it.
3. At the next recovery entry, `recovery_install_boot()` finds the request:
   - Normal case (stage header erased): it refuses STAGE_NOT_READY, writes a
     FAIL result and shows "INSTALL FAILED". This is the spurious failure
     LOW-2 of REVIEW_AUTOAPPLY described.
   - If `stage_clear_after_upload()` failed (reported in the response, not
     fatal): it applies the staged image unattended over the pushed one. This
     is the original LOW-1 replay.

Fix: call `recovery_install_supersede()` in `ota_esp_post` after the boot
partition set succeeds.

### LOW-3: the poll-task create failure records FAIL while the apply is still running

Location: `firmware/KilnFW_recovery/main/recovery_install.c:243-255`

`update_install_boot_run` has already started the apply task when the poll
task create fails. The new branch immediately writes `FAIL/apply_failed`,
caches it as `s_last`, and shows "INSTALL FAILED" on the LCD. The copy is
still running and usually succeeds.

On success, pre_boot overwrites the result with `ok`, so the app sees the
truth. During the copy, however, the LCD and `GET /api/recovery/status` both
say the install failed. An operator who believes the LCD and power-cycles
interrupts a copy that was in progress. Attempts are bounded, so this is a
retry and not a brick. It is still the wrong signal at the wrong time.

The `APPLY_POLL_STACK` reservation makes this branch hard to reach: it needs a
fragmented heap. Fix: run the poll loop inline in the boot path when the task
cannot be created. Otherwise record nothing and show "INSTALLING (unmonitored)"
until the phase reaches FAILED.

### LOW-4: the atomic claim closes apply against apply only; the other routes still race the unattended start

Locations:

- `firmware/KilnFW_recovery/main/recovery_apply_esp.c:198-201`
- `firmware/KilnFW_recovery/main/recovery_http.c:736`
- `firmware/KilnFW_recovery/main/recovery_http.c:189`
- `firmware/KilnFW_recovery/main/recovery_main.c:95,128`

`recovery_exit_post`, `ota_esp_post`, `sw_reset_post` and `wifi_reset_post` all
read `recovery_apply_busy()` and then act. `recovery_install_boot()` still runs
after `recovery_http_start()`. This is the same window as the original LOW-3,
and the compare-exchange does not cover these routes.

Scenario:

1. An exit request passes the busy check and the app verify.
2. The main task claims and starts the unattended apply. `app` is erased.
3. The exit supersedes the request (clears it), sets boot to `app` and
   restarts 500 ms later. The copy is cut off.
4. The bootloader rejects the half-written `app` and falls back to factory
   (recovery).

The board is now in recovery with no request and a destroyed `app`. It needs
a manual apply over the SoftAP. As before, this is unreachable in practice: a
client must already be joined within milliseconds of SoftAP start.

Fix: call `recovery_install_boot()` before `recovery_http_start()`, or have the
mutating routes take the same claim.

### LOW-5: no test covers any recovery-side fix

Negative tests X3, X4 and X5 below were run against
`check_recovery_wifi_policy.ps1`, the check that compiles `recovery_http.c`.
All three were MISSED:

- removing the manual-apply supersede (X3)
- removing `APPLY_POLL_STACK` from the start check (X4)
- making supersede keep the request (X5)

The other recovery checks do not include `recovery_install.c` either.

The fix status said "Target-only, no host test", so the gap was disclosed.
Still, LOW-1 to LOW-4 of REVIEW_AUTOAPPLY can each regress silently. The
`d43ef4b4a` anchor does not test the supersede call: it only keeps an
unrelated mutant's needle matching.

Separately, `APPLY_POLL_STACK` (`recovery_apply_esp.c:22`) and `POLL_TASK_STACK`
(`recovery_install.c:20`) are two literal 3072s tied together only by
comments. This is the mirror-drift class.

Fix:

- Export the poll stack size from `recovery_install.h` and use it in both files.
- Add a source-level guard that `recovery_install_supersede()` is called in
  each route that sets the boot target to `app`: exit, manual apply and
  `ota_esp_post`. Alternatively, add a host harness for `recovery_install.c`
  with stubbed NVS.

### INFO-1: a supersede failure is fail-open and logged only

Both callers ignore the return value: `recovery_exit_post` and
`manual_apply_pre_boot`. Failing the exit or the apply over a leftover-request
clear would be worse, so fail-open is the right call. Neither response body
says the clear failed, though. The next recovery entry then refuses or
replays, with no hint. Suggest adding a "request clear FAILED" note to the
exit body.

### INFO-2: an exit right after a failed auto-apply can rewrite the FAIL result after the supersede

The poll task wakes every 500 ms. If `recovery_exit` runs between the apply
task setting FAILED and the poll task recording it, the poll task writes `FAIL`
after the supersede has cleared `res`. Only the result is rewritten, never the
request (`update_install_record_result` writes `res` only). The result is
accurate, because the auto-install did fail. This is harmless.

### INFO-3: manual applies now need 3 KB more free internal RAM

The start check counts `APPLY_POLL_STACK` for every apply, including manual
ones, which create no poll task. The refusal threshold rises from 17408 B to
20480 B. That is conservative and acceptable.

### INFO-4: the M6/M7 catches are indirect

Under M6 (cap 2700) and M7 (deadline after the POST), the mocked
`time.monotonic` iterator runs dry. The tool returns
`error: unexpected StopIteration`, and the `startswith("UNKNOWN")` assertion
fails. These are real assertion failures, but they come from exhausting the
fake clock, not from the `call_count == 1` assertion meant to pin the budget.
A clock that returns its last value forever would make the intent explicit.

### INFO-5: REVIEW_AUTOAPPLY fix status cited pre-rebase SHAs

`0388de1f0`, `f8bc24c62` and `92dea4204` are not on origin/dev. This commit
corrects them to `14ff316e0`, `d26785f9a` and `9241f5362`. It also adds
`8e0a53497` and `d43ef4b4a`.

## Negative tests

Run from `C:\wt\rvautofx_my4kxt` (base `0c245eef9`). Every run reported
`real_tree_unchanged: true` and `copies_removed: true`. The worktree was not
edited while the runs were in progress.

```
tools\negtest.ps1 -Preset kilnfw-host -Mutations kf.json -Parallel 2 -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
tools\negtest.ps1 -Preset pytest -PresetArg tests/test_mcp_server_update_install.py -Mutations py.json
tools\negtest.ps1 -Preset check -PresetArg firmware/KilnFW_recovery/main/check_recovery_wifi_policy.ps1 -Mutations rec.json
```

KilnFW baseline: 273 s, pass. Every M1-M4 mutant compiled. Each log has a
`  FAIL ...test_update_install_record.c:<line>` assertion and no build error.
Every pytest CAUGHT is an `AssertionError`, not a collection error.

| # | Mutation | Harness | Verdict | Failing assertion |
|---|----------|---------|---------|-------------------|
| M1 | result decode: drop `r.code > UPD_INST_RES_FAIL` | kilnfw-host | CAUGHT | `test_update_install_record.c:188` |
| M2 | result decode: drop the `reason` range check | kilnfw-host | CAUGHT | `:193` |
| M3 | prepare: drop `req.semver[0] == '\0'` | kilnfw-host | CAUGHT | `:428` |
| M4 | prepare: drop `req.image_length == 0` | kilnfw-host | CAUGHT | `:435` |
| M5 | `if exc.status is not None:` becomes `if True:` | pytest | CAUGHT | `test_mcp_server_update_install.py:96`, `:103` |
| M6 | deadline cap 270 becomes 2700 | pytest | CAUGHT | `:103`, `:112` (indirect, INFO-4) |
| M7 | deadline recomputed after the POST | pytest | CAUGHT | `:112` (indirect, INFO-4) |
| X1 | `post_unknown = True` becomes `False` (own) | pytest | CAUGHT | `:104` ("possibly applied") |
| X2 | `exc.status is not None` becomes `is None` (own) | pytest | CAUGHT | `:38`, `:96`, `:103`, refusal test |
| X3 | manual apply pre-boot drops the supersede (own) | check_recovery_wifi_policy | MISSED | none (LOW-5) |
| X4 | apply start check drops `APPLY_POLL_STACK` (own) | check_recovery_wifi_policy | MISSED | none (LOW-5) |
| X5 | supersede keeps the request (`rq = 0`) (own) | check_recovery_wifi_policy | MISSED | none (LOW-5) |

All 7 claimed CAUGHT verdicts (M1-M7) are confirmed. Own mutants: 2 of 5
caught. The 3 misses are the untested recovery glue.

## Recommendations

1. MED-1: on the UNKNOWN path, do not accept `ok` without evidence that a new
   install ran. Add a test for it.
2. LOW-1: report an `HttpAuthError` cause as FAILED, not UNKNOWN.
3. LOW-2: supersede in `ota_esp_post`.
4. LOW-4: run `recovery_install_boot()` before `recovery_http_start()`.
5. LOW-3: do not record FAIL while the apply runs unmonitored.
6. LOW-5: add one shared poll-stack constant, plus a source guard or host
   harness for the supersede call sites.

## Fix status

Code fixes landed in `e8a6ad365`. The fix doc commit follows it on dev.

| Finding | Status | Notes |
|---|---|---|
| MED-1 | fixed `e8a6ad365` | After a POST with no HTTP answer, `ok` is accepted only once a poll failed (board seen unreachable). Otherwise the tool reports UNKNOWN and says the `ok` may be stale. Pytest: stale ok, and ok accepted after the board went down. The board's status has no uptime field, so unreachability is the only evidence used. |
| LOW-1 | fixed `e8a6ad365` | A `HttpAuthError` cause with no HTTP status returns FAILED at once, with no polling. Pytest. |
| LOW-2 | fixed `e8a6ad365` | `ota_esp_post` calls `recovery_install_supersede()` after the boot partition is set. Recovery target build passes. |
| LOW-3 | fixed `e8a6ad365` | New reason `UPD_INST_REASON_OUTCOME_UNKNOWN` ("outcome_unknown_do_not_power_cycle"), recorded with code FAIL, and the LCD shows "INSTALL UNMONITORED / DO NOT POWER-CYCLE". `test_update_install_record.c` pins the name and the range check. |
| LOW-4 | not fixed | Unreachable in practice (a client must join within milliseconds of SoftAP start); the fix is a boot-order change in `recovery_main.c`, out of scope here. |
| LOW-5 | partly fixed `e8a6ad365` | One shared `RECOVERY_INSTALL_POLL_STACK` define in `recovery_apply_esp.h`, used by both files. Not done: the source-level supersede guard. `recovery_http.c` has no host coverage and no harness was added, so the LOW-2 call is covered by the recovery target build only. |
| INFO-1..5 | not fixed | Informational. |

## Fix status 2 (UPDLOWFX)

- LOW-4: not fixed. Moving `recovery_install_boot()` ahead of `recovery_http_start()` (or giving the four mutating routes the apply claim) changes recovery boot order and the `recovery_apply_busy()` contract, and the recovery image has no host harness. It needs a recovery target build plus hardware verification; the window is milliseconds after SoftAP start and needs an already-joined client.
- LOW-5: already closed by earlier work: the shared poll-stack define (`e8a6ad365`) and the source-level supersede guards (AUTOFX3 INFO-1, `check_recovery_status_fragments.ps1`); no behavioural harness exists.
