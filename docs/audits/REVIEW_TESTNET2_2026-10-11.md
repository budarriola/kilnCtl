# Review: REVIEW_TESTNET fix commits (link hub disabled under pytest, snapshot-failure UNVERIFIED), 2026-10-11

Reviewed: `0668ff14d` and `dbb236e3e` on origin/dev. These fix
`docs/audits/REVIEW_TESTNET_2026-10-11.md` (MED-A, MED-B, MED-C, LOW-1 to LOW-4).
Reviewer: Opus. Read-only; the bench board was not touched.

## Verdict

The fixes are correct. There are no HIGH or MED findings.

- **NullLink cannot reach production.** `get_shared_link()` raises only when
  `KILNCTL_TEST_NO_HUB` is exactly `"1"`. Only `tests/conftest.py` sets it, and only in
  the pytest process's own `os.environ`, which its children inherit. Nothing in `src/`,
  `scripts/` or `mcp_servers.ps1` sets it. A real MCP server started without the variable
  takes the unchanged path through `get_shared_link()` and gets the real
  `RemoteUartLink`. `NullLink` defines the only plain attributes read from `_link`
  anywhere in `src/` (`is_connected`, `port`, `baudrate`; checked by grep). So
  `__getattr__`, which returns a callable for any other name, cannot turn a missing flag
  into a truthy value. `gui.py` does not catch `LinkHubDisabled`, so the GUI fails loudly
  if the variable is set.
- **The MED-B path fails closed.** `snapshot_failed` is set in every way the snapshot can
  fail: the export raises, the export returns something other than a list, or the
  id-less test itself throws (the broad `except` also sets the flag). When the read-back
  finds no problem, the flag turns the result into `UNVERIFIED ...`. Every `UNVERIFIED`
  return comes before the `ok - restored` return, and no code path can clear the flag. A
  snapshot that works but returns `[]` correctly means "nothing existed before". The one
  over-strict case: a backup with no id-less profiles whose `json.loads` throws inside
  the try also reports UNVERIFIED. That is the safe direction.
- **The tests pin the fixes.** In a full negtest re-run, all 10 of the original
  REVIEW_TESTNET mutations are now CAUGHT, including the three that were MISSED before
  (LOW-4, LOW-3 and MED-C). 9 of 12 new mutations against the fixes are CAUGHT as well.
  The 3 MISSED ones are minor test gaps (LOW-1 below).
- **The LOW-1 residuals are acceptable**, but one cheap hardening is worth doing (LOW-3).

## Findings

### LOW-1: three fix branches have no test (negtest MISSED)

- `medB_nonlist_no_flag`: changing `snapshot_failed = before_profiles is None` to
  `False` is MISSED. Nothing tests the case where the snapshot export succeeds but its
  `profiles` is not a list. The code is correct today. Add a test where the first
  `get_export` returns `("", {"profiles": None})` or `("", {})` and assert `UNVERIFIED`.
- `low1_no_no_proxy`: removing `NO_PROXY=*` / `no_proxy=*` is MISSED. This line does more
  than tidy up. On Windows, `urllib.request.getproxies()` returns the environment proxies
  if there are any, and falls back to the registry otherwise. `no_proxy=*` makes the
  environment dict non-empty and bypasses everything, so the registry proxy is never
  used. Without it, turning on a system proxy in the registry would reopen the LOW-1
  proxy bypass. Extend the subprocess test to assert `NO_PROXY == "*"` inside the guarded
  test, or assert that `urllib.request.getproxies()` there is `{"no": "*"}`.
- `low1_no_proxy_lowercase`: dropping the lowercase loop is MISSED. This is an
  equivalent mutation on Windows, not a real gap: `os.environ` is case-insensitive
  there, so deleting `HTTP_PROXY` also deletes `http_proxy`. The loop still matters on
  POSIX. No action needed. It is listed so the MISSED result is not misread.

### LOW-2: with KILNCTRL_BENCH_HOST set, every test module gets the real hub link again

`conftest.py` disables the hub only when `KILNCTRL_BENCH_HOST` is empty. In a bench run,
`mcp_server.py` therefore connects `_link` to the real hub at import time, for every test
in the session, not only the `live_bench` ones. Non-live tests are back to the original
MED-A situation, where only `_idle_shared_link_stub` patching `srv._link.send` stands
between them and the board. The guard still refuses runtime loopback connects to
`HUB_PORT` in non-live tests, so a fresh `get_shared_link()` connection is still covered.
Bench runs are deliberate, and they are normally run with `-m live_bench` or on the
`test_live_bench_*` files, so the exposure is small. If it matters, run bench sessions
with a node-id filter. Or keep `_link` a `NullLink` and let the live_bench fixtures
build their own link.

### LOW-3 (LOW-1 residual): subprocess children inherit the real board address and credentials

Of the residuals documented as not closed, only subprocess children carry a real
credential. They inherit `KILNCTL_HOST` (which points at the bench board on this
machine) and the `KILNCTL_WEB_*` credentials, and they run without the guard.
`test_mcp_server_import_order.py` and the new `test_proxy_env_removed_inside_guarded_test`
spawn such children. Neither does network I/O today. The children now also inherit
`KILNCTL_TEST_NO_HUB=1`, so they cannot reach the hub, which is an improvement. Cheap
hardening: when `KILNCTRL_BENCH_HOST` is unset, have conftest remove `KILNCTL_HOST`,
`KILNCTL_WEB_USERNAME`, `KILNCTL_WEB_PASSWORD` and `KILNCTL_WEB_PASSWORD_NEW` from
`os.environ` at import, next to the `KILNCTL_TEST_NO_HUB` line. First confirm that no
test depends on them being set.

The other residuals are acceptable as documented. `src/` has no asyncio, and nothing
uses raw `_socket`. The class- and module-scope setups do no network I/O. One note: the
fix-status table says the session-scope move was left out "by design" because the guard
is a function-scoped fixture. That is a choice, not a constraint. A module-level patch
that records into a per-test list would also cover `setUpClass`. Leaving it out is fine
while there is no in-repo exposure.

### INFO

- `NullLink` fallback in `mcp_server.py` is silent. Production cannot reach it (see
  Verdict), so this is cosmetic. A `log.warning` in the `except LinkHubDisabled`
  branch would make a stray environment variable obvious in a server log.
- `test_mcp_server_import_link_is_null_link` asserts only `not is_connected`. A real but
  unconnected `RemoteUartLink` would pass too. `assertIsInstance(mcp_server._link,
  link_hub.NullLink)` would be tighter. Removing the fallback is still caught, as a
  collection ERROR (mutation 14).
- `test_mcp_server_backup.py:443`: the `with` statement in `BackupImportSnapshotTest._run`
  lost its backslash continuations. It is one long valid line with runs of spaces.
  `test_idless_before_idcarrying_does_not_reuse_its_slot` calls
  `ReadbackContentTest._idless(ReadbackContentTest, ...)` unbound, with the class passed
  as `self`. Both work. They are cosmetic only.
- The MED-C tool-level test (`test_preexisting_identical_profile_is_unverified_via_tool`)
  does not assert which read-back problem it hit. The control test
  (`test_new_slot_next_to_preexisting_is_ok_via_tool`) uses the same zones and passes
  `ok - restored`, which shows the zones match. Mutation 9 (`med1_prefetch_not_passed`)
  is CAUGHT by this test alone. So it does pin the snapshot wiring.
- The fix-status table in REVIEW_TESTNET is accurate.

## Test summary

- **Negative tests**: `tools/negtest.ps1 -Preset pytest -PresetArg "tests/test_network_guard.py
  tests/test_mcp_server_backup.py" -Mutations <22> -Parallel 4`, in worktree `C:\wt\rvtn2_cl9etf`
  minted from origin/dev (base `66593acf6`, which contains `dbb236e3e`). The baseline passed. The
  real tree was unchanged and the copies were removed.
  Every mutation that could make the test process reach the hub also remapped `HUB_PORT` to
  18765, so no run could touch the real 8765 hub or the bench board.

  | # | Mutation | What it breaks | Result |
  |---|---|---|---|
  | 1 | guard_no_teardown_fail | drop the teardown `pytest.fail` | CAUGHT |
  | 2 | guard_all_loopback | `_is_loopback_addr` always True | CAUGHT |
  | 3 | guard_connect_unpatched | stop patching `socket.socket.connect` | CAUGHT |
  | 4 | med1_no_claim_check | id-less match ignores the consumed set | CAUGHT |
  | 5 | med1_no_id_seed | consumed not seeded from id-carrying entries | CAUGHT (was MISSED, LOW-4) |
  | 6 | med1_no_preexisting | drop case-3 pre-import consumption | CAUGHT |
  | 7 | med1_prefetch_always | snapshot taken unconditionally | CAUGHT (was MISSED, LOW-3) |
  | 8 | med1_prefetch_never | snapshot never taken | CAUGHT, now also by the snapshot tests |
  | 9 | med1_prefetch_not_passed | pass None instead of the snapshot | CAUGHT (was MISSED, MED-C) |
  | 10 | low3_no_cell_compare | skip the derived coupling-cell compare | CAUGHT |
  | 11 | medA_conftest_no_env | conftest does not set `KILNCTL_TEST_NO_HUB` | CAUGHT |
  | 12 | medA_get_shared_link_no_check | `get_shared_link` ignores the flag | CAUGHT |
  | 13 | medA_no_hub_refusal | `_is_hub_addr` always False | CAUGHT |
  | 14 | medA_no_nulllink_fallback | `mcp_server` does not fall back to `NullLink` | CAUGHT (collection ERROR; expect `FAILED\|ERROR`) |
  | 15 | medB_no_flag_check | drop the `snapshot_failed` UNVERIFIED branch | CAUGHT |
  | 16 | medB_except_no_flag | snapshot exception does not set the flag | CAUGHT |
  | 17 | medB_nonlist_no_flag | non-list snapshot does not set the flag | MISSED (LOW-1) |
  | 18 | low1_no_sendto_patch | stop patching `sendto` | CAUGHT |
  | 19 | low1_no_proxy_delete | stop deleting proxy variables | CAUGHT |
  | 20 | low1_no_proxy_lowercase | drop the lowercase variants | MISSED (equivalent on Windows, LOW-1) |
  | 21 | low1_no_no_proxy | drop `NO_PROXY=*` | MISSED (LOW-1) |
  | 22 | low2_marker_only | exemption on the marker alone | CAUGHT |

  Logs: `%TEMP%\negtest_logs\20261011_233408_7sgs`.

## Fix status

| Finding | Status | Where |
|---|---|---|
| LOW-1 non-list snapshot | Fixed: `test_snapshot_profiles_not_a_list_is_unverified` (None, missing, str) | `tests/test_mcp_server_backup.py` |
| LOW-1 `NO_PROXY=*` | Fixed: guarded child asserts `NO_PROXY`/`no_proxy` == `*` and `getproxies() == {"no": "*"}` | `tests/test_network_guard.py` |
| LOW-1 lowercase proxy loop | No action (equivalent mutation on Windows) | - |
| LOW-2 hub with bench host | Fixed: conftest disables the hub unconditionally (no live_bench test uses `mcp_server._link`); child test pins it | `tests/conftest.py` |
| LOW-3 board env in children | Fixed: conftest drops `KILNCTL_HOST`, `KILNCTL_WEB_USERNAME/PASSWORD/PASSWORD_NEW` unless `KILNCTRL_BENCH_HOST` is set; two child tests | `tests/conftest.py` |
| INFO NullLink warning | Fixed: `log.warning` in the fallback | `src/kilnctrl/mcp_server.py` |
| INFO type assert | Fixed: `assertIsInstance(_link, NullLink)` | `tests/test_network_guard.py` |
| INFO backup.py:443 continuations | Fixed: backslash continuations restored | `tests/test_mcp_server_backup.py` |
| INFO unbound `_idless` call | Left (cosmetic) | - |

Negtest (`-Preset pytest`): medB_nonlist_no_flag, no_no_proxy, low2_no_hub_conditional, low3_no_env_drop, NullLink-not-instance all CAUGHT (low2 was MISSED until the child env stopped inheriting `KILNCTL_TEST_NO_HUB`). Full PcTools suite: 7520 passed, 57 skipped.
