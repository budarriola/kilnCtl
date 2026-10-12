# Review: PcTools test-infra fixes for REVIEW_TESTNET2 (2026-10-12)

Scope: origin/dev at `21b4ef2b2`, commits `602e1dc06`, `aa51369f8`, `21b4ef2b2`,
which fix `docs/audits/REVIEW_TESTNET2_2026-10-11.md`. The reviewer had no
board access and touched no board.

The changes reviewed:

- `tools/PcTools/tests/conftest.py:37` always sets `KILNCTL_TEST_NO_HUB=1`.
- `tools/PcTools/tests/conftest.py:41-44` drops `KILNCTL_HOST` and the three
  web credential variables when `KILNCTRL_BENCH_HOST` is unset.
- The guard sets `NO_PROXY`/`no_proxy` to `*` (`conftest.py:333-334`), with
  assertions in `tests/test_network_guard.py:123-126`.
- `tools/PcTools/src/kilnctrl/mcp_server.py:126` logs a warning on the
  NullLink fallback.
- `tests/test_mcp_server_backup.py:460`
  `test_snapshot_profiles_not_a_list_is_unverified` covers
  `mcp_server_info.py:1315-1316`.

Verdict: the fixes are correct and their tests are not vacuous. There are no
HIGH or MED findings. Four LOW/INFO gaps remain; they are listed below.

## Mutation evidence

All runs used `tools/negtest.ps1 -Preset pytest -PresetArg "tests/test_network_guard.py tests/test_mcp_server_backup.py"`
at `21b4ef2b2`. The unmutated baseline passed in every run. Log directory:
`%TEMP%\negtest_logs\20261012_005554_qcik`, plus `...\20261012_010300_sk0k`
for r5b.

The fixer did not re-run the five original mutations after the final fix, so
r1-r5 re-run them.

| id | mutation | verdict | caught by |
|----|----------|---------|-----------|
| r1 | `snapshot_failed = False` (`mcp_server_info.py:1316`) | CAUGHT | test_snapshot_profiles_not_a_list_is_unverified |
| r2 | drop both `NO_PROXY`/`no_proxy` setenv (`conftest.py:333-334`) | CAUGHT | test_proxy_env_removed_inside_guarded_test |
| r3 | `NO_HUB` set only without a bench host (the old behaviour) | CAUGHT | test_conftest_keeps_board_env_with_bench_host_but_no_hub |
| r4 | env drop becomes a no-op (`conftest.py:44`) | CAUGHT | test_conftest_drops_board_env_without_bench_host |
| r5 | `_link` is a non-NullLink fake (`mcp_server.py:127`) | invalid first try (fake lacked attributes, collection error) | - |
| r5b | `_link` is a full duck-typed non-NullLink fake | CAUGHT | test_mcp_server_import_link_is_null_link |
| n6 | `KILNCTL_WEB_PASSWORD_NEW` removed from the drop list | CAUGHT | test_conftest_drops_board_env_without_bench_host |
| n7 | web creds dropped even when a bench host is set | **MISSED** | - (finding L2) |
| n8 | drop made unconditional | CAUGHT | keeps_board_env test |
| n9 | `NO_PROXY=localhost` instead of `*` | CAUGHT | proxy test |
| n10 | non-list snapshot treated as `[]` | CAUGHT | not_a_list test |
| n11 | `isinstance(_bp, list)` check removed | CAUGHT | not_a_list test |
| n12 | NullLink `log.warning` removed (`mcp_server.py:126`) | **MISSED** | - (finding I1) |
| n13 | `os.environ.setdefault("KILNCTL_TEST_NO_HUB", "1")` | **MISSED** | - (finding I2) |
| n14 | drop condition inverted | CAUGHT | both env tests |
| n15 | UNVERIFIED message wording changed | CAUGHT | test_snapshot_failure_with_idless_profile_is_unverified (the older MED-B test; the new LOW-1 test has no message assert) |

## Questions asked

**Does always disabling the hub break a live_bench path?** No.

- Live tests talk to the board over HTTP. When they need UART they open their
  own `UartLink()` in `tests/bench_fixture_session.py:374-395`.
- No test reads `mcp_server._link` except `test_network_guard.py:88`.
- Code paths that call `get_shared_link()` under pytest now raise
  `LinkHubDisabled`, which is the intended behaviour:
  - `mcp_server_coordinated_gpio_test.py:152`
  - `gui.py:219`
  - `scripts/coordinated_gpio_test.py`
  - `scripts/current_sense_commissioning.py`
- `run_pctools_tests` (`mcpkit/workbench.py:839`) runs pytest as a
  subprocess, so the always-set `NO_HUB` never reaches a long-running MCP
  server process.

**Can the env drop leak credentials another way?** Yes, in narrow ways. See
L1 and L3.

- Windows `os.environ.pop` does propagate to child processes. A Python child,
  `cmd`, and `GetEnvironmentVariableW` all saw the variable as gone, so the
  drop itself works.

## Findings

### L1 (LOW) The drop list is narrower than the set of board credentials

Location: `tools/PcTools/tests/conftest.py:42-43`.

The list names four variables: `KILNCTL_HOST`, `KILNCTL_WEB_USERNAME`,
`KILNCTL_WEB_PASSWORD` and `KILNCTL_WEB_PASSWORD_NEW`. It misses two groups.

Board secrets and gates set on this bench machine's User scope (names only)
that survive into every test's environment and its children:

- `KILNCTL_LCD_PIN`
- `KILNCTL_STA_SSID` and `KILNCTL_STA_PASSWORD`
- `KILNCTL_HOTSPOT_SSID` and `KILNCTL_HOTSPOT_PASSWORD`
- `KILNCTL_AP_PASSWORD`
- `KILNCTL_AUX_BENCH_CONFIRM`, a bench write gate read at
  `src/kilnctrl/bench_test/cases_aux.py:40`

Other variables that source code reads but the list also misses:

- `KILNCTL_WEB_USER_USERNAME` and `KILNCTL_WEB_USER_PASSWORD`
- `KILNCTL_TOTP_CODE`
- `KILNCTL_WIFI_PASSWORD`
- `KILNCTL_BOARD_HOST`
- `KILNCTL_RECOVERY_AP_PASSPHRASE`
- `MOUSER_API_KEY`

Failure scenario: a unit test of a bench case runs with
`KILNCTL_AUX_BENCH_CONFIRM=1` inherited from the machine. A test that forgot
to clear it then exercises the confirmed path instead of the refusal path.
The test passes or fails depending on which machine runs it.

Mitigation: the outbound-network guard still blocks the board itself, so no
secret reaches the board.

Fix: drop by prefix (`KILNCTL_*` except `KILNCTL_TEST_*`, plus
`MOUSER_API_KEY`) rather than by an enumerated list. Pin the change with a
test that sets one non-listed variable.

### L2 (LOW) The keep-path test pins only `KILNCTL_HOST`, not the web credentials

Location: `tools/PcTools/tests/test_network_guard.py:185-191`.

`test_conftest_keeps_board_env_with_bench_host_but_no_hub` asserts only that
`KILNCTL_HOST` is kept. Mutation n7 drops the three web credential variables
even when a bench host is set. All tests pass under n7 (MISSED).

Failure scenario: a refactor narrows the bench-host keep to the host
variable. Every `live_bench` test that logs in with
`KILNCTL_WEB_USERNAME`/`PASSWORD` then fails on the bench with a 401, and the
unit suite stays green.

Fix: assert all four `_CRED` names are present in that test, as the drop
test already asserts all four are absent.

### L3 (LOW) PowerShell helpers re-read credentials from User scope, bypassing the drop

Location: `tools/PcTools/scripts/web_commission_row.ps1:48-49`. The same
pattern appears in `bench_hotspot.ps1:88-89` and in `make_release.ps1:317`
for `GITHUB_TOKEN`.

The scripts call `[Environment]::GetEnvironmentVariable(...,'User')`, which
reads the registry rather than the inherited environment. So the conftest
drop cannot stop them.

Today this is not reachable from a test: `test_web_commission_row` runs the
script in dry-run mode only.

Failure scenario: a future test invokes the live mode of one of these
scripts. It would get real credentials regardless of the conftest drop. The
network guard does not cover a separate `powershell.exe` child, so that
child could reach the board.

Fix: document this in the conftest comment. Any test that shells out to
these scripts must pass a dry-run switch or stub the User-scope read.

### I1 (INFO) Nothing tests the NullLink fallback warning

Location: `tools/PcTools/src/kilnctrl/mcp_server.py:126`.

Removing the `log.warning` (mutation n12) is MISSED. The warning is
diagnostic only, so this is acceptable. If it matters, add a `caplog`
assertion on a fresh import.

### I2 (INFO) Set versus setdefault of `KILNCTL_TEST_NO_HUB` is not pinned

Location: `tools/PcTools/tests/conftest.py:37`.

Mutation n13 (`setdefault`) is MISSED, because `_run_child_pytest` strips
`KILNCTL_TEST_NO_HUB` from the child environment
(`test_network_guard.py:163`). A child started with `KILNCTL_TEST_NO_HUB=0`
inherited from outside would keep the hub enabled under `setdefault`.

Fix: add a child run that passes `KILNCTL_TEST_NO_HUB=0` and asserts it reads
back `"1"`.

### I3 (INFO) The new LOW-1 test asserts the prefix only

Location: `tools/PcTools/tests/test_mcp_server_backup.py:460-465`.

The test checks `startswith("UNVERIFIED")` and the absence of "ok - restored".
It does not check the "snapshot unavailable" reason. Today the reason is
covered indirectly, because the non-list and exception paths share one
message (n15 was caught by the older MED-B test at line 452). The new test
would not notice if the two paths diverged.

Fix: add `assertIn("snapshot unavailable", out)` to the new test.

## Not findings

- The proxy child test (`test_network_guard.py:114-140`) builds its child
  environment from `os.environ`, so the child inherits the parent guard's
  `NO_PROXY=*`. This does not make it vacuous: the child's own conftest must
  set the value per test, and r2/n9 prove the assertion fires.
- Under a bench host, non-live tests keep the board environment by design
  (LOW-3 in REVIEW_TESTNET2). The network guard still blocks them from
  reaching the board.

## Fix status

| id | status | fix |
|----|--------|-----|
| L1 | fixed | `conftest.py` `_scrub_board_env()` drops every `KILNCTL_*` variable plus `MOUSER_API_KEY` and `GITHUB_TOKEN`, keeping control variables (`KILNCTL_TEST_*`, `KILNCTL_SLOW_TESTS`, `KILNCTL_BUILD_*`, `KILNCTL_LIGHT_GATE_*`, a few runner switches). With `KILNCTRL_BENCH_HOST` set it keeps only `KILNCTL_HOST`, `KILNCTL_WEB_USERNAME`, `KILNCTL_WEB_PASSWORD` (the live_bench tests use `host_resolve` and `http_auth`; `bench_fixture_session` reads only `KILNCTRL_BENCH_HOST`/`_PORT`). Pinned by a child run with 17 non-listed secrets. |
| L2 | fixed | The keep test asserts every kept name and value, and that every other secret (including `KILNCTL_WEB_PASSWORD_NEW`) is dropped. Mutation n7 is CAUGHT. |
| L3 | fixed | `web_commission_row.ps1` (live mode), `bench_hotspot.ps1` (`start`) and `make_release.ps1` (`-Publish`) refuse under `KILNCTL_TEST_NO_HUB=1` before any User-scope read. `test_user_scope_credential_scripts_refuse_under_test_env` runs all three under the test env and proves refusal with no sentinel secret in the output. |
| I1 | fixed | `test_nulllink_fallback_warning_logged` imports `mcp_server` in a child and asserts the warning. n12 is CAUGHT. |
| I2 | fixed | `test_conftest_forces_no_hub_over_inherited_zero`. n13 is CAUGHT. |
| I3 | fixed | `assertIn("snapshot unavailable", out)` added to the not-a-list test. |
