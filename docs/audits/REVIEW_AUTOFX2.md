# Review: AUTOFX2, fixes for REVIEW_AUTOFX: 0 HIGH, 1 MED, 4 LOW, 5 INFO (2026-10-13)

Adversarial review of the commits on origin/dev that fix `REVIEW_AUTOFX.md`:

- `e8a6ad365` (code): MCP `update_install_staged` POST-unknown handling, the
  auth-refusal shortcut, `ota_esp_post` supersede, the new install reason
  `UPD_INST_REASON_OUTCOME_UNKNOWN`, the shared `RECOVERY_INSTALL_POLL_STACK`.
- `7974bd6de` (doc): REVIEW_AUTOFX fix status.

Reviewed in a worktree at `7974bd6de`. No board access. Method: source reading,
a scratch model of the MCP tool's decision path (stubbed `uhc`, no board
writes; importing `mcp_server_core` attached to the already-running link hub
as a client and printed "link hub already running elsewhere", nothing was
sent), byte counts of the recovery status fragment, and `tools/negtest.ps1`
(table at the end).

## Claims checked

| Claim | Verdict |
|---|---|
| MED-1: after a POST with no answer, `ok` is accepted only once a poll saw the board unreachable | Implemented as written, but "unreachable" is any poll error, so the residual stays open (MED-1 below) |
| MED-1: the changed test is not weakened | Holds. `test_post_timeout_is_unknown_then_follow_up_decides` still asserts `ok`; the down poll it gained is what the new gate needs. It is a different-version case (1.0.0 to 1.0.1), so the new gate does not change its outcome |
| LOW-1: an `HttpAuthError` cause gives FAILED at once, no polling | Holds and is sound: `http_auth.urlopen` raises `HttpAuthError` only after a 401 (the handler never ran), so "not sent" is true. The message says "authentication refused" also when the relogin failed on transport (`_login` raises `from None`); the verdict is still right (INFO-3) |
| LOW-2: `ota_esp_post` supersedes after the boot partition is set | Present, but the order differs from `recovery_exit_post` and leaves a replay window (LOW-3 below) |
| LOW-3: new reason `UPD_INST_REASON_OUTCOME_UNKNOWN` | Enum, name table, range check and host test are consistent. LCD strings fit (19 chars x 12 px = 228 px of 480; buffers 24/28). App `/api/update/status` fits (worst case 322 of 448 B). But the 34-char name overflows the recovery status fragment (LOW-1 below). No web page JS reads `last_install`; PcTools consumers are `update_http_client`/`mcp_server_update` (code `fail` maps to "FAILED: install recorded a failure") |
| LOW-5: shared `RECOVERY_INSTALL_POLL_STACK` | Holds: one define (3072) in `recovery_apply_esp.h`, used by both `APPLY_POLL_STACK` and `recovery_install.c` |

Answers to the two questions:

1. **Can one transient failed poll then a stale ok still give a false positive?**
   Yes. See MED-1.
2. **Is "unreachable" ever observed when the POST never reached the board?**
   Yes. The same outage that lost the POST usually fails the next poll too, and
   an HTTP 503/500 or a relogin refusal on a live board also sets `saw_down`.

## MED-1: `saw_down` is set by any poll error, so a stale ok is still reported as installed

`tools/PcTools/src/kilnctrl/mcp_server_update.py:606-617`. `saw_down = True`
on any `uhc.UpdateHttpError` from a poll. `uhc._request` wraps every failure
into that type: transport errors, HTTP errors with a status (503, 500, 409),
JSON decode errors and `HttpAuthError`. None of them proves the board rebooted.

Scenario: same-version reinstall (or a retry of an install that already
completed earlier). `before` shows `running == want` and
`last_install = recorded/ok/<want>` from the earlier install. The PC's Wi-Fi
drops for 10 s; the POST fails with no status (`post_unknown`). The first poll
after the 15 s settle also fails on the same outage (`saw_down`). The next poll
reads the unchanged stale record, `running == want`, code `ok`, and the tool
returns "ok - installed". The board never entered recovery. The scratch model
reproduced `ok` for three variants of the failed poll: a transport error, an
HTTP 503 from a live board, and a 401-style auth error.

The fix narrowed the window from "zero failed polls" to "one failed poll", but
the gate's premise ("board went down") is not what it measures.

Fix options, cheapest first:
- When `post_unknown` and `before` already showed `running == want` with a
  recorded ok for `want`, never return ok; return UNKNOWN with the stale note.
- Count only status-None, non-auth errors as down (excludes 503/500/401),
  and require the record to differ from `before` (code, reason or version).
- Best: compare a boot identity (`uptime_s` dropping, or a boot counter)
  from before and after, which is direct proof of a reboot.

Test gap: no test pins a failed poll that is a live-board HTTP error, and no
test pins a same-version stale record with a down poll.

## LOW-1: the 34-char reason name overflows the recovery status fragment

`firmware/KilnFW_recovery/main/recovery_http.c:609-616` builds the last
fragment of `GET /api/recovery/status` through `send_frag()`
(`recovery_http.c:388-398`, `static char frag[200]`; `n >= 200` returns
`ESP_FAIL`, the handler then closes the stream). That fragment carries
`free_heap` (7 digits: the recovery sdkconfig sets
`CONFIG_SPIRAM_USE_CAPS_ALLOC=y`, so PSRAM counts),
`heap_internal_min_free` (6 digits) and the reason name.

With `fail`/`outcome_unknown_do_not_power_cycle` the fragment is 191 B for
version `1.0.0`, 198 B for `1.0.0-pre.12`, 199 B for `1.0.0-pre.100`, and
overflows at a version of 14 chars or more, e.g. any dev build's
`git describe` version (`v1.0.0-pre.3-12-gabcdef0` gives 210 B). A nonzero
`nvs_failed_mask` or `"true"` shifts the limit down by a char or two.
`semver` can hold 31 chars.

Scenario: the poll task fails to start on a dev-build install. The record is
FAIL/outcome_unknown. The operator opens `/api/recovery/status` to see what
happened, and the stream is cut before `last_install`; the JSON is invalid,
so `recovery_status` and any parser see nothing. The diagnostic is lost
exactly when it is needed. The shape is pre-existing: `result_write_failed`
(19 chars) overflows at a version of 29 chars or more.

Fix: shorten the name (e.g. `outcome_unknown`), or split the fragment so
`last_install` goes out alone, and add a host or check-level test of the
worst-case length.

## LOW-2: the "DO NOT POWER-CYCLE" banner never resolves, and it hides the SoftAP passphrase

`firmware/KilnFW_recovery/main/recovery_install.c:248-256`: when the poll task
cannot be created, the record becomes FAIL/outcome_unknown and the LCD shows
"INSTALL UNMONITORED / DO NOT POWER-CYCLE". Nothing watches the apply phase
afterward (that was the poll task's job), so nothing ever clears or updates
the banner. If the apply fails and returns to idle, the LCD keeps saying not
to power-cycle indefinitely, beside "Use JTAG if it persists".

The error banner also replaces the whole network section
(`recovery_lcd.c:395-401`), so the SSID and per-boot passphrase are no longer
shown. The operator cannot join the SoftAP to read
`/api/recovery/status`, and the only on-screen advice is not to power-cycle.

Fix: keep the passphrase visible under this banner, or poll the phase from the
existing LCD refresh path and replace the banner with a done/failed line.

## LOW-3: `ota_esp_post` supersedes after the boot partition is set, unlike `recovery_exit_post`

`recovery_http.c:237-248`: boot partition set and verified, then
`recovery_install_supersede()` (result ignored), then
`stage_clear_after_upload()`. `recovery_exit_post` (`recovery_http.c:757-758`)
supersedes before it sets the boot partition.

Scenario: an operator pushes an ESP image over the recovery route while an
auto-install request is pending. Power is lost (or the supersede's NVS write
fails) between the boot set and the supersede. The pending request survives.
On the next recovery entry, `recovery_install_boot()` replays the verified
staged image over the image the operator just pushed. Not a brick (it applies
a verified image), but it silently undoes the operator's push. A failed
supersede is also not reported in the response body.

Superseding before the write would also drop a pending request when the
upload is then rejected; that is benign because the operator is present.

Fix: supersede after the upload verifies and before the boot set, as
`recovery_exit_post` does, and report a failed clear in the response.

## LOW-4: test gaps around the new gates

- No test pins that a POST that got an answer accepts ok without a down poll:
  `test_success_needs_version_and_ok` also contains a down poll. Mutant P3
  (`saw_down` required always) was MISSED.
- No test of the different-version `post_unknown` case with no down poll.
- The recovery glue is untested: removing the new `ota_esp_post` supersede
  (R1) was MISSED by `check_recovery_wifi_policy.ps1`; removing the exit
  route's supersede (R2) only tripped that check's own self-test anchor, not
  an assertion (see table). This carries the untested part of REVIEW_AUTOFX
  LOW-5 forward.

## INFO

1. If `pre_boot`'s OK result write fails, the persisted FAIL/outcome_unknown
   stays, so a good install reads `do_not_power_cycle` in the app and FAILED
   in the tool. A double fault, same shape as before `14ff316e0`.
2. The name's advice ("do not power cycle") outlives the event: the record
   persists after the board is back in the app, where the advice no longer
   applies.
3. The LOW-1 fix's message says "authentication refused" also when the
   relogin failed on transport (`_login` raises `HttpAuthError ... from None`).
   The verdict (not sent, FAILED) is still correct, since a 401 came first.
4. A different-version install that finishes within the 15 s settle sleep,
   with a lost POST answer, never shows a down poll and is reported UNKNOWN,
   not ok. A false UNKNOWN, unlikely, and safe.
5. P4's mutant run took 327 s: with the auth check disabled the tool falls
   into the real polling loop until its deadline. The test still fails by
   assertion, but a regression there would make this test slow rather than
   fast-failing; a mocked clock would bound it.

## Negative tests

All runs: baseline passed, `real_tree_unchanged: true`, copies removed, base
`7974bd6de`. Every CAUGHT below was checked in its log: the mutant compiled
(or imported) and a test assertion failed.

| # | Mutant | Runner | Verdict | Evidence |
|---|---|---|---|---|
| K1 | reason name shortened to `outcome_unknown` | kilnfw-host | CAUGHT | `test_update_install_record.c:201: outcome-unknown reason name` |
| K2 | decode range check `>= UPD_INST_REASON_OUTCOME_UNKNOWN` | kilnfw-host | CAUGHT | `test_update_install_record.c:211: outcome-unknown result round-trips` |
| P1 | `saw_down` never set | pytest | CAUGHT | 2 tests, AssertionError (`..._accepted_once_board_went_down`, `..._follow_up_decides`) |
| P2 | gate always open (`or True`) | pytest | CAUGHT | `test_stale_ok_after_post_transport_failure_is_not_success` AssertionError |
| P3 | `saw_down` required even when POST answered | pytest | MISSED | LOW-4 |
| P4 | auth check disabled (`if False:`) | pytest | CAUGHT | `test_auth_refusal_without_status_is_failed_and_does_not_poll` AssertionError (327 s, INFO-5) |
| P5 | stale note condition inverted | pytest | CAUGHT | `'stale' not found` AssertionError |
| R1 | drop `ota_esp_post` supersede | check_recovery_wifi_policy | MISSED | LOW-4 |
| R2 | drop `recovery_exit_post` supersede | check_recovery_wifi_policy | VOID (counted MISSED) | exit 1 only because the check's own built-in mutant could not find its anchor ("negative test exitplain: could not find ..."); `RESULT pass=61 fail=0`, no assertion failed |

Tally: 6 CAUGHT, 3 MISSED (P3, R1, R2) of 9.

Commands (from the worktree):

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset kilnfw-host -Mutations kf.json -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest -Mutations py.json
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check -PresetArg firmware/KilnFW_recovery/main/check_recovery_wifi_policy.ps1 -Mutations rec.json
```

## Fix status

Fixed in `5ee85aaac` (AUTOFX3), on origin/dev:

- **MED-1**: `saw_down` is gone. `update_install_staged` reads `uptime_s` (GET `/api/status`) before the POST and
  accepts an `ok` after an unanswered POST only when the uptime now plus 2 s of truncation slack is below the
  earlier uptime plus the elapsed time (a reboot happened), or when the version that was running before the POST
  differs from the requested one. A same-version `ok` with no proof (uptime unreadable, or still counting) stays
  UNKNOWN with the stale note. Pytest covers outage, 503 and auth-error poll failures, unreadable uptime before
  and after, and uptime that keeps counting.
- **Pooled bench finding** (pre.3 install reported UNKNOWN after the 270 s window): the UNKNOWN text now says the
  install may still be running (up to about 4 minutes) and to poll again with `update_install_staged(confirm=False)`.
  The tool keeps the pre-POST evidence per host, so that follow-up read reports `ok` once the new version runs with an
  `ok` record and the same reboot proof holds, or `FAILED` on a recorded failure. The per-call cap is still 270 s.
- **LOW-1**: the `last_install` JSON now goes out as its own `send_frag` fragment (worst case 154 B of the 200 B
  static buffer, no stack cost). `check_recovery_status_fragments.ps1` computes that worst case from the reason-name
  table and `UPD_INST_SEMVER_LEN`.
- **LOW-2**: the unmonitored-install notice is now a warning line (`recovery_lcd_set_warning`) on the note row, drawn in
  addition to the SSID and passphrase. The 1 Hz LCD task removes it when `recovery_apply_busy()` goes false. The
  passphrase is not logged anywhere new. The persisted FAIL/`outcome_unknown` record is unchanged (INFO-2 stays).
- **LOW-3**: `ota_esp_post` supersedes before it sets the boot partition and reports a failed supersede in its
  response body. The same check asserts the order.
- **LOW-4**: `test_answered_post_is_accepted_without_a_down_poll` pins mutant P3. The recovery supersede calls have no
  host-test harness; they are covered only by the source-level order/report assertions in
  `check_recovery_status_fragments.ps1`, not by a behavioural test.

Negative tests (base `5ee85aaac`, all CAUGHT by a failing assertion): pytest 7/7 (drop the uptime comparison, any poll
error counts as down, follow-up ignores evidence, answered POST needing proof, inverted comparison, UNKNOWN text,
version-change shortcut forced on); check script 5/5 (shrunk buffer, last_install merged back into the numeric
fragment, supersede dropped, supersede not reported, supersede after the boot set).

INFO-1..5 are unchanged. Not fixed: INFO-5 (slow P4 mutant) is addressed in passing, because the tests now run on a
virtual clock.
