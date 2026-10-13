# Review: relsuitefx4 (release bench suite, REVIEW_RELSUITEFX3 fixes)

Date: 2026-10-13. Reviewer: Opus. No board access.

Scope: code commit `bbfa0712d` ("release suite: pin real IPv4Address host via
_pin_host (keeps port), post-POST non-ok install results never cancel/clear,
inconclusive post-install fields INCONCLUSIVE, ...") and docs commit
`82531b514` (fix status in `REVIEW_RELSUITEFX3.md`), both on origin/dev.
Files: `tools/PcTools/src/kilnctrl/bench_test/cases_release.py`,
`tools/PcTools/tests/test_bench_test_cases_release.py`.

Result: 0 HIGH, 0 MED, 4 LOW, 5 INFO. Both FX3 MEDs are fixed on the real
code path.

## Checks requested

### (a) Host pin on the real code path: fixed

`_pin_identity()` now pins any non-empty `ip` through
`mcp_server_aux._pin_host(host, ip)`. The default identity function is the
real `mcp_server_aux._board_identity_check()`, which returns
`(None, IPv4Address)`. `_pin_host` reads the port with `urlsplit` and returns
`"<ip>:<port>"`, or `str(ip)` when there is no port. A probe returned
`10.0.0.1:8080` for `http://kiln.local:8080/x` and `10.0.0.1` for
`https://kiln.local`. The new test
`test_pin_identity_real_board_identity_check_ipv4address_keeps_port` calls the
real `_board_identity_check` with a stubbed UART Wi-Fi status and checks both
the `IPv4Address` type and that the port is kept. N1 and N2 below are CAUGHT
by it. The scheme is dropped (INFO-4).

### (b) Classification of every `update_install_staged` return

Source: `mcp_server_update.py:563`. The production `srv` is `mcp_server`, and
its `update_install_staged` is the `_core._tool()` wrapper
(`mcp_server_core.py:46`).

| Return prefix | Where | POST sent? | Fix classifies as | Correct? |
|---|---|---|---|---|
| `error: could not read GET /api/update/status` | before POST | no | pre-POST, cleanup | yes |
| `REFUSED: ... pass confirm=True` (`_refuse_unconfirmed`) | before POST, unreachable (case passes `confirm=True`) | no | pre-POST | yes |
| `REFUSED: no verified stage ready` | before POST | no | pre-POST, cleanup | yes |
| `FAILED: install refused or lost: HTTP <n> ...` | `start_install` raised `UpdateHttpError` | yes, or a definite 4xx | may-have-run | safe; a definite 4xx is over-conservative (LOW-4) |
| `ok - installed ...` | after POST | yes | ok path | yes |
| `FAILED: install recorded a failure` | after POST | yes | may-have-run | yes |
| `UNKNOWN: board not confirmed ...` | after POST | yes | may-have-run | yes |
| `error: <exc>` / `error: unexpected <T>: <exc>` from the `_tool` wrapper | any exception that escapes the tool, **including after the POST** | either | pre-POST, cleanup | **no** (LOW-1) |

The stale-server banner is appended at the end, so it never changes a prefix.

### (c) The two tests changed from `FAILED: nope` to `REFUSED: nope`: coverage not weakened

Under the new rule a `FAILED:` install skips cleanup. Without the change,
`test_ota_install_failed_clears_stage` would fail. More importantly,
`test_d01_every_update_call_carries_the_pinned_host` would skip `cancel` and
`clear`, and its host assertions on those two calls would pass with nothing
checked. With `REFUSED:` both tests still go through the cleanup path. N5
(REFUSED no longer pre-POST) is CAUGHT by both tests and by
`test_install_pre_post_refusal_cleans_up[REFUSED: not staged]`. The `FAILED:`
variants are now covered by the new parametrized
`test_install_post_sent_non_ok_never_cancels_or_clears`, which N3 CAUGHT. The
old test name (`..._failed_clears_stage`) is now misleading. That is cosmetic.

### (d) INFO-1 INCONCLUSIVE path: correct

After a confirmed reboot with a valid baseline and a changed build,
`judge_snapshot(after)` FAIL still wins. A non-empty inconclusive list now
gives INCONCLUSIVE with the items in `observed["after_inconclusive"]` and
`install_may_have_run=True`, so no cleanup runs. N7 and N11 are CAUGHT. This
can make a clean install INCONCLUSIVE when a field is transiently unreadable
right after boot (INFO-5). That is the intended trade.

### (e) What the fix broke or left open: LOW-1..LOW-4 below

## LOW-1: an exception after the POST comes back as `error:` and is read as "never started"

The `_tool()` wrapper turns every exception into `error: {exc}` or
`error: unexpected {T}: {exc}`. If anything after `uhc.start_install()` raises
something other than `UpdateHttpError`, the case sees `error:...`, treats it
as pre-POST, and runs `update_fetch_cancel` + `update_stage_clear`. That is
the cleanup on a mid-apply board that MED-2 forbade. A probe with the install
returning `error: unexpected KeyError: 'x'` called
`status, cancel, clear, status`.

This is latent today. `_request()` wraps every urlopen/read/JSON error in
`UpdateHttpError`, and the poll loop has no other raising operation that I
could find. The rule depends on that unstated invariant, though. It also
means the case's own `raised` branch never runs in production, because the
wrapper never lets an exception out.

Fix options:
- (preferred) Have `update_install_staged` catch everything after the POST
  and return `UNKNOWN: ...`.
- Or have the case treat only the two known pre-POST strings as
  "never started": `error: could not read GET` and `REFUSED:`.

## LOW-2: a post-POST failure can end INCONCLUSIVE and lose the install verdict

In `_install_and_verify`, `after is None` with `bad_field` set returns
INCONCLUSIVE ("baseline field ... stayed unreadable") before `raised` or
`raised_non_ok` is checked. A probe with
`FAILED: install recorded a failure` returned that verdict. In the probe, the
board came back on a new build with `heap_min_free` unreadable, for example a
recovery image that serves identity but not counters. A definite install
failure is reported as INCONCLUSIVE, and the reason does not name the install
result. The raw text is still in `observed["install"]`.

INCONCLUSIVE exits 2 in the runner, so the release still does not pass. That
is why this is LOW. Fix: when `raised` or `raised_non_ok` is set, return FAIL
and name the install result before the bad-field INCONCLUSIVE.

## LOW-3: the fix-status SHA in REVIEW_RELSUITEFX3.md is wrong

The fix status says "Fixed in 62377b873". That object exists but is not an
ancestor of origin/dev; it is the pre-rebase SHA. The landed commit is
`bbfa0712d`.

## LOW-4: a definite 4xx install refusal is treated as may-have-run, leaving the stage and not tainting the run

Examples are a 409 for a firing or the mode gate, a 428, or a 400. They
return `FAILED: install refused or lost: HTTP 409 ...`. The board refused, so
nothing is applying. The case still polls for up to 300 s, skips cancel and
clear, and does not set `_tainted`. The verified stage is left on the board
with nothing in the run result saying so, and the next run's D01 pre-check
returns INCONCLUSIVE ("stage not provably empty").

This is safe, but it costs time and leaves state unreported. Fix options:
- Distinguish `HTTP <4xx>` (refused) from `HTTP None` (lost).
- At least set `_tainted` or add an observed note that the stage was left
  staged.

## INFO

- INFO-1: `_install_and_verify`'s docstring still says "The stage is only
  left alone when the install tool reported ok or raised". Now any non-ok
  result after the POST also leaves the stage alone.
- INFO-2: `observed["install_non_ok"]` and the "update_install_staged returned
  ..." text in the no-reboot reason are not test-pinned (N9 and N10 MISSED).
  Both are diagnostic only.
- INFO-3: `test_ota_install_failed_clears_stage` and
  `test_d01_every_update_call_carries_the_pinned_host` use contexts without an
  injected `_now`/`_sleep`. A regression into the poll path makes them sleep
  in real time: N5 took 606 s against about 6 s for the others. A wrong
  classification still fails, but slowly. Inject the fake clock there.
- INFO-4: `_pin_host` keeps the port but drops the scheme and path. That has
  no effect today because `update_http_client._url` always builds `http://`.
- INFO-5: the post-install snapshot is judged at the first poll that has a
  valid baseline, with no settle time. Fields that are transient after boot
  (readiness, Wi-Fi status, boot_guard `persisted_count` before the healthy
  mark) can give FAIL or, since INFO-1, INCONCLUSIVE on a clean install. This
  risk is from before the fix and has not been checked on hardware.

## Negative tests

Run: `tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_release.py"`
with a 12-entry `-Mutations` JSON, at `82531b514`. Baseline PASS (7.1 s);
`real_tree_unchanged: true`, copies removed. Every CAUGHT result matched the
preset's `^FAILED tests/...` assertion line (the matched test is named below).
No result came from a collection or import error.

| # | Mutation | Verdict | Failing test(s) |
|---|---|---|---|
| N1 | pin only when `isinstance(ip, str)` (FX3 MED-1 reverted) | CAUGHT | test_pin_identity_ipv4address_from_fake_is_pinned_as_str_with_port |
| N2 | pin `str(ip)` (port dropped) | CAUGHT | test_pin_identity_real_board_identity_check_ipv4address_keeps_port, ..._from_fake_... |
| N3 | `FAILED` treated as pre-POST | CAUGHT | test_install_post_sent_non_ok_never_cancels_or_clears[FAILED: install recorded a failure], [...refused or lost...] |
| N4 | `UNKNOWN` treated as pre-POST | CAUGHT | test_install_post_sent_non_ok_never_cancels_or_clears[UNKNOWN...], test_install_post_sent_non_ok_without_reboot_still_skips_cleanup |
| N5 | `REFUSED` no longer pre-POST | CAUGHT | test_ota_install_failed_clears_stage, test_d01_every_update_call_carries_the_pinned_host, test_install_pre_post_refusal_cleans_up[REFUSED: not staged] |
| N6 | non-ok install, rebooted, gives PASS | CAUGHT | test_install_post_sent_non_ok_never_cancels_or_clears (all 3) |
| N7 | post-install inconclusive list ignored | CAUGHT | test_install_post_reboot_unreadable_fields_are_inconclusive |
| N8 | `raised_non_ok` never set | CAUGHT | test_install_post_sent_non_ok_never_cancels_or_clears (all 3) |
| N9 | `observed["install_non_ok"]` dropped | MISSED | (INFO-2) |
| N10 | no-reboot reason drops the install text | MISSED | (INFO-2) |
| N11 | post-install INCONCLUSIVE turned PASS | CAUGHT | test_install_post_reboot_unreadable_fields_are_inconclusive |
| N12 | `_stage_empty` drops the `ok` prefix | CAUGHT | test_stage_empty_requires_ok_prefix (both params) |

Tally: 10/12 CAUGHT. Both MISSED results are diagnostic text only.
