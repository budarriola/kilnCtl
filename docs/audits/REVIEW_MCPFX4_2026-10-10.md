# Review: mcpfx4 (mcpfx2 H1, M1-M3, L1-L5; mcpfx1 LOW-5/7) -- 2026-10-10

Reviewer: Opus, read-only plus local pytest/negtest runs. No board access.

Range: `68e3b4966..81bbbfd02` on origin/dev:

- `bc09d681c` MCP review fixes (mcpfx2 H1, M1-M3, L1-L5; mcpfx1 LOW-5/7)
- `8fc0549db` aux rule profile: call-site test for derived-field tolerance (negtest L5)
- `81bbbfd02` docs: mark mcpfx1 LOW-5/LOW-7 fixed, LOW-6 open

The task gave the range as `bc09d681c..81bbbfd02`, but that excludes the main
fix commit itself, so this review covers its parent onward.

## Fix status (confx, 2026-10-10)

All findings fixed: HIGH-1 (conftest stubs `_wifi.get_status` with `WifiUartQueryError`; idle status is a real
`ProfileExecStatus`; `test_env_opt_in` hermetic; sweep passes an ota arg to reach the `bench_test_run` gate and
exempts the `bench_test_start` job twin), MED-1 (`_IMPORT_ONLY_KEYS` in `mcp_server_info.py`), LOW-1 (id-less
profiles matched by content), LOW-2, LOW-3, LOW-4 (new tests kill the three mutations). SHA in the landing report.

## Verdict

The production-code fixes are correct: H1, M1, M2 (with the caveats below),
L1-L5 and the SaftyFW fuzz-test fixes. **The M3 conftest fixture breaks 107
PcTools tests (HIGH-1).** Before it, those tests passed only because they sent
real UART queries to the bench board. They are not caught as a regression by
anything short of a full suite run.

## HIGH

### HIGH-1: the new autouse `_idle_shared_link_stub` makes 107 tests fail

`tools/PcTools/tests/conftest.py` `_idle_shared_link_stub` patches
`mcp_server._link.send` to raise `OSError`. Many tools resolve the board
address through `mcp_server_ota._ota_resolve_host_with_source()`. That calls
`_srv._wifi.get_status()`, a UART GET_STATUS over `_link.send`, and it catches
only `WifiUartQueryError`:

```python
    try:
        status = _srv._wifi.get_status()
        ...
    except WifiUartQueryError:
        pass
```

`WifiUartClient._query()` raises `WifiUartQueryError` only for a non-ok
`SendResult` or a reply timeout. An exception from `link.send` itself
propagates, so the tool returns `error: tests must not touch the real link`.

Full-suite result at origin/dev `17e6ce311`, with no extra plugin: **107
failed**. A re-run of just the failing ids reproduces all 107.

| File | Failures |
|---|---|
| test_mcp_confirm_gate_sweep.py | 79 |
| test_safety_rate_guard.py | 15 |
| test_safety_unset_commissioning_params.py | 7 |
| test_safety_set_config.py | 4 |
| test_mcp_server_kiln_configs_tools.py | 1 |
| test_bench_test_cases_aux.py | 1 (pre-existing, see INFO-4) |

Before `bc09d681c`, these tests sent a real GET_STATUS through the running
kilnctrl hub to the bench ESP. That is the same hidden bench dependency M3
set out to remove; the fixture surfaced it as failures instead of fixing it.

There is a second defect once the first is out of the way. A scratch plugin
stubbed `_srv._wifi.get_status` to raise `WifiUartQueryError`, which fixed 74
of the 107. The other 32 `test_mcp_confirm_gate_sweep` cases then fail with:

```
error: unexpected attributeerror: 'types.simplenamespace' object has no attribute 'profile_id'
```

The fixture's idle `SimpleNamespace(state=0, state_name="IDLE")` lacks fields
that the mid-run gates read, for example `mcp_server_aux._running_reason()`.
That affects the four `control_*aux*`/`profile_save_bench_aux_rule` tools and
four `control_set_zone_*`/`control_set_relay_type` tools, four parametrisations
each.

Suggested fix, all in `conftest.py`:

1. Also patch `srv._wifi.get_status` (and `_wifi` generally) to raise
   `WifiUartQueryError`. Alternatively, make `_boom` return a non-ok
   `SendResult` instead of raising, so every UART client maps it to its own
   query-error type.
2. Build the idle exec status from the real status dataclass, or give the
   namespace every field the gates read (`profile_id`, ...). Do not use a bare
   `SimpleNamespace`.
3. Re-run the **full** PcTools suite, not only the touched files.

## MED

### MED-1: M2 content compare gives a false UNVERIFIED for any v1-v3 backup with legacy zone keys

`_first_content_diff()` requires every key in the backup to exist in the
re-export. Firmware `backup_import.c` still accepts the legacy zone keys
`coupling_coeff` and `coupling_neighbor_zone` from format-version 3 or older
backups. `backup_export.c` never emits them; a key-set diff found these are
the only two keys that are imported but not exported. Every successful import
of such a backup therefore reports `UNVERIFIED - POST returned HTTP 200 ...`
deterministically. This fails safe (it never reports a false OK), but it makes
the verifier useless for old backups.

Fix: skip, or map, keys that the firmware consumes but never re-exports. Keep
an explicit allowlist next to the firmware citation.

## LOW

- **LOW-1 (M2):** a profile without `id` is matched to the re-export by
  position (`g[i]`). The firmware places such a profile in the first free
  slot, so a backup that mixes id-less profiles with occupied slots can
  produce a false UNVERIFIED. This fails safe.
- **LOW-2 (M1):** on the saved-credentials path (`ssid=None`), passing
  `password_from_env=True` gives "give either password or password_from_env,
  not both", because the saved password has already filled `password`. The
  message confuses the caller. Refuse with a message that names the
  saved-network path instead.
- **LOW-3 (M3):** the fixture docstring says it answers "pico-armed" reads
  idle, but it patches only `_profiles.get_exec_status`, `_autotune.get_status`
  and `_link.send`.
- **LOW-4 (test gaps, negtest MISSED):** `M2_absent_key_ignored`,
  `M2_list_length_ignored` and `L2_deadline_from_poll_start` survive the
  targeted tests. No test covers a key that is absent from the re-export, a
  list of a different length, or a slow preflight/POST counting against the
  upload deadline. The production code is correct for all three.

## INFO

- **INFO-1 (H1):** the presence-only compare catches a real failure to set or
  clear the AP password. A request for `""` (open AP) must read back `""`, and
  a non-empty request must read back the firmware's `[set]` marker
  (`uart_bridge_ext_wifi.c:57-59`). A change from one non-empty password to
  another cannot be verified. That is by design, and the result text says so.
- **INFO-2 (M1):** no caller relies on the old implicit environment fallback.
  The only hits are `docs/BENCH_TEST_LOG.md` history, the GUI's own path and
  facade keywords.
- **INFO-3 (L2):** worst case is the 240 s clamp, plus the last poll (1 s sleep
  and 6 s GET), plus 3 done re-reads (0.5 s and 6 s each), about 267 s. That is
  under the client's 300 s abort. The lost-poll path now checks the same
  deadline.
- **INFO-4:** `test_bench_test_cases_aux.py::GateTest::test_env_opt_in` fails
  in the pre-fix baseline too. It is unrelated to this range.
- **INFO-5 (L1, L4, L5):** `allow_undetermined` is taken only by
  `capability_preflight.py` and its MCP wrapper, matching the reworded docs. No
  caller matches the old thermo "warning" text. The aux stored-field compare
  (name, zone_mask, segments minus `feasibility`, on_off_rules) is sound.
- **INFO-6 (mcpfx1 LOW-5/7):** `g_cw_ret` reset, the accepted-rollback no-send
  check and the announce-version save/restore are correct.
  `link_task.c:2252` sends a result only when the rollback request is refused,
  which matches the new assertion.

## Full PcTools pytest

Both runs used `-n 8` and a scratch plugin that makes `mcp_server._link.send`
raise, so neither run could reach the bench.

| Tree | Failed | Passed | Skipped |
|---|---|---|---|
| origin/dev `17e6ce311` (HEAD) | 107 | 7263 | 67 |
| pre-fix baseline `68e3b4966` | 150 | 7208 | 67 |

- **New failures at HEAD:** none relative to the baseline run.
- **Fixed relative to baseline:** 43. These are `test_wifi_write_gates`,
  `test_debug_reset_verify`, `test_debug_post_reset_state` and
  `test_debug_run_guard_partb`. In the true baseline they reached the bench;
  the fixture now keeps them off it.
- **Without the scratch plugin, at HEAD:** the same 107 still fail. They are
  real failures, not plugin artifacts (HIGH-1). In the true baseline they
  would have passed by querying the bench.

## Negative tests (`tools\negtest.ps1`)

Python mutations ran against the touched test files. SaftyFW mutations used
the `saftyfw-host` preset.

| Mutation | Result |
|---|---|
| M3_conftest_fixture_disabled | CAUGHT (28 failed) |
| H1_presence_check_removed | CAUGHT |
| H1_value_compare_restored | CAUGHT |
| M1_sta_env_implicit | CAUGHT |
| M1_ap_env_implicit | CAUGHT |
| M1_both_given_allowed | CAUGHT |
| M2_content_diff_disabled | CAUGHT |
| M2_float_tolerance_removed | CAUGHT |
| M2_absent_key_ignored | MISSED (LOW-4) |
| M2_list_length_ignored | MISSED (LOW-4) |
| L2_lost_poll_deadline_removed | CAUGHT |
| L2_deadline_from_poll_start | MISSED (LOW-4) |
| L4_mismatch_warning_again | CAUGHT |
| L5_full_compare_restored | CAUGHT |
| L5_segment_feasibility_kept | CAUGHT |
| L5_segments_not_compared | CAUGHT |
| SFW LOW7_rollback_accept_sends_result | CAUGHT (`FAIL test_link_task_fuzz.c:1329`) |
| SFW LOW5_cw_ret_reset_removed | not attributable: exit 0; the preset matched passing output. Hygiene-only reset, not expected to fail in a single-order run |

M3 is CAUGHT: with the fixture removed and the link raising, the wifi/debug
tests that used to reach the bench now fail, as the review asked.
