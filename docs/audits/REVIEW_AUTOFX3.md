# Review: AUTOFX3, fixes for REVIEW_AUTOFX2: 0 HIGH, 1 MED, 3 LOW, 4 INFO (2026-10-13)

Adversarial review of the origin/dev commits that fix `REVIEW_AUTOFX2.md`:

- `9073ff25a` (code, tests, new check): uptime-based reboot proof in
  `update_install_staged`, follow-up evidence per host, the split
  `last_install` fragment, the LCD warning line, supersede before the boot set
  in `ota_esp_post`, `check_recovery_status_fragments.ps1`.
- `b861d46ff`, `53e1d361c` (REVIEW_AUTOFX2 fix status), `3f001c6c7`,
  `2e02986d2` (gate evidence row and counts).

Reviewed in a worktree at `53e1d361c`. No board access. Method: source
reading, a scratch pytest (placed under `tools/PcTools/tests` for the
conftest's hub/network guard, deleted before commit) that drives
`update_install_staged` on the existing virtual clock with a slow
`GET /api/status`, byte counts of the recovery status fragment, and
`tools/negtest.ps1` (table at the end).

## Claims checked

| Claim | Verdict |
|---|---|
| Clock source for `elapsed` | `time.monotonic()`. Correct source, but the interval is bracketed the wrong way round (MED-1) |
| `before_up` read close to the POST | Yes: read right after the initial status GET, immediately before `start_install`. `t_pre` is taken before that read, which is the MED-1 problem |
| Uptime read fails before or after | Before: `before_up` is None, `_reboot_proven` is False, a same-version unanswered POST stays UNKNOWN. After: False for that poll, the next poll retries. Both pinned by tests (P4 below) |
| Uptime wrap or reset fakes it | No wrap: `uptime_s = (uint32_t)(hal_time_now_us() / 1000000)` (`dashboard_http.c:538`), 64-bit source, uint32 seconds wraps after 136 years. Any reboot counts as proof, though, including one unrelated to the install (LOW-1) |
| Recovery-image uptime read instead of the app's | No. The recovery image registers no `/api/status` (its uptime is only in `/api/recovery/status`), so the read fails with 404 and gives None. `_ok_is_new` also needs `running_version` from the app-only `/api/update/status` first |
| Two reboots (recovery, then app) | Proof holds: the app's uptime since its own boot is lower than `before_up + elapsed`. The pending entry is kept per host across calls, so a late follow-up still compares against the pre-POST value |
| Follow-up `confirm=False` reports ok on a real success | Yes. It reads `_PENDING_INSTALL[host]` stored by the timed-out call (`before_up`, `t_pre`, `before_running`, `post_unknown`), so the evidence is the original call's. A different-version install needs no uptime proof (`before_running != want`). The entry lives only in the server process: after an MCP restart the follow-up falls back to the plain "would install" refusal, which is safe. See LOW-1 and LOW-2 for the follow-up's other paths |
| UNKNOWN text says to poll again; cap still 270 s | Text holds. The 270 s deadline is still the loop bound, but the uptime read after the last poll can run past it (INFO-2) |
| LOW-1 arithmetic | Holds. Fragment literal is 52 B; worst case 52 + 4 (`fail`) + 34 (`outcome_unknown_do_not_power_cycle`) + 31 (`UPD_INST_SEMVER_LEN 32u` minus NUL; `ir_ver` is copied into a 32 B buffer and terminated at index 31) = 121 B plus NUL, of 200. The check's bound (each `%s` at the longest of the three) is 154 B, conservative. The numeric fragment alone is well under 200. The check's name parser has a gap (LOW-3) |
| LOW-2: SSID and passphrase stay visible, banner clears | Holds. The warning is drawn on `Y_NOTE` only; the network branch still draws SSID and passphrase. The panel is 480 x 320 landscape: `Y_NOTE` 290 + 16 px glyph = 306 < 320; 18 chars x 12 px = 216 of 480 px; `s_warn[28]`. Nothing new logs the passphrase: `recovery_lcd_set_warning` logs nothing and is passed a literal. `recovery_apply_busy()` reads a volatile flag without a lock, so calling it under `s_lock` from the LCD task has no lock-order risk. On success the apply restarts; on failure `s_running` goes false and the 1 Hz task removes the line (INFO-3) |
| LOW-3: supersede before the boot set, failure reported | Holds: `recovery_install_supersede()` now runs before `recovery_boot_partition_set_and_verify()` and its rc is in the response body (`body[320]` fits the longest combination). Only a text-level check covers it (INFO-1) |
| New check not vacuous; gate row added | Not vacuous for its five recorded mutants and two of mine (C1, C2), but a reason name with a digit escapes it (LOW-3). Gate row and counts (207 rows, 195 NEGATIVE-TESTED) are consistent |

## MED-1: request latency on the two uptime reads fakes a reboot, so a stale `ok` is reported as installed

`tools/PcTools/src/kilnctrl/mcp_server_update.py`, `update_install_staged`
and `_reboot_proven`. `t_pre = time.monotonic()` is taken **before** the
`before_up` GET is sent, and `elapsed = time.monotonic() - t_pre` is taken
**after** the after-read has returned. So `elapsed` includes the time the
first request took to reach the board (d) and the time the second answer took
to come back (r). Neither is time the board's uptime counted between the two
samples. With no reboot, `after = before_up + elapsed - d - r` (give or take
the 1 s truncation of each reading), so the test
`after + 2 < before_up + elapsed` passes as soon as d + r is more than about
1 to 3 s, depending on where the truncations fall.

Scenario: the owner reinstalls a dev build of the same semver (different
commit, staged with `force`, which `update_policy.c` allows). So
`running_version == staged_version` before the POST, and `last_install` holds
`recorded/ok` for that version from the earlier install. The Wi-Fi link is
poor. The POST is lost before it reaches the board (`post_unknown`; the board
never runs `install_prepare`, so the stale record is not erased). The
pre-POST uptime GET took 1 s to reach the board, and an after-read answer
takes 1 s to come back. The first poll reads the stale `ok`, `_reboot_proven`
returns True, and the tool reports `ok - installed '1.0.1' and running in the
application`. The board never left the application. This is the false
positive that REVIEW_AUTOFX2 MED-1 was about, still open through a different
door. A poor link is the usual cause of `post_unknown`, so slow reads are
likely exactly when this gate matters.

Reproduced with the scratch test, on the suite's virtual clock (fake status
read that advances the clock by d before sampling and r after):

| d (s) | r (s) | result |
|---|---|---|
| 0 | 0 | UNKNOWN |
| 1.0 | 1.0 | ok - installed |
| 1.5 | 1.5 | ok - installed |
| 2.5 | 0.5 | ok - installed |
| 3.0 | 0 | ok - installed |

Fix: bracket the interval conservatively. Take `t_pre` when the `before_up`
read has returned, and take the "now" for `elapsed` before the after-read is
sent. Then `elapsed` can only under-count the time between the two samples,
and request latency can no longer look like a reboot. The 2 s slack then only
has to cover truncation. Add a test with a slow status read (the existing
`_status` fake answers in zero virtual time, which is why the suite did not
see this). A boot identity that does not depend on timing (a boot counter or
a random boot id in `/api/status`) would remove the problem entirely.

## LOW-1: the pending evidence never expires, and any reboot counts as proof of the install

`_PENDING_INSTALL[host]` is removed only on `ok` or `FAILED`. An UNKNOWN
leaves it in place for the life of the MCP server process. A later
`update_install_staged(confirm=False)`, which is also the ordinary "show me
the install status" read, then reports
`ok - installed ... (confirmed by a follow-up read)` as soon as any reboot has
happened since `t_pre`: a power cycle, a `sw_reset` by another agent, a
watchdog reset or a JTAG flash.

Scenario: same-version reinstall, POST lost before it reached the board, the
call returns UNKNOWN (correctly). The next day another agent reflashes or
resets the board. The operator runs `update_install_staged(confirm=False)` to
check status and is told the install from yesterday succeeded. Reproduced
with the scratch test: first call UNKNOWN, follow-up after
`clock += 86400; reboot()` returns `ok - installed '1.0.1' ... (confirmed by
a follow-up read)`.

Fix: expire the entry after a bound longer than the install (say 10 minutes
past `t_pre`, then report "expired, cannot confirm"). Better, also require
evidence that the board took the request, for example a poll that saw
`last_install.state == "none"` (`install_prepare` erases the result before it
writes the request, so an erased record proves the POST arrived).

## LOW-2: the follow-up "still unconfirmed" answer tells the caller to pass `confirm=True`

When a pending entry exists but nothing is proven yet, the `confirm=False`
path returns `_refuse_unconfirmed(...)`, which is
`REFUSED: an earlier install of '1.0.1' is still unconfirmed ... -- pass
confirm=True (exactly True) to proceed`. The UNKNOWN text from the call
before says the opposite: poll again with `confirm=False`.

Scenario: an agent follows the UNKNOWN advice and polls while the board is
back in the application but the proof is not there yet (e.g. MED-1's mirror:
no slow read, uptime still counting). The answer it gets starts with
REFUSED and tells it to pass `confirm=True`. A literal agent does, and
sends a second install POST: a second recovery round trip and, for a same
version, another unproven `post_unknown` cycle.

Fix: return a distinct non-REFUSED status for this case (e.g.
`PENDING: ... poll again with update_install_staged(confirm=False)`), and
pin the wording in a test.

## LOW-3: the check's reason-name parser skips any name containing a digit or capital

`check_recovery_status_fragments.ps1` collects reason lengths with
`'"([a-z_]+)"'`. A quoted name with any other character (`sha256_mismatch`,
`ERR_X`) is not matched and silently does not count toward `maxReason`.

Scenario: a future reason `outcome_unknown_do_not_power_cycle_0_0_...`
(154 chars, mutant C3) gives a real worst case of 52 + 4 + 154 + 31 = 241 B,
which overflows `frag[200]` and cuts the status stream. The check computes
its bound from the remaining names (longest 19 chars, `maxStr` 31) and passes.
Mutant C3 was MISSED. The same name made of letters only (C1) is CAUGHT.

Fix: match `'"([^"\\]*)"'`, and fail the check unless the number of names
found equals `UPD_INST_REASON_COUNT`, so a parse that drops an entry cannot
pass.

## INFO

1. The supersede guard is text-level. Mutant C4
   (`int supersede_rc = 0; if (0) { (void)recovery_install_supersede(); }`)
   keeps the call text before the boot set and the `supersede_rc == 0`
   report, so the check passes while no supersede ever runs. The fix status
   already says there is no behavioural harness; this records that the check
   cannot stand in for one.
2. Call budget: inside the poll loop, `_ok_is_new` calls `_read_uptime`
   (5 s timeout) after `get_install_status` (5 s timeout). A poll started just
   before the 270 s deadline can therefore run about 10 s past it (more if
   connect and read each use the timeout), so the docstring's "total call
   under 280 s" is no longer a bound and the margin to the client's 300 s
   silence limit is smaller than before. Skip the uptime read when the
   deadline has passed, or charge it to the deadline.
3. When the apply fails without a poll task, the warning is removed within
   1 s and the LCD then shows nothing about the failure; the record says
   `fail/outcome_unknown_do_not_power_cycle`. If the apply fails before
   `recovery_lcd_set_warning` runs, the line is shown for under 1 s.
   REVIEW_AUTOFX2 LOW-2 suggested replacing the banner with a done/failed
   line; only the clearing half was done. Not a hazard: the AP stays up and
   `/api/recovery/status` has the record.
4. Confirmed sound: an answered POST cannot be followed by a stale `ok`
   (`install_prepare` erases `UPD_INST_KEY_RES` before writing the request),
   so `_ok_is_new`'s `not post_unknown` shortcut is right; `json_safe`/`ir_ver`
   filtering keeps the version JSON-safe; `body[320]` in `ota_esp_post` fits
   the longest message.

## Negative tests

All runs: baseline passed, `real_tree_unchanged: true`, copies removed, base
`53e1d361c`. Each pytest CAUGHT was checked in its log: the mutant imported,
tests failed with `AssertionError`, no collection `ERROR`. Each check CAUGHT
is the check's own `FAIL:` line with exit 1.

| # | Mutant | Runner | Verdict | Evidence |
|---|---|---|---|---|
| P1 | `_UPTIME_SLACK_S = 1000.0` (reboot never proven) | pytest | CAUGHT | 4 failed, e.g. `FollowUpTests::test_follow_up_reports_ok_once_new_version_runs_and_reboot_proven` |
| P2 | follow-up `FAILED` return replaced by `pass` | pytest | CAUGHT | `FollowUpTests::test_follow_up_reports_recorded_failure` |
| P3 | `_UPTIME_SLACK_S = 0.0` (no truncation slack) | pytest | CAUGHT | 5 failed, e.g. `RebootEvidenceTests::test_uptime_that_keeps_counting_is_not_a_reboot` |
| P4 | pending entry stores `post_unknown: False` | pytest | CAUGHT | 7 failed, e.g. `test_same_version_ok_not_accepted_when_uptime_unreadable_before` |
| C1 | longest reason name lengthened to 154 letters | check_recovery_status_fragments | CAUGHT | `FAIL: last_install fragment worst case 514 B does not fit frag[200]` |
| C2 | `UPD_INST_SEMVER_LEN 200u` | check_recovery_status_fragments | CAUGHT | `FAIL: last_install fragment worst case 649 B does not fit frag[200]` |
| C3 | longest reason name lengthened to 154 chars with digits | check_recovery_status_fragments | MISSED | LOW-3 |
| C4 | supersede call made dead (`if (0)`) | check_recovery_status_fragments | MISSED | INFO-1 |

Tally: 6 CAUGHT, 2 MISSED of 8.

Commands (from the worktree):

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest -PresetArg tests/test_mcp_server_update_install.py -Mutations py3.json
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check -PresetArg firmware/KilnFW_recovery/main/check_recovery_status_fragments.ps1 -Mutations rec3.json
```

## Fix status (2026-10-13)

Fixed in `75ff29b39` (code, check) with test follow-ups in the two commits after it on the same push:

- MED-1: `t_pre` is taken after the `before_up` read returns; `elapsed` is taken before the after-read is sent, so request latency can only under-count. `SlowReadTests` (virtual clock, slow before-read, slow after-read, slow both) must not return ok; a real reboot with slow reads still does.
- LOW-1: pending records expire 600 s after `t_pre` (`_PENDING_TTL_S`, past the 270 s poll cap) and are dropped on expiry. Not done: the `last_install.state == "none"` proof that the POST arrived.
- LOW-2: the unproven follow-up answers `UNKNOWN: ... still pending ... poll again with update_install_staged(confirm=False)`, no REFUSED, no confirm=True.
- LOW-3: reason names are parsed with `"([^"\]*)"` and the check fails unless the count equals the `UPD_INST_REASON_*` enum size.
- INFO-1: the supersede call must be a top-level statement of `ota_esp_post` (4-space indent), so `if (0) { ... }` fails. A deeper dead-code guard would need a C parser and is not attempted.

Negtest: pytest preset 4/4 CAUGHT (t_pre before read, elapsed after read, no TTL, REFUSED wording); check preset 3/3 CAUGHT (digit-bearing long name, old narrow regex plus that name, dead supersede).
