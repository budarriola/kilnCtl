# Review: AUTOAPPLY unattended install (2026-10-13)

Adversarial review of the unattended-install feature on origin/dev ending
`2013cf733`: the application writes a one-shot install request to NVS and
reboots into the recovery image, which applies the verified stage with no
SoftAP join.

Commits reviewed: `f263ebe8f` (flash_firmware `set_otadata_app`), `cafff48f2`
and `77c870bc5` (request/result record, attempt counter), `0459844fd`
(recovery_boot prepare/undo hooks), `6e2b8c84e` (POST /api/update/install,
GET /api/update/status, URI cap 190), `c62ca693f` (recovery wiring, web
Install button, MCP tool), `dc65ac669`, `768889b5a` and `ff5eacd09` (tests),
`e5ae3a14c` (docs), `2013cf733` (checks).

Reviewed by reading the code only. The board was not touched.

## Summary

0 HIGH, 0 MED, 6 LOW, 5 INFO.

No path was found that bricks the board or loops it through boots without a
bound:

- Recovery re-hashes the stage before any erase and checks its project and
  size (`recovery_apply_esp.c`). It re-hashes the app and verifies it before
  selecting it.
- The attempt counter is written with a read-back BEFORE `apply_start`
  (`update_install_record.c:226-229`). A failed counter write refuses the
  install without erasing anything.
- `attempts >= 3` clears the request (`update_install_record.c:207`). A power
  loss in the middle of a copy therefore retries at most three times. After
  that the board stays in recovery with the stage still available for a
  manual apply.
- Pre-boot clears boot_guard, then the request, and only then selects the app
  (`update_install_record.c:153-158`). A failed clear refuses the select.
- The application gate order is: mode gate, interlock, run claim, relay read,
  prepare, select_boot. A failed hand-off runs undo.
- MCP `update_install_staged` refuses unless `confirm is True`. It reports `ok`
  only when `running_version` equals the staged version AND a recorded `ok`
  result is present. The stale result is erased by prepare before the new
  request is written (`update_install_record.c:287`), so an older result
  cannot pass as this one.

## Findings

### LOW-1: an install request left behind is replayed on a later, unrelated recovery entry

Locations:

- `firmware/KilnFW/App/drivers/update/update_install_http.c:143-148`: `install_undo` only logs a failed erase.
- `firmware/KilnFW_recovery/main/recovery_http.c:734-775`: `recovery_exit_post` never clears the request.
- `firmware/KilnFW_recovery/main/recovery_http.c:1038-1041`: manual `apply_pre_boot` clears only boot_guard.

The request is left in place by the refusals NVS_UNAVAILABLE, COUNTER_FAILED
and APPLY_FAILED, and by an undo whose erase failed. Nothing at application
boot removes a leftover request.

Scenario:

1. An auto-install fails before the app is touched. Examples are APPLY_FAILED
   from NO_MEM, or a stage re-hash that fails inside the apply task. The
   request stays with attempts=1.
2. The operator joins the SoftAP and runs `recovery_exit`. The app boots.
3. Weeks later the board enters recovery for an unrelated reason: the
   boot_guard threshold, or `recovery_enter`.
4. `recovery_install_boot()` finds the old request. It applies the staged
   image with no fresh mode gate or interlock, and no operator intent.

Limits on the damage: the sha must still match the stage, so only the same
image is installed. Attempts stay bounded at three. No firing can be running
while in recovery.

Fix: `recovery_exit_post` and the manual `apply_pre_boot` should clear the
request (and record a "superseded" result), or the application should erase
`upd_inst/req` once it boots healthy.

### LOW-2: a stale failure result survives a successful manual apply

Location: `firmware/KilnFW_recovery/main/recovery_http.c:1038-1041`

Scenario: an auto-install records APPLY_FAILED. The operator then runs a manual
`recovery_apply_staged`, which succeeds. That path's pre_boot does not touch
`upd_inst/res`. GET /api/update/status then reports
`last_install{code:fail, reason:apply_failed}` while the new version runs.

The MCP tool is not used on this path, so it reports no false verdict. A human
reading the web page does see a contradiction.

The same lingering request also causes the next recovery entry to refuse with
STAGE_NOT_READY (the stage header was erased) and to show "INSTALL FAILED" on
the LCD. If the stage-header erase had itself failed, that next recovery entry
reinstalls the same image.

### LOW-3: the s_running check-then-set in recovery apply is not atomic

Location: `firmware/KilnFW_recovery/main/recovery_apply_esp.c:197-203`

`recovery_install_boot()` runs after `recovery_http_start()` has succeeded
(`recovery_main.c:95` and `:128`). A POST /api/recovery/apply_staged handled
in the httpd task in that window can pass `s_running == false` at the same
time as the main task. The two then share `s_ctx` and `s_scratch`, and two
apply tasks copy at once.

Each verify still catches a torn result, so the app is never selected
unverified. The result is a modified app and a board that stays in recovery.

The window is milliseconds long and needs a client already joined to a
just-started SoftAP whose passphrase is shown only on the LCD. In practice it
is unreachable. Fix: set `s_running` under a spinlock or with an atomic
compare-exchange.

### LOW-4: the poll task's stack can push internal RAM under the 8192 B floor

Locations:

- `firmware/KilnFW_recovery/main/recovery_install.c:20`
- `firmware/KilnFW_recovery/main/recovery_install.c:243`
- `firmware/KilnFW_recovery/main/recovery_apply_esp.c:200`

The apply start-check requires `FLOOR + STACK + 1024` free. The poll task's
3072 B internal stack is allocated after that check, so internal free memory
can fall to about 8192 + 1024 - 3072 = 6144 B, below the owner's
`heap_internal min_free >= 8192 B` floor. In the recovery image this means
less margin for the NVS writes the poll task does on failure.

A failed `xTaskCreate` is only logged. If it fails, an apply failure records
no result and shows no "INSTALL FAILED" on the LCD; the request then stays
with attempts raised.

Fix: include `POLL_TASK_STACK` in the apply start-check (or create the poll
task first). On a failed create, record the result in the boot path.

### LOW-5: MCP worst-case wall time is above the 300 s client abort

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:578-608`

Worst case: GET 8 s + POST 30 s + deadline up to 280 s (with the 15 s initial
sleep inside it) + a final 5 s status timeout + 5 s sleep, about 328 s. The
MCP client aborts a call after 300 s of silence.

The board keeps installing either way, and the fallback text already tells
the operator to re-run with `confirm=False`. The cost is a lost verdict, not
a wrong one. Fix: clamp the deadline to about 240 s, or add a background job
twin like `bench_test_start`.

### LOW-6: a POST timeout is reported as FAILED although the install may be running

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:588-590`

Scenario:

1. `start_install` writes the request, sends 200 and reboots.
2. The response is lost: the board drops off Wi-Fi while sending, or the
   connection is reset.
3. `_request` raises `UpdateHttpError`, and the tool returns
   `FAILED: install refused or lost`. Meanwhile the board is in recovery
   installing.

The wording says "or lost", but FAILED is a stronger verdict than the evidence
supports. A transport error (as opposed to an HTTP 4xx/5xx refusal) should be
reported as UNKNOWN, followed by the same polling.

### INFO-1: a refusal that never touched the app still leaves the board in recovery

`otadata` was erased by `recovery_enter`. A refusal before any erase (stage
re-hash, NO_MEM, record bad, size or sha mismatch) leaves an intact,
verified app that is not selected. Recovery out of this needs the SoftAP
passphrase from the LCD and `recovery_exit`. The "bounded retry" happens only
after an external reboot.

Suggestion: when `app_modified == false` and the app image verifies, select
the app automatically (the same steps as `recovery_exit`, after clearing
boot_guard) so an unattended install never strands the board.

### INFO-2: power loss between the request clear and set_boot

Scenario: power is lost after `req_clear` and before `set_boot` (the gap
between `update_install_record.c:156` and the set in `cb_set_boot`). The board
comes back with a verified new app, no request, and otadata still pointing at
factory, so it boots into recovery. Nothing is lost; `recovery_exit` finishes
the job.

### INFO-3: a failed OK result write makes MCP report UNKNOWN on success

Location: `firmware/KilnFW_recovery/main/recovery_install.c:155-157`

When the OK result write fails, the install has still succeeded but the app
sees `last_install.state == none`. The MCP tool reports UNKNOWN. This is
honest, and acceptable.

### INFO-4: char json[448] on the httpd stack

Location: `firmware/KilnFW/App/drivers/update/update_install_http.c:212`

This relies on the httpd stack-ceiling checks. Those are SKIP-FAST in a `-Fast`
run, so only a full run or a build with an ELF measures it.

### INFO-5: flash_firmware set_otadata_app (`f263ebe8f`)

The opt-in writes both otadata sectors with `ota_seq=1` and the IDF CRC
(cross-checked against otatool in the tests), in the same OpenOCD session,
with verify. The default stays False, so the deliberate otadata gap described
in CLAUDE.md is unchanged unless a caller opts in.

## Test gaps

- The range check on the result record's `code` (`update_install_record.c:101`)
  is never reached, because the codec test tampers a byte under the CRC and
  the CRC check fails first. See M1 below.
- No host test covers the recovery-side glue (`recovery_install.c`), the
  pre-boot hook wiring, the poll task, or the `recovery_exit` / manual apply
  interaction with a leftover request (LOW-1/LOW-2).
- `update_install_prepare` refusing an empty semver or a zero image length
  (`update_install_record.c:270`) has no test. Dropping either check is not
  caught (M3, M4). For length 0 the encode refuses later (`:32`) and the
  route answers 500 instead of 409; an empty semver is accepted and written.
- The result decoder's `reason` range check (`:101`) is untested for the same
  reason as `code` (M2).
- The magic and version checks in the decoders sit behind the CRC check.
  Mutating them gives equivalent mutants unless a test re-computes the CRC.

## Negative tests

Command, run from `C:\wt\rvautoapply_nkt2cd`:

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset kilnfw-host -Mutations aa_mut.json -Parallel 2 -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
```

All mutations are in `firmware/KilnFW/App/drivers/update/update_install_record.c`.

Baseline passed (287 s). Every mutant compiled. Each CAUGHT verdict comes
from failing test assertions (`FAIL ...test_update_install_record.c:<line>`),
not from a build failure. negtest reported `real_tree_unchanged: true`, and
`git status` was clean in the worktree and in the main tree afterwards.

| # | Mutation | Verdict | Failing assertions |
|---|----------|---------|--------------------|
| M1 | result decode: drop `r.code > UPD_INST_RES_FAIL` | MISSED | none (exit 0) |
| M2 | result decode: drop the `reason` range check | MISSED | none (exit 0) |
| M3 | prepare: drop `req.semver[0] == ' '` | MISSED | none (exit 0) |
| M4 | prepare: drop `req.image_length == 0` | MISSED | none (exit 0) |
| M5 | boot_run: `attempts >= MAX` becomes `>` | CAUGHT | :223, :225, :226 |
| M6 | pre_boot: skip the req_clear failure check | CAUGHT | :315, :323, :329, :330, :345 |
| M7 | prepare: skip the result-erase failure check | CAUGHT | :390, :418 |
| M8 | boot_run: skip the sha compare | CAUGHT | :240, :242 |
| M9 | boot_run: no attempt counter bump | CAUGHT | :209, :212, :218, :221, :223, :225, :226 |
| M10 | boot_run: skip the size check | CAUGHT | :249 |

6 of 10 caught. The safety-relevant boot path (counter, bound, sha, size,
request clear before boot switch, stale result erase) is all pinned. The four
misses are the input-validation gaps listed under Test gaps. None of them can
brick the board: the boot path re-checks size and sha against the stage.

## Recommendations

1. Clear the install request (and write a superseded result) in
   `recovery_exit_post` and in the manual apply pre-boot, or erase it when the
   app boots healthy (LOW-1, LOW-2).
2. Count the poll task's stack in the apply start-check, and record a result
   when its create fails (LOW-4).
3. Make `s_running` an atomic claim (LOW-3).
4. In the MCP tool, report a transport error from `start_install` as UNKNOWN
   and keep polling; clamp the total wait under 300 s (LOW-5, LOW-6).
5. Add host tests for M1-M4: decode records with a recomputed CRC and an
   out-of-range code/reason, and prepare with an empty semver or length 0.
6. Consider auto-selecting an intact, unmodified app after a refusal so an
   unattended install never leaves the board in recovery (INFO-1).

## Fix status (2026-10-13)

Commits (worktree autoapplyfx1, branch from origin/dev): `0388de1f0` (firmware
recovery glue + codec tests), `f8bc24c62` (prepare tests), `92dea4204` (MCP).

| Finding | Status |
|---|---|
| LOW-1 stale request re-applied | Fixed: `recovery_install_supersede()` clears req and res on `recovery_exit_post` and before a manual apply. Target-only, no host test. |
| LOW-2 stale failure result | Fixed: same supersede call. Target-only, no host test. |
| LOW-3 `s_running` test-and-set | Fixed: `__atomic_compare_exchange_n` claim, released on start failure. Target-only. |
| LOW-4 poll stack / create failure | Fixed: `APPLY_POLL_STACK` counted in the internal-heap start check; poll-task create failure records FAIL/APPLY_FAILED and shows it. Stack-margin registration kept; `check_stack_margin_registration.ps1` passes. Target-only. |
| LOW-5 MCP over 300 s | Fixed: deadline 270 s from entry (GET, POST, settle sleep and polls all inside it); message says still in progress, poll with `update_install_staged(confirm=False)`. |
| LOW-6 POST timeout reported FAILED | Fixed: transport failure (no HTTP status) is UNKNOWN/possibly applied and polling continues; a board refusal still FAILED. |
| Test gaps M1-M4 | Fixed in `test_update_install_record.c`. |

Negtest (LOW-1..4 recovery glue has no host harness, so none was run for it):

| Mutant | Result |
|---|---|
| M1 drop result code range check | CAUGHT (`:188`) |
| M2 drop result reason range check | CAUGHT (`:193`) |
| M3 drop empty-semver refusal in prepare | CAUGHT (`:428`) |
| M4 drop zero-length refusal in prepare | CAUGHT (`:435`) |
| M5 POST transport error treated as refusal | CAUGHT (pytest) |
| M6 wait cap 270 -> 2700 | CAUGHT (pytest) |
| M7 deadline measured after the POST | CAUGHT (pytest) |
