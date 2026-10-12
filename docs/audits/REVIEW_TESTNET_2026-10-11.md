# Review: CONFX fix commits (test network guard, backup_import id-less matching), 2026-10-11

Reviewed: `4b4765639` and `d67294589` on origin/dev, which fix
`docs/audits/REVIEW_CONFX_2026-10-10.md`: HIGH-1 (an autouse `_no_outbound_network`
guard plus the mocks it forced), MED-1 (id-less profile matching in the
`backup_import` read-back), and LOW-1 to LOW-3. Reviewer: Opus, read-only on the
bench board.

## Verdict

The fixes do what the CONFX review asked. The guard catches the ordinary paths
(`socket.socket.connect`, `connect_ex`, `socket.create_connection`, so urllib,
http.client and requests). It records each attempt and fails the test at teardown
even when the code under test swallows the refusal. The mocks the guard forced do
not weaken any test I checked. MED-1's claim logic is correct. The negative tests
confirm that the new helper-level tests catch it, but not that `backup_import()`
passes its snapshot to the read-back (MED-C).

Open items: no HIGH findings. There are three MED findings. Two are places where
the guard or the read-back can be quietly bypassed (MED-A, MED-B). They are about
defence in depth, and I found no test in the tree that uses either bypass today.
The third (MED-C) is a test gap: nothing checks that `backup_import()` passes its
snapshot to the read-back, so a one-line wiring slip would silently undo MED-1
case 3.

## Findings

### MED-A: the loopback exemption covers the link hub, which reaches the bench board

`_is_loopback_addr` allows every loopback port. Port 8765 is the shared link hub
(`link_hub.py`, `RemoteUartLink`). The running kilnctrl MCP server bridges that port
over UART to the real bench board. `mcp_server.py:123` creates the hub client at
import time with `_link = get_shared_link()`, which is before any function-scoped
fixture runs, so the guard never sees that socket at all. Nothing stops a test from
sending a frame to the board except `_idle_shared_link_stub`, which patches
`srv._link.send`.

Code that calls `get_shared_link()` at runtime gets a fresh hub connection, which
the guard allows because it is loopback. One example is
`mcp_server_coordinated_gpio_test.py:152`, which runs past the confirm gate. That
`send` stub does not cover a fresh connection like this. No test reaches that line
today with `confirm=True`, so nothing touches the board now. But the guard's stated
purpose is "a test cannot reach the bench". For the most direct path to the bench,
that promise rests on one stub and test discipline.

Fix: make the guard refuse loopback connections to `link_hub.HUB_PORT`, except under
`live_bench`. Also make `get_shared_link()` raise inside tests, for example behind an
env flag set in `pytest_configure`.

### MED-B: if the MED-1 pre-import snapshot fails, the failure is swallowed and not reported

`backup_import()` calls `backup_export_http_client.get_export()` to take the
pre-import profile snapshot. It wraps that call in `except Exception:
before_profiles = None` and records nothing. When the snapshot fails, the read-back
silently drops case 3: an id-less backup profile that matches a board profile which
already existed before the import. The tool can then report a verified import that
it did not actually verify that way. No test covers the failure path. `_Base.setUp`
makes `get_export` raise by default, but only the success tests feed it a snapshot,
and none of them asserts what happens when it fails.

Fix: when the snapshot fails and the backup holds id-less profiles, add a result line
saying the id-less match is UNVERIFIED (snapshot unavailable). The CONFX review
suggested the same. Add a test for that line.

### LOW-1: other ways around the guard that it does not see

I confirmed each of these empirically on Python 3.14.6 against TEST-NET 192.0.2.1,
with a throwaway probe outside the repo. None is refused and none is recorded:

- `asyncio.open_connection` on the default Windows `ProactorEventLoop`. It uses
  `ConnectEx` and never calls `socket.connect`. The `SelectorEventLoop` is guarded.
- UDP `sendto`.
- A raw `_socket.socket(...).connect`, and a `socket.socket` subclass whose `connect`
  calls `_socket.socket.connect` directly.
- `HTTP_PROXY` pointed at a loopback listener. The listener received
  `GET http://192.0.2.1/api/x`, so the guard saw only the allowed loopback connect.
- Subprocess children. They are unguarded and inherit `KILNCTL_HOST` and the web
  credentials from the environment. `test_mcp_server_import_order.py` spawns such
  children, but they only import modules.
- `setup_class`, `setUpClass`, `setUpModule` and module/session-scoped fixtures. These
  run before the function-scoped autouse fixture. A socket connected at import time
  also stays usable.

In-repo exposure today: none found. `src` has no asyncio, UDP or proxy use. No proxy
env vars are set, and the Windows registry proxy is off. The class and module setups
I found do no network I/O.

Cheap hardening:

- Patch `socket.socket.sendto` as well.
- Delete `HTTP_PROXY`, `HTTPS_PROXY` and `ALL_PROXY` (both cases) with `monkeypatch`.
- Set `NO_PROXY=*`.
- Move the core patch into `pytest_configure` or a session-scoped fixture, so class
  and module setup is covered too.

### LOW-2: the live_bench exemption does not depend on the bench being selected

A test is exempt when `request.node.get_closest_marker("live_bench")` finds the
marker, which can be set by a module `pytestmark` or a class marker. The three
`test_live_bench_*` modules are safe today because their fixtures skip unless
`KILNCTRL_BENCH_HOST` is set, and it is unset. However, any other test that inherits
the marker, for example from a helper class or a copied module header, runs fully
unguarded with `KILNCTL_HOST` set. KILNCTL_HOST is set on this machine and points at
the real board. Make the exemption require both the marker and a non-empty
`KILNCTRL_BENCH_HOST`.

### MED-C: no test checks that backup_import() actually hands the snapshot to the read-back

The case-3 test, `test_idless_matching_preexisting_board_profile_detected`, calls
`_backup_import_readback_problem(..., before)` directly. No tool-level test runs
`backup_import()` with an id-less backup and a board where the identical profile
already exists, then asserts that the import is reported as not verified. Negative
test `med1_prefetch_not_passed` replaced the call
`_backup_import_readback_problem(resolved, body_text, after_readiness,
before_profiles)` with `..., None)`. That turns case-3 detection off inside the tool,
and the PcTools suite still passed (MISSED). Negative test `med1_prefetch_never` was
caught only by accident. Removing the snapshot call shifted the
`side_effect=[snapshot, export]` list by one, which broke two unrelated success tests.
It was not caught because case 3 went undetected. So the MED-1 case-3 fix is
verified only at the helper level. One wiring slip would silently undo it.

Fix: add a `BackupImportSuccessTest` case whose snapshot and post-import export both
hold the same id-carrying profile, and whose backup holds that profile without an id.
Assert that the result reports the read-back problem.

### LOW-3: no test that the pre-import snapshot is skipped when every profile carries an id

`backup_import()` takes the snapshot only for confirmed, non-dry-run imports whose
backup holds at least one profile dict without `"id"`. Negative test
`med1_prefetch_always` replaced that condition with `True` and was MISSED. Add a test
asserting `get_export` is called once, for the post-import read-back only, when every
profile has an id. Add another asserting it is not called at all on a dry run.

### LOW-4: the id-claim seed is not tested independently

Before the id-less pass, `consumed` is seeded with every board entry matched by an
id-carrying backup entry. Negative test `med1_no_id_seed` replaced the seed with
`set()` and was MISSED. The case-2 test,
`test_idless_does_not_reuse_slot_claimed_by_id_entry`, lists the id-carrying entry
first, so the in-loop claim covers for the missing seed. Add the same case with the
id-less entry first, `[P, dict(P, id=1)]` against `[dict(P, id=1)]`. That order is
what the seed exists to handle.

### INFO

- **Flash-verify wiring mocks** (`test_flash_firmware_verify.py`
  `FlashFirmwareVerifyWiringTest.setUp`): these stub
  `_check_app_flash_offset_matches_chip`, `_refuse_if_adapter_absent` and the two
  boot_guard HTTP calls. The stubs are not a weakening:
  - The adapter refusal has its own tests in `test_flash_board_pinning.py` and
    `test_flash_recovery.py`.
  - The offset guard is tested in `test_mcp_server_flash_partition_guard.py`.
  - `test_flash_firmware_erase_partitions.py` and `KilnFwRootOverrideTest` already
    used the same stub.
  - The boot_guard subclasses override per test.
- **Capability preflight assertion move** (`test_capability_preflight.py`): the
  trailing unreachable-board `assertRaises(PreflightFailed)` in
  `test_clean_board_no_crash_record_passes` used to rely on a real failed connect. It
  now gets a mocked `URLError`. The intent ("unreachable means refuse") is kept. The
  block is misplaced, though. It was written for `BoardUnreachableTest` in
  `0acc2f28e` and ended up in the clean-board test when `2cc90b0b3` inserted tests
  above it. It also partly duplicates
  `test_unreachable_board_not_ok_even_with_zero_required_capabilities`. Suggest moving
  it.
- **Other mocks**:
  - `test_bench_test_cases_heat.py`'s autouse `_no_board_http` raises `OSError` from
    the four HTTP readers.
  - `test_wifi_get_status_ap_fallback.py` raises `OSError` from `urlopen`.

  Both are equivalent to the real refused connect the tests previously depended on.
- **LOW-1 and LOW-2 of CONFX**: correct.
  - The confirm-gate sweep now overrides `bench_test_start`, and `RUNTIME_EXEMPT` is
    only `flash_firmware`.
  - The idle autotune stub is a real `AutotuneStatus(state=0, ...)`.
- **LOW-3 of CONFX**: `_first_content_diff` now compares the per-cell coupling key
  derived from a legacy `coupling_coeff`/`coupling_neighbor_zone` pair, but only when
  the neighbor index is a real int. The firmware rejects a fractional index, so a
  float such as `1.0` is skipped rather than compared. That is harmless.
- **MED-1 claim logic**:
  - Board entries matched by id-carrying backup entries are claimed first.
  - Board entries identical, id included, to a pre-import entry are claimed next
    (case 3).
  - An id-less backup entry then takes the first unclaimed full match. If there is
    none, it falls back to the unclaimed positional entry, and otherwise reports
    "missing".

  This matches the firmware (`backup_import.c` puts each id-less profile in the
  first free slot and never wipes slots). Case 3 can only arise with profiles the
  firmware does not refuse as name collisions, so the residual risk is small.
- **`_is_loopback_addr`**: `::ffff:127.0.0.1` is treated as loopback, and
  `::ffff:192.0.2.1`, `0.0.0.0`, `::`, `LOCALHOST` and `localhost.` are refused. That
  is correct, if strict. Hostnames other than `localhost` are refused without
  resolution. The empty host is allowed, which matches `connect(("", port))` to the
  local host.

## Test summary

- **Full PcTools suite**: run in fresh worktree `C:\wt\rvtnet_20zwoq`, minted at
  origin/dev `950a3e1d39` with submodules. Command: `KILNCTL_SLOW_TESTS=1 uv run
  python -m pytest tests -rfEs --timeout=300 -n 8`. Result: 7454 passed, 58 skipped,
  0 failed, in 18 min 41 s. The skips are:
  - absent gitignored captures;
  - the live_bench modules (`KILNCTRL_BENCH_HOST` unset);
  - Tk with no display;
  - the confirm-gate sweep's documented exemptions;
  - the red-by-design `run_queue` idle-stop test.

  The new guard raised no teardown failure anywhere in the suite.
- **Negative tests**: run with `tools/negtest.ps1 -Preset pytest`, PresetArg
  `tests/test_network_guard.py tests/test_mcp_server_backup.py`, 10 mutations. The
  baseline passed. The real tree was unchanged and the copies were removed.

  | Mutation | What it breaks | Result |
  |---|---|---|
  | guard_no_teardown_fail | drop the teardown `pytest.fail` on recorded attempts | CAUGHT |
  | guard_all_loopback | `_is_loopback_addr` always True | CAUGHT |
  | guard_connect_unpatched | stop patching `socket.socket.connect` | CAUGHT |
  | med1_no_claim_check | id-less match ignores the consumed set | CAUGHT |
  | med1_no_id_seed | consumed not seeded from id-carrying entries | MISSED (LOW-4) |
  | med1_no_preexisting | drop the case-3 pre-import consumption | CAUGHT |
  | med1_prefetch_always | take the snapshot unconditionally | MISSED (LOW-3) |
  | med1_prefetch_never | never take the snapshot | CAUGHT (side_effect order only, see MED-C) |
  | med1_prefetch_not_passed | pass None instead of the snapshot | MISSED (MED-C) |
  | low3_no_cell_compare | skip the derived coupling-cell compare | CAUGHT |

- **Guard bypass probes**: a throwaway pytest module in the reviewer scratchpad, not
  committed, run on Python 3.14.6 against TEST-NET 192.0.2.1. Results are as listed
  under LOW-1 and LOW-2.
