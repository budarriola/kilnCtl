# Review: confx (fixes for REVIEW_MCPFX4) -- 2026-10-10

Reviewer: Opus. Read-only review plus local pytest and negtest runs. No board access.

Commit: `2f9aa19cc` on origin/dev ("MCP review 4 fixes: conftest link stub raises WifiUartQueryError and
idles a real ProfileExecStatus (HIGH-1), backup_import compare skips import-only legacy keys (MED-1), ...").

## Verdict

The fixes do what they claim. mcpfx4 HIGH-1 is closed: the 107 failures are gone. Every new test kills
its mutation (negtest table below). There are two problems:

- The link stub was never a network guard. One test in `test_flash_firmware_verify.py` sends a real
  `POST /api/ota/esp/boot_guard_reset` to the bench board's LAN address (HIGH-1). This predates the
  commit, but it is the gap the task asked about.
- The new id-less profile matching never marks an entry as used. It can report a false OK where the
  old positional match failed safe (MED-1).

## Test runs (worktree, `2f9aa19cc`)

- Full PcTools suite, `-n 12`: **1 failed, 7407 passed**, 75 skipped. The one failure is pre-existing
  and real (INFO-1).
- Full suite again with a scratch plugin that refuses and logs every non-loopback `connect` and
  `create_connection` (HIGH-1): same result, 79 outbound attempts logged.
- Negtest, `-Preset pytest` over the five touched or related test files:

| Mutation | Result |
|---|---|
| conftest: drop the `_wifi.get_status` stub | CAUGHT (sweep) |
| conftest: idle exec back to a bare `SimpleNamespace` | CAUGHT (sweep) |
| `_IMPORT_ONLY_KEYS = frozenset()` | CAUGHT (`test_legacy_import_only_zone_keys_not_demanded`) |
| id-less profile matched by position again | CAUGHT (`test_idless_profile_matched_by_content_not_position`) |
| wifi `ssid=None` + `password_from_env` message removed | CAUGHT |
| recovery pico upload deadline measured from poll start | CAUGHT (`test_deadline_counts_from_call_start_not_poll_start`) |
| `bench_test_start` synchronous OTA refusal removed | CAUGHT (`test_bench_test_start_jobs`, 2 tests) |
| `bench_test_start` un-exempted from the sweep, with an override like `bench_test_run`'s | MISSED, i.e. the sweep passes: the exemption is unnecessary (LOW-1) |

Real tree unchanged; copies removed.

## HIGH

### HIGH-1 (pre-existing, outside the range): flash-verify tests send real HTTP, including one POST, to the bench board

`_idle_shared_link_stub` covers the UART link only (`_link.send`, plus the exec, autotune and wifi reads).
None of the shared-link clients reach the board except through `_link.send`. The other link methods,
`connect`, `disconnect` and `status`, are only called from `mcp_server_link.py` tools and
`fixture.py`, and no swept test calls them. So the link side is closed.

HTTP is not covered, and nothing in `conftest.py` blocks sockets. The spy run logged these
non-loopback connects. The same set appears at the parent commit, so `2f9aa19cc` did not cause them:

| Test | Target | Call |
|---|---|---|
| `test_flash_firmware_verify.py::PreFlashProbeWiringTest::test_preflash_address_is_passed_through_to_verification` | 192.168.1.156 | `partition_http_client.get_partitions`, `ota_http_client.get_boot_guard_status`, **`ota_http_client.boot_guard_reset_esp` (POST)** |
| 8 `BootGuardResetWiringTest` cases in the same file | 192.168.1.156 | `partition_http_client.get_partitions` |
| `test_capability_preflight.py::UnacknowledgedCrashReportTest::test_clean_board_no_crash_record_passes` | 192.168.1.50 | `capability_preflight._get_json` |
| `test_bench_test_cases_heat.py` (65), `test_profile_live_mcp_tools.py`, `test_wifi_get_status_ap_fallback.py` | 10.0.0.5 | fake test host; real connects to an address that may exist on another network |

192.168.1.156 is the bench board's address. It is hardcoded in those tests as the pre-flash
probe result, and it also appears in `conftest.py`'s own docstring and the user's `KILNCTL_HOST`.
`flash_firmware()`'s post-flash steps that the tests leave unmocked go to it over `http_auth.urlopen`.
That includes the boot_guard_reset POST, which is admin-tier and clears the board's boot counter.
With a live board and an admin session in the environment, a plain host-side `pytest` run writes to
the bench board. The partition GETs also make those tests timing-dependent on whether the board answers.

Fix: add an autouse fixture in `conftest.py` that refuses non-loopback `socket.create_connection` and
`socket.socket.connect`, as `test_mcp_confirm_gate_sweep.py` already does locally. Live-bench tests
opt out through the `live_bench` marker. Then mock `partition_http_client.get_partitions` and the
boot_guard calls in the flash-verify tests.

## MED

### MED-1: id-less profile content matching can pair two backup entries with one board entry (false OK)

`_backup_import_readback_problem()` takes the first re-exported profile whose content matches,
using `next(e for e in g ...)`. It does not remove that entry from later matches, and it ignores
entries already claimed by an id-carrying backup entry. Run against `2f9aa19cc`, each of these
returns `None` (verified):

1. Two identical id-less profiles in the backup, one landed: both match the same board slot.
2. A backup with `{id:1, ...}` and an identical id-less copy, board has only slot 1: the id-less entry
   reuses slot 1.
3. An id-less profile whose content already existed on the board before the import, and the import
   wrote nothing: it matches the old slot. Profiles use a `>=` count check, so the count does not catch it.

Before the fix, cases 1 and 2 failed safe (UNVERIFIED by position). The verifier's purpose is to never
claim a restore it cannot prove, so this reverses the direction of failure. Exports always carry `id`
(`backup_export.c:372`), so only hand-edited backups reach this path. That keeps it at MED.

Fix: keep a set of used re-export entries, seeded with every id matched by an id-carrying backup
entry. Match id-less entries only against unused entries. For case 3, compare against the pre-import
export, or report "cannot verify id-less profiles" as UNVERIFIED.

## LOW

### LOW-1: the `bench_test_start` sweep exemption is unnecessary, and its comment is wrong

The new comment says the refusal "arrives in the job's FAILED report". Since `de796cfa3` (toolfx8 T4),
`bench_test_start` refuses an unconfirmed OTA/update run **synchronously** with `confirm is not True`,
before any job exists (`mcp_server_bench_test.py:297-304`).
`test_start_unconfirmed_ota_refused_synchronously_and_creates_no_job` covers `False`, `"yes"`, `1`
and `None` for three argument shapes, and the negtest removal of that check was CAUGHT. So the
exemption does not hide a missing gate.

It is still unnecessary. Giving `bench_test_start` the same `ARG_OVERRIDES` entry as `bench_test_run`
makes the sweep pass (negtest MISSED row). The exemption also removes it from
`test_gate_is_an_exact_true_test_not_truthiness`, which it would pass. Fix: replace the exemption
with the override and correct the comment. The cited test name is also not the synchronous-refusal test.

### LOW-2: the idle autotune stub is still a bare `SimpleNamespace` with `state_name="IDLE"`

The same defect class as mcpfx4 HIGH-1 part 2 remains for autotune. The real
`AutotuneStatus.state_name` is lowercase `"idle"` (`STATE_NAMES`), and the stub has no `zone`, `method`
or other fields. Two gates compare `state_name` against lowercase sets:
`mcp_server_coordinated_gpio_test.py:73-74` (`not in ("idle", "done", "aborted")`) and
`mcp_server_ota_matrix.py:182-183` (`_AUTOTUNE_INACTIVE`). Both read the stub as an **active** run. The
result is a refusal, so this fails safe today. But the fixture docstring says the default answer is
"idle", and a test that relies on that would get a spurious refusal. Fix: build `idle_at` from
`AutotuneStatus(state=0, ...)`, as the commit now does for `ProfileExecStatus`.

### LOW-3: `_IMPORT_ONLY_KEYS` is accurate, but the legacy pair's effect is never verified

The list is accurate. A literal key diff of `backup_import.c` against `backup_export.c` and
`backup_json.c` finds no other import-only field names. The remaining literals are `c0`/`c4` (the
`relay_cycles` keys, built with `snprintf("c%u")` and exported), enum and value strings (`mirror`,
`delete`, `absent`, `unknown`), and `x` from a comment. Skipping the two keys masks nothing that the
export contains.

But the firmware maps the pair onto one cell, `coupling_row[neighbor] = coeff`
(`backup_import.c:1811-1815`), unless the entry has explicit `coupling_c<n>` keys. A v<=3 backup has
no `coupling_c<n>` keys, so the derived cell is never compared. A legacy coupling value that
failed to apply would still read OK. Fix: when both legacy keys are present and
`coupling_c{neighbor}` is absent from the backup entry, compare re-export
`coupling_c{neighbor}` against `coupling_coeff`. Separately, the skip matches the key name at any depth,
not only under `zones`. That is harmless today, because no other section uses these names.

## INFO

- **INFO-1 (pre-existing, real defect):** the full suite's one failure is
  `test_bench_test_srv_tool_names::test_every_srv_attribute_exists_on_real_server`.
  `bench_test/cases_aux.py:472` calls `srv._zone_collateral_diff`, which is defined in
  `mcp_server_control.py` but not exported on `mcp_server`. AX-C03's `restore()` wraps the call in
  `except Exception: return False`, so on a real run the restore always reports failure and taints
  the run. It was introduced in `d7cf4dd0b`. Fix: call `mcp_server_control._zone_collateral_diff`, or
  re-export the name.
- **INFO-2:** the commit message says it includes a "hermetic aux env test", and the mcpfx4 fix-status
  note credits it. No aux test file changed in `2f9aa19cc`. `test_env_opt_in` was made hermetic
  earlier, in `d7cf4dd0b`, and it passes at HEAD.
- **INFO-3:** the idle `ProfileExecStatus` stub, the `WifiUartQueryError` stub, the wifi `ssid=None`
  refusal and the new LOW-4 tests are correct. The wifi refusal comes after the confirm/mode gate
  and before any credential load. The new tests are deterministic and kill their mutations.
- **INFO-4:** with `_wifi.get_status` raising, host resolution falls back to `OTA_AP_DEFAULT_HOST`,
  which is the user's `KILNCTL_HOST` value. The spy run found no test reaching that address through
  this fallback. The only 192.168.1.156 traffic is the hardcoded flash-verify case in HIGH-1, and the
  sweep blocks sockets itself.
