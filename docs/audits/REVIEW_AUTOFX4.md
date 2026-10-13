# Review: AUTOFX4, fixes for REVIEW_AUTOFX3: 0 HIGH, 0 MED, 3 LOW, 4 INFO (2026-10-13)

Adversarial review of the origin/dev commits that fix `REVIEW_AUTOFX3.md`:

- `55718f506` (code, check): uptime interval bracketing in
  `update_install_staged`, `_PENDING_TTL_S` expiry of the pending record,
  UNKNOWN wording for an unproven `confirm=False` follow-up; reason-name
  parser and enum count, top-level supersede rule in
  `check_recovery_status_fragments.ps1`.
- `72cee739b`, `2c79a9204` (tests: `SlowReadTests`, no `subTest`).
- `0067be3a9` (REVIEW_AUTOFX3 fix status).

Reviewed in a worktree at `0067be3a9`. No board access. Method: source
reading of `tools/PcTools/src/kilnctrl/mcp_server_update.py`, its tests,
`dashboard_http.c`/`dashboard_status_http.c` (where `uptime_s` comes from),
`update_install_record.[ch]`, `recovery_http.c`, `recovery_apply.c`; a direct
run of the check; `tools/negtest.ps1` (table at the end).

## Questions

| Question | Answer |
|---|---|
| Is `elapsed` a lower bound on board uptime advance, first poll? | Yes. `t_pre` is taken after the `before_up` read returned (the board sampled before it); "now" is taken before the after-read is sent (the board samples after it). `uptime_s` is computed per request (`dashboard_status_http.c:329` calls `dashboard_get_status()`, which reads `hal_time_now_us()` at `dashboard_http.c:538`), not from a cached snapshot, so the bound holds |
| Later polls? | Same function, same `t_pre`: yes |
| Follow-up `confirm=False` using the saved record? | Yes: `elapsed = now - pend["t_pre"]` in the new call, same ordering. A different MCP process has no record (falls back, see LOW-2) |
| Without a reboot, can the proof pass? | No. With `before_up = floor(B)`, `after = floor(B + D)` and `elapsed <= D`, `after > before_up + elapsed - 1`, so `after + 2 < before_up + elapsed` cannot hold. The slack only has to be at least 1 s; 2 s is safe. Below 1 s it is not, and no test pins that (LOW-3) |
| Any remaining way to report `ok - installed` without a reboot after the POST? | Not without a reboot. But any reboot after the POST counts, including one that is not the install (LOW-1). The other two `ok` paths (`post_unknown` False; running version changed to the requested one) each need either an answered POST (which erased the old record) or a new image booting |
| Can a genuine install come back UNKNOWN forever? | Not forever. Before the TTL: same version, POST unanswered and `before_up` unreadable stays UNKNOWN until the record expires. After 600 s, or after an MCP restart, the follow-up returns the plain `REFUSED: would install the staged image ... pass confirm=True` (LOW-2). Never a false `ok`. After a successful apply the stage header is erased (`recovery_apply.c:176`), so a `confirm=True` sent on that advice refuses "no verified stage ready"; after a failure that left the stage, it would install again |
| Is the "POST arrived" proof (`last_install.state == "none"`) still needed? | Its purpose still is (LOW-1), but that particular observation is not workable: `install_prepare` erases the result just before the board reboots into recovery, and polling starts 15 s after the POST, so the app API is normally never seen in the "none" state. Requiring it would make every unanswered same-version install unprovable. A firmware-side identity is the real fix (LOW-1) |
| New tests real? | Yes. `FakeClock` drives `time.monotonic`/`time.sleep` (advances 0.01 s per read); `SlowReadTests` advances it before and after sampling inside the fake status read. The new tests use no `subTest`; the slow-read loop stops at the first failing case and names it. P1, P2 (the two bracketing moves) are each caught by a separate case. The `subTest` blocks left in `RebootEvidenceTests` are older and still fail the test under pytest (P6 shows 9 failed) |
| Check false positives on current source? | None: PASS, 12 names = 12 enum entries, worst case 154 B of 200. Fragile shapes listed in INFO-1 |

## LOW-1: any reboot after the POST still proves the install; a reset that lost the POST is the likely one

`_ok_is_new` accepts a same-version `ok` after an unanswered POST once the
uptime dropped. The TTL narrows the window to 600 s, but inside it the reboot
need not be the install. The correlated case: the board resets while handling
the POST, before `install_prepare` runs (panic, watchdog, brown-out). The POST
gets no answer (`post_unknown`), the stale `recorded/ok` for the same semver
stays, the board comes back in the application with a low uptime, and the
first poll reports `ok - installed`. The reset that made the POST unanswered
is the same reset that "proves" the install. `test_same_version_ok_accepted_when_uptime_proves_reboot`
in fact encodes this: it reboots the fake board without any install and
expects `ok`.

Consequence: a same-semver dev build reported installed while the old build
still runs. Not a safety hazard.

Fix: give the result record an identity the PC can compare. The request
already carries the staged image's `sha256` (`update_install_req_t`); copy it
(or a short prefix, or an install sequence number) into
`update_install_res_t` and `last_install`, and have the tool require it to
equal the staged sha256 read before the POST. A stale record then never
matches unless it is the same image, where reporting `ok` is harmless. The
`state == "none"` check is not a usable substitute (see above).

## LOW-2: expiry and MCP restart fall back to "REFUSED ... pass confirm=True", with no word about the expired install

REVIEW_AUTOFX3 LOW-1 asked for "expired, cannot confirm". The fix drops the
record silently, so a follow-up after 600 s (or after a server restart)
prints `REFUSED: would install the staged image: running=..., staged=False
(...), last_install=recorded/ok/... -- pass confirm=True (exactly True) to
proceed`. That is the wording LOW-2 of the last review removed from the
pending path, and it appears even when `staged=False`. An agent that polled
on the UNKNOWN advice and was slow gets told to install. Safe after a
successful apply (stage erased, the install refuses), a second install after
a failed one. The UNKNOWN text also does not say the follow-up only works for
10 minutes.

Fix: keep a tombstone for an expired host and answer `UNKNOWN: the earlier
install of X can no longer be confirmed (record expired); read
last_install above`, without the confirm hint; mention the 10-minute window
in the UNKNOWN text.

## LOW-3: the slack boundary is not pinned; a slack below 1 s fakes a reboot and no test fails

Mutant P8 (`_UPTIME_SLACK_S = 0.5`) passes all 25 tests. With no reboot,
`B = 600.0`, `D = 10.99`, zero latency: `before_up = 600`, `after = 610`,
`elapsed = 10.99`, and `610 + 0.5 < 610.99` reports a reboot. The fake
status read uses `int(clock - boot_t)` at arbitrary fractions, so the suite
never lands on a truncation worst case.

Fix: a no-reboot test with the boot instant and clock placed so `B` is a
whole second and `D` is just under the next whole second (e.g. `boot_t` at
`.0`, before-read at `.00`, after-read at `+10.99`), asserting UNKNOWN; it
fails for any slack below 1.

## INFO

1. Check `check_recovery_status_fragments.ps1`: text-level as stated. C4
   (`#if 0` around the 4-space call, `#else int supersede_rc = 0;`) is
   MISSED. Shapes that would fail on correct code: the call reflowed onto a
   continuation line (`int supersede_rc =\n        recovery_install_supersede();`),
   a plain assignment `    supersede_rc = recovery_install_supersede();`
   (the prefix group allows only `int x =` or `(void)`), and any comment
   inside the `UPD_INST_REASON` enum that names another `UPD_INST_REASON_*`
   token (inflates the enum count). The `"unknown"` fallback after
   `return reason` is outside the parsed region, so it does not count toward
   the worst case; it is 7 chars today.
2. REVIEW_AUTOFX3 INFO-2 is still open: the uptime read inside the poll loop
   (5 s timeout) is not charged to the 270 s deadline.
3. The REVIEW_AUTOFX3 fix status cites `75ff29b39`, a pre-rebase SHA on no
   remote branch; the landed commit is `55718f506`.
4. Confirmed sound: the bracketing comment matches the code; the TTL is
   checked before `_ok_is_new`, so an expired record cannot produce `ok` or
   `FAILED`; the pending path no longer mentions `confirm=True`.

## Negative tests

All runs: baseline passed, `real_tree_unchanged: true`, copies removed, base
`0067be3a9`. Each pytest CAUGHT log was checked: the mutant imported, the
listed tests failed with `AssertionError`, no collection error. Each check
CAUGHT is the check's own `FAIL:` line with exit 1.

| # | Mutant | Runner | Verdict | Evidence |
|---|---|---|---|---|
| P1 | `t_pre` taken before the `before_up` read | pytest | CAUGHT | `SlowReadTests::test_slow_reads_without_reboot_are_not_ok` |
| P2 | `elapsed` taken after the after-read | pytest | CAUGHT | `SlowReadTests::test_slow_reads_without_reboot_are_not_ok` |
| P3 | TTL check replaced by `False` | pytest | CAUGHT | `FollowUpTests::test_stale_pending_record_does_not_yield_installed` |
| P4 | `_PENDING_TTL_S = 1e9` | pytest | CAUGHT | same |
| P5 | `_PENDING_TTL_S = 280.0` | pytest | CAUGHT | `FollowUpTests::test_follow_up_reports_ok_once_new_version_runs_and_reboot_proven` |
| P6 | `_ok_is_new` returns True instead of `_reboot_proven` | pytest | CAUGHT | 9 failed (RebootEvidence, SlowRead, FollowUp) |
| P7 | pending follow-up back to `_refuse_unconfirmed` | pytest | CAUGHT | `FollowUpTests::test_unproven_follow_up_is_pending_not_refused` |
| P8 | `_UPTIME_SLACK_S = 0.5` | pytest | MISSED | LOW-3 |
| C1 | 13th reason name, enum unchanged | check | CAUGHT | `FAIL: reason-name parser found 13 names but the enum has 12` |
| C2 | supersede moved after the boot set | check | CAUGHT | `FAIL: ota_esp_post does not call recovery_install_supersede() as a top-level statement` |
| C3 | supersede inside `if (0) { }` | check | CAUGHT | same |
| C4 | supersede inside `#if 0` at 4-space indent | check | MISSED | INFO-1 |
| C5 | longest reason name with digits, 154 chars | check | CAUGHT | `FAIL: last_install fragment worst case 466 B does not fit frag[200]` |

Tally: 11 CAUGHT, 2 MISSED of 13.

Commands (from the worktree):

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest -PresetArg tests/test_mcp_server_update_install.py -Mutations py.json
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check -PresetArg firmware/KilnFW_recovery/main/check_recovery_status_fragments.ps1 -Mutations ck.json
```
