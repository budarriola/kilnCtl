# Review: mcpfx1 (H1-H3 MCP gate fixes) and r3sfw (SaftyFW link refusal tests), 2026-10-10

This is a review only; it changes no code. It was done in worktree `C:\wt\rvmcp4_x8921j` at origin/dev `7f5884db4`, which contains both commit pairs.

- **mcpfx1:** `99b0f5f9e` and `f63ad1fd4`. They fix H1-H3 of `docs/audits/REVIEW_MCP_TOOL_GATES_2026-10-10.md`:
  - `debug_probe.write_memory` now always resumes;
  - `control_set_aux_manual` gained a fail-closed relay read-back and an identity check;
  - the Pico GPIO probe write tools refuse unless the Pico is known not ARMED.
- **r3sfw:** `9ae6f3586` and `f394e7036`. They add refusal-path tests to `firmware/SaftyFW/test/test_link_task_fuzz.c`, change the CHECK macro, and add a section to `GUARD_TEST_MATRIX.md`.

Severity scale:
- **MED:** a gate can be defeated, or a safety-relevant report is wrong.
- **LOW:** a narrow gap or a test weakness.
- **INFO:** no action needed.

## Summary

- **mcpfx1, H3 (Pico ARMED gate):** correct and fail-closed, and its tests discriminate. It broke two older tests that do not mock the gate (MED-3). The full PcTools suite has 2 failures at dev tip.
- **mcpfx1, H1 (write_memory resume):** the resume is unconditional in the normal case. The loud "still halted" report, however, is lost whenever OpenOCD prints any `Error:` line (MED-1).
- **mcpfx1, H2 (aux read-back):**
  - The unknown/i2c_failed fail-closed check is correct.
  - The identity check runs after the POST, so it cannot prevent a write to the wrong board. In AP-fallback mode it turns every legitimate call into a FAILED report after the relay has switched (MED-2).
- **r3sfw:** the tests drive the real `link_task.c` handlers through `#include`. The new assertions discriminate. The CHECK change does not break any parser. The remaining items are LOW or INFO.
- **`test_esp_peer_never_consults_pico_armed_state`:** this is not an mcpfx1 regression. It passes both before and after mcpfx1. It depends on the environment because two ESP-path gates added after the test was written reach the live board. See "Test root cause" below.

## Findings: mcpfx1

### MED-1: write_memory reports a halted core only when OpenOCD exits clean

**Status: FIXED (mcpfx3): write_memory evaluates the KCTL_* markers whether or not OpenOCD counted the run ok; tests for ok=False + halted / write error.**

**Where:** `tools/PcTools/src/kilnctrl/debug_probe.py:874-881`, together with `pico_gpio_probe.py:142-145`.

**Scenario:**
1. `openocd_util.run_openocd` returns `ok=False` whenever the output contains `Error:`.
2. OpenOCD logs `Error:` lines even for errors that a Tcl `catch` handles. That includes a failed `mww` (the write error) and a failed `halt`/`resume` inside `_RESUME_TCL`.
3. In exactly the cases the new code was written for (the resume failed, or the core stayed halted), `ok` is therefore usually False. Both `if ok and (rerr or halted)` and `if ok and werr` are skipped.
4. Because `KCTL_AFTER` lines are present, the WARNING branch is also skipped. The function returns `(False, output)` with no "RESUME FAILED / run debug_resume" text.
5. `pico_gpio_probe._write_word` then raises a generic "write ... failed". On the Pico this can leave a halted safety core that is reported only as a failed write, which is the H1 hazard itself.

A failure the other way round is also possible: an unrelated `Error:` line, after a write and resume that both succeeded, reports the write as failed.

**Fix:**
- Evaluate `rerr`, `halted` and `werr` regardless of `ok`. Always put the RESUME FAILED banner first when any `KCTL_AFTER` reads non-running.
- Treat the `KCTL_*` markers as authoritative for write success, rather than the global `Error:` heuristic.
- Add a test with `run_openocd` returning `ok=False` and `KCTL_AFTER rp2040.core0 halted`.

**Negtest:** mutation `wm_failed_run_halted_not_flagged_baseline` (disables the WARNING branch) was MISSED. The `ok=False` path is untested.

### MED-2: the aux identity check runs after the POST, and always fails in AP mode

**Status: FIXED (mcpfx3): identity check runs before the POST, host normalized (scheme/port/name), AP-fallback / not connected / Wi-Fi exception refuse (no board-unique ID is shared by UART and HTTP, so AP mode refuses with a clear message).**

**Where:** `tools/PcTools/src/kilnctrl/mcp_server_aux.py:180-193` and `:231-253`.

**Scenario:**
- **Wrong board:** `_board_identity_mismatch` runs only after `post_aux_manual` has already switched a relay. If the HTTP host is a different board from the UART-linked one, the relay on that other board has already switched. The check can only relabel the result as FAILED. It does not stop the write H2 was about.
- **AP fallback:** `_ota_resolve_host` falls back to `OTA_AP_DEFAULT_HOST` when the UART reports no station IP. That is the same condition under which `_board_identity_mismatch` returns "reports no station IP". Every legitimate call on an AP-only board therefore switches the relay and then reports FAILED / UNVERIFIED. That result is wrong in a way that matters for safety (the operator may retry or distrust a correct state), and the tool is unusable in AP mode.
- **Host format:** the comparison is a raw string compare (`ip != resolved`). An explicit `host="kiln.local"`, `"10.0.0.5:80"`, `"http://10.0.0.5"`, or an IPv4 with leading zeros all report a mismatch after a successful write. The reverse cannot happen (a different board is never accepted), so the check stays fail-closed.

**Fix:**
- Move the identity check before the POST, at least to the pre-write GET.
- Normalize the host: strip the scheme and port, resolve names with `getaddrinfo`, and compare `ipaddress` objects.
- In AP mode, either refuse up front with a clear message or compare a board-unique ID (MAC or boot_id) that is available over both UART and HTTP.
- Add tests for: not connected, `get_wifi_status` raising, the AP default host, and a host name.

**Negtest:**
- MISSED: `aux_not_connected_check_removed` and `aux_wifi_exception_swallowed_as_match`. The fail-closed branches are untested.
- CAUGHT: `aux_ip_compare_removed` and `aux_unknown_check_removed`.

### MED-3: mcpfx1 broke two existing pico_gpio_write tests, and on the bench they would reach the Pico

**Status: FIXED (mcpfx3, 43cf6c767): both tests mock pico_armed_state; the ESP write_memory test also mocks the profile and read-back seams.**

**Where:**
- `tools/PcTools/tests/test_gate_flag_strictness_partb.py:79` (`PicoGpioConfirmTests::test_write_readback_warning`);
- `tools/PcTools/tests/test_pctools_batch_d_2026_10_10.py:143` (`PicoGpioWriteUnverifiedTests::test_readback_exception_not_ok`).

**Scenario:** neither test mocks `pico_armed_state`, so the new `_armed_refusal` runs the real query (nm symbol lookup on `SaftyFW.elf`, then an OpenOCD read).
- **Fresh worktree** (no ELF or nm): the gate refuses with "could not confidently determine Pico ARMED state ... could not resolve symbol 's_state'". Both tests fail, and they fail at dev tip today. The refusal text comes only from the mcpfx1 gate.
- **Main tree** (ELF and probe present): the tests would read live Pico RAM through the debug probe. Whether they pass then depends on the board's real ARMED state.

mcpfx1's own tests mocked the gate, but these two older callers were not updated.

**Fix:** in both test classes, patch `mcp_server_pico_gpio_probe.pico_armed_state` (or `_armed_refusal`) to return `False` / `None`.

### LOW-1: `halt` before the write is not catch-wrapped

**Status: FIXED (mcpfx3): halt is catch-wrapped; resume always follows.**

**Where:** `debug_probe.py:861`.

**Scenario:** if `halt` throws, the rest of the Tcl string (write, resume, `KCTL_AFTER`) never runs. The WARNING branch covers this ("could not confirm resumed"). Nothing was written, but the core may be left halted by a partial halt on a multi-core target.

**Fix:** wrap it, or run `_RESUME_TCL` in that error branch too.

### LOW-2: write_memory now resumes a core the operator halted on purpose

**Status: FIXED (mcpfx3): leave_halted=False option on write_memory / debug_write_memory (default resumes).**

**Where:** `debug_probe.py:841-868`.

**Scenario:** `debug_halt`, then `debug_write_memory` (for example, a poke during an inspection), then the operator expects the core to be still halted. It is now running. `resume()` (line 535) checks `curstate` first, but `write_memory` resumes every target unconditionally, and there is no `leave_halted` option. This is the intended default for H1, but it changes an existing workflow silently.

**Fix:**
- Record the pre-halt `curstate` and resume only the targets that were running before.
- Or add an explicit `leave_halted=False` parameter and document it.

### LOW-3: ARMED gate TOCTOU across several OpenOCD sessions

**Status: DOCUMENTED (mcpfx3): single ARMED read before the write is the same window as debug_write_memory; stated in both docstrings.**

**Where:** `mcp_server_pico_gpio_probe.py:111` and `:139`, with `pico_gpio_probe.set_mode`.

**Scenario:** the gate reads ARMED once. `set_mode` then performs several separate OpenOCD sessions (write, read, write, write), each halting and resuming the core. The Pico can arm in between. This is the same window `debug_write_memory` already has, so it is not a regression.

**Fix:** document it, or re-check before each write session.

### LOW-4: stale docstrings

**Status: FIXED (already, 6d5599f1e/mcpfx3; verified mcpfx4): both docstrings are current.**

**Where:**
- `pico_gpio_probe.py:36-44` still says "No 'refuse while ARMED' gate" and that there is "nothing here to query yet". Both are now false (`_armed_refusal` uses `pico_armed_state`).
- The `write_memory` docstring does not describe the always-resume behaviour or the `KCTL_*` markers.

**Fix:** update both texts.

### INFO: H3 gate is correct

`_armed_refusal` (`mcp_server_pico_gpio_probe.py:81-92`) returns None only for `armed is False`. Every other outcome refuses: ARMED, unreadable, an exception, or None. The gate runs after `confirm` and applies to every GPIO except 6, which keeps its own hard deny. The tests cover ARMED, unreadable and not-armed, and mutation `pico_gate_unknown_allowed` was CAUGHT. There is no regression for legitimate flows.

## Findings: r3sfw

The tests really drive the handlers: `test_link_task_fuzz.c:27` does `#include "link_task.c"`, so `set_ct_cal` (link_task.c:2027), `rollback` (:2231), `reboot` (~:2320), `firing_ceiling` (:2464) and `set_clock` (:2504) run their real code. The CommonFW codecs check only length and cmd, so the value-range refusals in the handlers really are reached.

### LOW-5: `g_cw_ret` leaks `true` into scenario_fuzz

**Where:** `test_link_task_fuzz.c:127`, `:1164`, `:1321`.

**Scenario:** before `9ae6f3586`, the fake `config_store_write` always returned false. It now returns `g_cw_ret`, which `scenario_set_ct_cal` leaves `true`. `scenario_fuzz` therefore exercises the write-success path (reloads, staging drops). This is an unintended order dependence: reordering the scenarios silently changes what the fuzz covers.

**Fix:** reset `g_cw_ret = false` (and the other new fakes) at the end of the scenario, or in a common per-scenario reset.

### LOW-6: no positive control per refused value

**Where:** `test_link_task_fuzz.c:1121-1190` (ct_cal) and `:1192-1244` (ceiling, clock).

**Scenario:** several bad values are sent in sequence, and the test asserts one aggregate counter. One handler bug that refuses everything would pass the refusal checks. The single accepted case afterwards is what catches it, which negtest confirmed (`ctcal_bounds_skipped` was CAUGHT). The channel upper boundary (channel 2 accepted, channel 3 refused) is not asserted at the edge.

**Fix:** add edge-value accepted/refused pairs.

### LOW-7: ROLLBACK accepted path untested; announce version not restored

**Where:** `test_link_task_fuzz.c:1278-1291`.

**Scenario:**
- The rollback fake always refuses (`return false`). The accepted path (no reply, then reset) is never exercised.
- `s_peer_announce.version` is forced to 0 at the end instead of being restored to its prior value. That is a latent order dependence for any later scenario that expects an announced peer.

**Fix:** add an accepting fake case, and save and restore the version.

### INFO: reply-before-reset ordering is covered elsewhere

The REBOOT scenario does not itself assert that the reply is queued before `update_task_reboot_now()`. Negtest mutation `reboot_reset_before_reply` moved the reset before the reply. It was reported as MISSED only because the expect pattern did not match. The run failed at `test_reboot_in_place_wiring.c:295` ("the reply is queued BEFORE the reset"), so the ordering already has a discriminating test.

### INFO: CHECK macro output and parsing

- The new format is `FAIL test_link_task_fuzz.c:%d: ...`. It has no leading indent, and the trailing backslash at line 213 is misaligned (cosmetic).
- `check_00_saftyfw_host_tests.ps1` keys on the verdict line `SAFTYFW HOST TESTS: FAILED`, and nothing greps the old `FAIL line` form. Parsing is not broken.
- negtest `-RequireAssertion`'s pattern `(?m)^\s+FAIL \S+:\d+:` would not match the unindented line. The saftyfw-host preset uses its own expect pattern, and presets refuse `-RequireAssertion` anyway, so this matters only for a hand-built negtest.

### INFO: GUARD_TEST_MATRIX.md

The section "Link handler refusal-path coverage, round 3" matches the scenarios that were added.

## Test root cause: test_esp_peer_never_consults_pico_armed_state

- **Result:** `uv run pytest tools/PcTools/tests/test_debug_write_memory_armed_gate.py` gave 5 passed at both `f394e7036` (the mcpfx1 parent) and dev tip `f63ad1fd4`. mcpfx1 neither introduced nor fixed a failure here.
- **Why it can fail:** the test (`tests/test_debug_write_memory_armed_gate.py:86`, written in `32ce64794`) mocks only `pico_armed_state` and `debug_probe.write_memory`. Commit `0c2846fb3`, which is later than the test and older than mcpfx1, added two ESP-path steps to `debug_write_memory` (`mcp_server_debug.py:700`), and the test does not mock either:
  1. `_esp_profile_running_refusal` (`:334`) queries `_srv._profiles.get_exec_status(timeout=2.0)` over the shared UART link. It fails closed.
  2. `_write_readback_note` (`:684`) calls the real `debug_probe.read_memory`, which runs OpenOCD on the ESP adapter.
- **When it fails:** importing `mcp_server` connects `_link` to the link hub. The test fails, with "error: refusing to write memory on ESP -- profile executor state could not be read", and `assertNotIn("error")` trips, whenever:
  - the hub or board is down or busy;
  - a profile is running;
  - the exec-status reply is slow.
- **When it passes:** it has queried the real board and run a real OpenOCD read on the bench ESP.
- **Proof:** `run_openocd` and `get_exec_status` were patched in a scratch probe test:
  - link down: error result, 0 OpenOCD calls;
  - link idle: "UNVERIFIED - wrote ...", 1 OpenOCD call.
- **Fix (LOW, test):** also patch `_srv._profiles.get_exec_status` (to return idle) and `mcp_server_debug._write_readback_note`, or `debug_probe.read_memory`. Then no unit test reaches the board.
- **Disclosure:** the two plain runs of the original test file during this review most likely made one read-only exec-status query and one read-only OpenOCD `read_memory` against the bench ESP. No writes were made, because `write_memory` was mocked.

## Test runs

- `test_debug_write_memory_armed_gate.py`: 5 passed at `f394e7036` and at `f63ad1fd4`.
- Full PcTools pytest at dev tip `7f5884db4`: 2 failed, 7272 passed, 67 skipped (32 min). Both failures are MED-3:
  - `test_gate_flag_strictness_partb.py::PicoGpioConfirmTests::test_write_readback_warning`;
  - `test_pctools_batch_d_2026_10_10.py::PicoGpioWriteUnverifiedTests::test_readback_exception_not_ok`.
- SaftyFW negtest (preset saftyfw-host, 5 mutations):
  - CAUGHT: `ctcal_bounds_skipped`, `rollback_version_gate_dropped`, `ctcal_reload_removed`.
  - MISSED: `ctcal_drop_id_gain_removed`. This was expected: it is redundant with the superseded-staging drop.
  - MISSED: `reboot_reset_before_reply`, but only by pattern. It failed in `test_reboot_in_place_wiring.c`; see the INFO above.
- PcTools negtest (preset pytest, the aux and pico-write test files, 7 mutations):
  - CAUGHT: `aux_unknown_check_removed`, `aux_ip_compare_removed`, `wm_halted_ignored`, `pico_gate_unknown_allowed`.
  - MISSED: `aux_not_connected_check_removed`, `aux_wifi_exception_swallowed_as_match`, `wm_failed_run_halted_not_flagged_baseline`. See MED-1 and MED-2.
