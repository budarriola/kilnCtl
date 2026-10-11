# Review: mcpfx2 (origin/dev 9410690ca), MCP tool gates M1-M11

Date: 2026-10-10. Reviewer: Opus. Scope: commit 9410690ca ("MCP tool gates
(review 2026-10-10 MED M1-M11) ...") under `tools/PcTools/src/kilnctrl/`,
its tests, and the CLAUDE.md / `docs/MCP_SERVERS.md` text it added. No board
access. Every run below is host-only; the two `pico_gpio` failures on dev
(MED-3, owned by mcpfx3) were ignored.

Numbering note: the task brief numbers the capability_preflight item "M5";
`docs/audits/REVIEW_MCP_TOOL_GATES_2026-10-10.md` (the source review) numbers
it differently (M5 there is adaptive tune, M7 the waits, M8 the allow-lists).
This document names each fix by area instead of number.

## Verdict

Most fixes fail closed and are tested. One HIGH: `wifi_set_ap_identity` can
never report success after a real AP password change, because the firmware
never returns the password and the test fake does. Three MEDs: the new Wi-Fi
env fallback overrides "None = keep unchanged"; the backup_import read-back is
a count check that passes when nothing was written; and the Wi-Fi and
debug_reset test suites query the real bench board through the link hub (this
is also the xdist flake). The capability_preflight change does not refuse a
healthy board on older firmware. It does refuse on a 401 or 5xx with no
override in several callers, which is the intended fail-closed behavior but
undocumented. The allow-lists match the firmware enums exactly. No secret
echo was found.

## HIGH

### H1. `wifi_set_ap_identity` reports FAILED after every real AP password change

**Status: FIXED (mcpfx4): presence-only compare, marker fake, mismatch test.**

- Where: `tools/PcTools/src/kilnctrl/mcp_server_wifi.py:304`
  (`if ap_password is not None and got_pw != ap_password`).
- Firmware: `firmware/KilnFW/App/drivers/bridge/uart_bridge_ext_wifi.c:57-59`.
  GET_STATUS sends `ap_password` as a `"[set]"`/`""` presence marker, never
  the value (PC side also documents this, `devices_common.py:18`).
- Scenario: operator calls `wifi_set_ap_identity(ap_password="newpass123",
  confirm=True)`. The board saves it and reports ok. The read-back gets
  `"[set]"`, which is not equal to `"newpass123"`, so the tool returns
  `FAILED - ... AP password does not read back as the requested value`.
  The same happens on the env path (`KILNCTL_WIFI_AP_PASSWORD`). The tool
  can never return ok when a password is given, so an operator retries a
  change that already landed, or concludes the board is broken.
- Masked by: the test fake's `get_status` returns the real password
  (`tests/test_wifi_write_gates.py:45-56`, `ap_password=self.ap_pw`).
- Negtest: mutating line 304 to `if False:` is MISSED (W9 below). No test
  covers a password mismatch at all.
- Fix: compare presence only. A non-empty requested password must read back
  as a non-empty marker, and `""` must read back as `""`. Say in the result
  that the value itself cannot be verified over the link ("password presence
  verified; value not readable by design"). Change the fake to return
  `"[set]"`/`""` like the firmware, and add a test where the board reports ok
  but the marker reads `""` after a non-empty password was requested.

## MED

### M1. Env password fallback overrides "leave unset to keep unchanged"

**Status: FIXED (mcpfx4): explicit password_from_env / ap_password_from_env flags; env never implicit.**

- Where: `mcp_server_wifi.py:290-291` (`if ap_password is None: ap_password =
  os.environ.get(WIFI_AP_PASSWORD_ENV) or None`) and `:223-224` (same for
  `wifi_add_network` with `KILNCTL_WIFI_PASSWORD`).
- Scenario A: `KILNCTL_WIFI_AP_PASSWORD` is set in the operator's environment
  (the docs call this the preferred way). The operator calls
  `wifi_set_ap_identity(ap_ssid="kiln2", confirm=True)` to rename only. The
  docstring (`:275-276`) says None keeps the password unchanged, but the tool
  silently sends the env password and changes it. Anyone holding the old AP
  password is locked out of the provisioning AP.
- Scenario B: `KILNCTL_WIFI_PASSWORD` holds the home network password. The
  operator adds a second SSID, or an open network, with no `password`
  argument. The tool sends the home password for the wrong SSID; the join
  fails or the open network gets a PSK. The saved-credentials path
  (`ssid is None`) also falls into the env override when the saved entry has
  no password.
- Fix: make the env read explicit, for example `password_from_env=True`
  (refused when the variable is unset), or key the variable by SSID. Report
  "(password taken from KILNCTL_...)" in the result without the value. Fix the
  docstring and the `docs/MCP_SERVERS.md` "Wi-Fi secrets" section to match.

### M2. backup_import read-back passes when the import wrote nothing

**Status: FIXED (mcpfx4): per profile id and zone index content comparison with float tolerance.**

- Where: `mcp_server_info.py:1051-1080` (`_backup_import_readback_problem`).
- What it checks: readiness readable; re-export `zones` count equal; re-export
  `profiles` count at least the backup's.
- Firmware: `backup_export.c:354-401` exports `zones` as the configured
  channels (3 on the bench and in any backup taken from it) and `profiles` as
  the saved user slots.
- Scenario: restoring a backup taken from the same board, or one with the same
  or fewer profiles. The firmware returns 200 but a pref, zone gain or profile
  body is not committed (the import writes many stores in sequence). The zone
  count is still 3 and the profile count is still at least as large, so the
  tool reports ok. The check can only fail on a channel-count change or lost
  profile slots. A 200 that wrote nothing on a board already holding the same
  shape reads as verified.
- Fix: compare content, not counts. Per profile id, check name, zone_mask,
  segment list and on/off rules. Per zone index, check the PID gains and
  limits. Normalize the floats the firmware rounds on print. Report the
  first differing field. If that is too strict for some fields (derived or
  migrated values), list those fields explicitly in the comparator.

### M3. Wi-Fi and debug_reset tests query the real bench board (and the xdist flake)

**Status: FIXED (mcpfx4): autouse conftest `_idle_shared_link_stub` (idle executor/autotune, raising link send).**

- Where: `tests/test_wifi_write_gates.py:58-62` patches `_srv._wifi` and
  `_srv._profiles` only. `_wifi_write_refusal` (`mcp_server_wifi.py:84-90`)
  then calls the real `_srv._autotune.get_status()`. That client is
  `AutotuneClient(get_shared_link())` (`mcp_server.py:123,274`). When the
  kilnctrl MCP server is running it holds the link hub, so the test process
  joins it as a client and sends a real AUTOTUNE GET_STATUS to the bench ESP.
  `tests/test_debug_post_reset_state.py` is the same: `md.debug_reset("esp",
  "run")` runs `_esp_profile_running_refusal` (`mcp_server_debug.py:334-364`)
  with no `allow_running`, so it sends real profile-exec and autotune queries.
  `tests/conftest.py` has no autouse fixture stubbing the shared link.
- Evidence (host-only, a pytest plugin that makes those client calls raise):
  `test_wifi_write_gates.py` 9 of 14 tests fail;
  `test_debug_post_reset_state.py` 16 of 40 fail. With the bench answering
  "idle" they pass. A "Device: W ... uart_proto ... no reply" line from the
  real board appears in the plain xdist run output.
- xdist: 6 back-to-back `-n 8` runs of `test_debug_post_reset_state.py` passed
  (40/40 each). The flake comes from that dependency. Each xdist worker is a
  hub client. The hub serializes one outstanding send-and-ack at a time, and
  every query has a 2 s timeout (`get_exec_status(timeout=2.0)`). Under
  contention (other workers, other sessions, a busy board), or when the bench
  is firing, autotuning, rebooting, in recovery, or the hub is down, a query
  times out or reads non-idle. The tool then refuses, and every test that
  expects "OK" fails. It is not a shared-file race: the history file goes
  into a per-test temp dir.
- Fix: an autouse conftest fixture that replaces `mcp_server._link` (or
  `link_hub.get_shared_link` before import) with a stub that raises on any
  send, and gives `_profiles`/`_autotune`/`_info`/`_safety` idle fakes by
  default. Tests that need another state patch it explicitly. This also
  makes the autotune allow-list testable (M3 has W8 MISSED below).

## LOW

### L1. Undetermined preflight reads now hard-refuse in callers with no override

**Status: FIXED (mcpfx4): docstring and refusal text (401 case) updated; no-override paths documented in MCP_SERVERS.md.**

- Where: `capability_preflight.py:301-330` (`undetermined_reads`, `ok`).
  Callers that cannot pass `allow_undetermined`: `bench_test/runner.py:339`,
  `bench_test/cases_heat.py:114`, `bench_test/cases_ota.py:176`,
  `mcp_server_ota_matrix.py:278`, `run_queue.py:1241`, and
  `mcp_server_zones_current_sweep.py:153` (refuses on `board.undetermined`).
- Older firmware is not a false undetermined. `_get_json`
  (`capability_preflight.py:110-146`) returns the firmware's exact
  `{"ok":false,"error":"no such endpoint"}` body as data. Only other non-2xx
  replies raise, and the crash/readiness handlers treat a missing route as
  absent, not unreadable.
- Scenario: web auth on and no admin session in the environment.
  `/api/crash_report` (ADMIN tier) returns 401, which counts as undetermined.
  A bench suite or run_queue job then refuses with no flag to override.
  A Wi-Fi-only host with no serial link now refuses on task liveness too:
  `stack-margin read failed`.
- This is the intended fail-closed behavior. But
  `mcp_server_capability_preflight.py:38-46` still says a Wi-Fi-only host
  "should not be refused over a check this preflight cannot perform", which
  is now false.
- Fix: update that docstring and the MCP_SERVERS.md line. Name the 401 case
  in the refusal text ("log in: KILNCTL_WEB_USERNAME/PASSWORD"). Either thread
  `allow_undetermined` through `bench_test_run`/`run_queue`, or document that
  those paths have no override.

### L2. recovery_pico_upload can exceed the 300 s client abort; UNKNOWN text omits the 90 s window

**Status: FIXED (mcpfx4): deadline from call start, checked on lost polls; 90 s text.**

- Where: `mcp_server_recovery.py:541,614,623-626,638-640`.
- Scenario: the POST reply is lost after its 60 s timeout
  (`recovery_post_client.py:145`). Preflight GETs take up to 6 s each
  (`recovery_http_client.py:20`), then the poll runs up to 240 s. Lost polls
  `continue` before the deadline check, so they can add up to 5 x (1 s sleep
  + 6 s timeout). Worst case is about 60 + 12 + 240 + 35 s, over 300 s, and
  the client aborts with no result.
- Separately, the relay aborts the upload if nobody polls for
  `RPP_CLIENT_GONE_MS` = 90 s (`recovery_pico_proto.h:132`). The UNKNOWN text
  says "poll recovery_status" but not that this must happen within 90 s.
  `recovery_status` does read the pico status (`mcp_server_recovery.py:~353`),
  so the result is still reachable.
- Fix: take the deadline from the start of the call (about 270 s total), and
  check it on lost polls too. Make the text say "call recovery_status within
  90 s or the relay aborts the upload". Mention the 240 s clamp in the
  docstring.

### L3. update_stage_release docstring omits its 200 s clamp

**Status: FIXED (mcpfx4): clamp in docstring and UNKNOWN text.**

- Where: `mcp_server_update.py:314,323,385`.
- The clamp is correct: the worst case is about 16 + 30 + 200 + 16 s, under
  300 s. But the docstring says "polls up to wait_s", and a caller passing
  `wait_s=600` gets 200 without being told.
- Fix: one line in the docstring, and the clamped value in the UNKNOWN text.

### L4. Thermo register write mismatch is still "warning -"

**Status: FIXED (mcpfx4): mismatch returns FAILED.**

- Where: `mcp_server_thermo.py:294`. An exception or empty read-back is now
  FAILED, but a value that reads back different from what was written still
  returns `warning - ...`, which a caller matching on `FAILED`/`ok` treats as
  not-a-failure.
- Fix: return `FAILED -` on mismatch, the same as the other read-back tools.

### L5. profile_save_bench_aux_rule compares derived fields of other profiles

**Status: FIXED (mcpfx4): only stored fields compared.**

- Where: `mcp_server_aux.py:~586-670` (`if after_full != before_full`, :667).
- `/api/profile?id=` detail includes `feasibility`, `exceeds_ceiling`,
  `ceiling_note` and per-segment `feasibility`
  (`profiles_catalog_http.c:504-525`). These depend on the zone limits, so a
  concurrent limit change gives a false FAILED. It fails closed, but the
  error is misleading, and the check costs 2N extra GETs per call.
- Fix: strip those derived keys before comparing, or compare
  name/zone_mask/segments/on_off_rules only.

## INFO

- I1. The allow-lists match firmware exactly. `profile_exec_state_t`: 0 idle,
  1 running, 2 paused, 3 done, 4 faulted (`profile_executor_state.h:33`).
  `autotune_engine_state_t`: 0 idle, 1 settling, 2 stepping, 3
  relay_approach, 4 relay_cycling, 5 done, 6 aborted (`autotune_engine.h:62`).
  The Wi-Fi gate allows {0,3,4}/{0,5,6} (`mcp_server_wifi.py:80,88`).
  `mcp_server_safety.py:1205-1218` and
  `mcp_server_coordinated_gpio_test.py:63-75` do the same. Any future state
  value refuses.
- I2. Secrets: no echo found. Passwords are never formatted into results,
  the call log records argument names only, and `_pack_str8`'s ValueError
  names no value. The tests assert the password is absent from the output.
- I3. adaptive_tune: `_opt_bool` (`adaptive_tune_http_client.py:139-143`)
  matches the firmware, which emits real JSON booleans
  (`adaptive_tune_http.c:85-103`). A missing key now returns UNVERIFIED.
- I4. profile_live: an UNVERIFIED read-back gets the `FAILED -` prefix
  (`mcp_server_profile_live.py:41`). This is fail-closed, but an
  `UNVERIFIED -` prefix would match the other tools.
- I5. The `debug_reset` tests are not part of 9410690ca. The test isolation
  gap (M3) predates it. 9410690ca made it wider by adding the autotune read
  to `_wifi_write_refusal`.

## Negative tests

Run with `tools/negtest.ps1 -Command ... -Mutations <json> -ExpectPattern
'(?m)^FAILED \S+'`. This is the pytest preset command plus one pytest plugin
that stubs the shared link and returns idle profile and autotune state, so
the runs never reach the board (see M3). `-Preset pytest` alone would send
real queries to the bench. All baselines passed. Base 9410690ca.

| id | mutation | result |
|----|----------|--------|
| W1 | wifi_add unreadable read-back returns ok | CAUGHT |
| W2 | ap_identity confirm gate bypassed | CAUGHT |
| W3 | ap_identity env fallback removed | CAUGHT |
| W4 | add_network env fallback removed | CAUGHT |
| W5 | ap_identity read-back raise returns ok | CAUGHT |
| W6 | forget unreadable read-back returns ok | CAUGHT |
| W7 | profile allow-list admits running/paused | CAUGHT |
| W8 | autotune allow-list admits every state | MISSED (no test sets a running autotune; M3) |
| W9 | AP password mismatch check removed | MISSED (H1; no mismatch test) |
| A1 | `_opt_bool` defaults to `bool(v)` | CAUGHT |
| A2 | `enabled is None` UNVERIFIED removed | CAUGHT |
| A3 | `revert_available is None` UNVERIFIED removed | CAUGHT |
| B1 | backup_import read-back call replaced by None | CAUGHT |
| B2 | unreadable-readiness check removed | CAUGHT |
| B3 | zones/profiles count check removed | CAUGHT |
| B4 | re-export failure returns None (verified) | CAUGHT |

Sets: wifi 7/9 caught, adaptive 3/3, backup 4/4. The backup run ended
`verdict: ERROR, real_tree_unchanged: false` only because this review doc was
created in the worktree while it ran. All four mutations were CAUGHT and the
copies were removed. W8 and W9 should get tests along with the H1 and M3
fixes. B1-B4 are caught, but they only exercise the count check that M2 calls
too weak, so a content comparator needs its own mutations.

Each pytest run under the stub link takes 1.5 to 4 minutes for about 20
tests. Importing `mcp_server` with a stub link makes the clients time out on
replies. That is more evidence that the suite expects a live hub (M3).
