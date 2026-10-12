# Review: oldpoolb batch (2026-10-12)

Range: `55f173a18..b69ba4a94` on origin/dev. Three commits:

- `4a4b1ef39`: conftest refuses loopback to 8766/8767, scrubs `KC_REUSE_SID`, adds `[ValidateRange(1,50)]` to `land.ps1 -MaxPushTries` (subject says "(WIP)").
- `9d93e816a`: `check_land.ps1` case for `-MaxPushTries 0`.
- `b69ba4a94`: doc rows in REVIEW_TESTNET4, REVIEW_TOOLFX13 and REVIEW_BUILDFX2.

Result: 0 HIGH, 1 MED, 1 LOW, 4 INFO. All four negtest mutations were CAUGHT by real failing assertions. The baselines passed and the real tree was unchanged.

## MED-1: asyncio connections on Windows bypass the whole network guard

`tools/PcTools/tests/conftest.py:393-417` patches `socket.socket.connect`, `connect_ex`, `sendto` and `socket.create_connection`. On Windows the default asyncio loop is `ProactorEventLoop`. Its `sock_connect` calls `IocpProactor.connect`, which calls `_overlapped.ConnectEx` on the raw fd and never calls `socket.socket.connect`. Any asyncio, anyio or httpx-async client therefore goes around the guard. That covers both the new MCP-port refusal (TESTNET4 I1) and the original non-loopback refusal (confx HIGH-1).

Probe: a throwaway test in this worktree ran under the autouse guard and was deleted before the negtests. `asyncio.open_connection` produced these results:

| target | result | guard attempts recorded |
|---|---|---|
| 127.0.0.1:8766 | connected (live kicad server) | 0 |
| ::1:8766 | TimeoutError (nothing listening on v6) | 0 |
| 192.0.2.1:80 (TEST-NET, non-loopback) | TimeoutError at the network | 0 |

The loop type was `ProactorEventLoop`. The same targets over `socket.create_connection` were all refused (see INFO-1).

The only in-repo client of port 8767 is `tools/PcTools/scripts/bench_test_client.py:26-30`. It uses `mcp.client.streamable_http`, which runs httpx on anyio on asyncio, so it takes exactly this path. No current test reaches the real transport: `test_bench_test_client.py` fakes `ClientSession`. There is no live exposure today. Still, I1's "refused" claim holds only for synchronous sockets, and `test_guard_refuses_mcp_server_ports_on_loopback` (`test_network_guard.py:83`) exercises only `socket.create_connection`.

Fix: in `_no_outbound_network`, also patch `asyncio.windows_events.IocpProactor.connect` and `asyncio.selector_events.BaseSelectorEventLoop.sock_connect`, applying the same `_is_loopback_addr`/`_is_hub_addr` test. Then add a parametrized test that uses `asyncio.open_connection` against 127.0.0.1:8767 and against a non-loopback address.

## LOW-1: TOOLFX13 INFO-5 cites the wrong fix SHA

`docs/audits/REVIEW_TOOLFX13_2026-10-12.md:150` reads "INFO-5 (FIXED, 9d93e816a)". The `[ValidateRange(1,50)]` line (`tools/land.ps1:80`) landed in `4a4b1ef39`, the commit marked "(WIP)". `9d93e816a` only adds the `check_land.ps1:102-105` case. The row should cite both commits. Both SHAs are ancestors of origin/dev, so they survived the rebase.

## INFO

- INFO-1 (Q1, host forms): through the synchronous path, each of these is refused for port 8767: `127.0.0.1`, `localhost`, `::1`, `::ffff:127.0.0.1`, `127.0.0.2`, `0.0.0.0`, `LOCALHOST` and `localhost.`. The last three fail closed as "non-loopback". A string port (`("127.0.0.1", "8767")`) gets past `_is_hub_addr` in `guarded_create`, but the resolved `sock.connect` inside the real `create_connection` is caught (1 attempt recorded). The hub port 8765 was already in the set (`link_hub.py:89`). gRPC (`logic_capture.py:30`, Saleae 127.0.0.1:10430) and subprocess children use non-Python sockets and are outside any in-process guard. No test calls `automation.Manager.connect` today.
- INFO-2 (Q2, env scrub, `conftest.py:55`): these non-`KILNCTL_` names are still read under `tools/PcTools/src` and `scripts`:
  - `KC_SID`: not a risk. `_child_env` (`web_commission_row.py:680-681`) always overwrites it with the caller's cookie, so an inherited value never reaches the Chrome child through that path.
  - `SALEAE_AUTOMATION_HOST`/`_PORT`, `OPENOCD_EXE`, `ESP_NM_EXE`, `ARM_NM_EXE`, `IDF_*`, `USERPROFILE`, `PYTEST_CURRENT_TEST`, `KILNCTRL_BENCH_HOST`: tool paths or bench knobs, not credentials.

  No unscrubbed credential variable remains. The `.env` loader in the mykicadMcp submodule stays open there (TESTNET4 I3).
- INFO-3 (Q3, ValidateRange callers): `git grep MaxPushTries` finds no caller other than `check_land.ps1`. `land.ps1`, `dev_promote.ps1` and the docs never pass it, and the default of 3 is inside the range. The upper bound of 50 is arbitrary and harmless. Commits `4a4b1ef39` and `9d93e816a` lack the Co-Authored-By trailer.
- INFO-4 (Q4, BUILDFX2 LOW-3): the acceptance is sound for the build gate. One residual case remains at flash time: if `partitions.csv` is edited after the last build, `flash_firmware` resolves offsets from the new CSV (`mcp_server_flash.py:1319`) but writes the stale `partition-table.bin` (`:1504`). The board-table guard (`:280-330`) compares the board against the CSV, not the bin against the CSV. The post-flash running-partition and `fw_build` verification fails loud in that case, so no change is required.

## Q5: doc SHAs

The TESTNET4 I1 row cites `4a4b1ef39`, which is correct. The I3 status line names the test but no SHA (cosmetic). For TOOLFX13 INFO-5, see LOW-1. BUILDFX2 LOW-3 is an acceptance and needs no SHA.

## Negtests

Both runs used `tools/negtest.ps1` from `C:\wt\rvoldpoolb_*` at `b69ba4a94`, with mutation JSON kept in private scratch. No edits were made in the worktree during either run. Both reported `real_tree_unchanged: true` and `copies_removed: true`.

| id | file | mutation | command | verdict | evidence |
|---|---|---|---|---|---|
| L1 | tools/land.ps1:80 | drop `[ValidateRange(1,50)]` | `-Preset check -PresetArg tools\check_land.ps1` | CAUGHT | `FAIL: MaxPushTries 0 rejected before any work`; the other 91 asserts ok (baseline: 92 ok) |
| L2 | tools/land.ps1:80 | `ValidateRange(0,50)` | same | CAUGHT | same single FAIL |
| P1 | conftest.py:337 | `address[1] in (HUB_PORT,)` | `-Preset pytest -PresetArg tests/test_network_guard.py` | CAUGHT | `test_guard_refuses_mcp_server_ports_on_loopback[8766]` and `[8767]`: `DID NOT RAISE NetworkGuardError` |
| P2 | conftest.py:55 | drop `"KC_REUSE_SID"` | same | CAUGHT | `test_scrub_drops_reuse_sid`: `AssertionError` at test_network_guard.py:96 |

Baselines: check_land 525.2 s PASS, pytest 150.1 s PASS. Logs: `%TEMP%\negtest_logs\20261012_072555_pm7x` (land) and `20261012_072605_vbud` (pytest).

This replaces the fixer's void land.ps1 run, which printed "REAL TREE CHANGED".
