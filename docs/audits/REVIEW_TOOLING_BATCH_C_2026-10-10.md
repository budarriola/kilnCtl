# Review: PcTools tooling batch C (2026-10-10)

Opus review of the PcTools fix batch on `origin/dev` that closes findings from
`REVIEW_TOOLINGB_WEBAUTH_2026-10-09.md`, `PCTOOLS_BATCH_A_REVIEW_2026-10-09.md` and
`PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md`.

Commits reviewed: `91eb1f0ce`, `6f90cd82c`, `a55afd94c`, `f3aa6e50c`, `1c1bacee2`,
`6bf6d34b9`, `19c22806d`, `6279e8a61`, `e685436df`, `3c3e12d9f`, `3f4e12260`,
`c769c8a6a`, `beac10dec`, `2cf7648b6`, `2549d8bf0`, `e5b8d3172`.

Reviewed at dev tip `b752f77e1`. Review only; nothing was fixed.

## Result summary

- No HIGH or MED findings.
- 6 LOW, 5 NIT.
- Full PcTools pytest at `b752f77e1`: **6819 passed, 63 skipped, 138 subtests passed**,
  exit 0 (387.95 s).
- `tools\negtest.ps1 -Preset pytest`, 11 mutations: **all 11 CAUGHT**. The unmutated
  baseline passed, and the real tree was unchanged afterwards (details below).

## Focus-area verdicts

- **MED-1 (latched trip in link-down mode becomes a note): correct, scoped to flashing only.**
  - `recovery_flash.board_state_refusals` (`recovery_flash.py:324-345`) demotes the
    `"a safety trip is latched"` reason only when `link_down_mode` is set and
    `safety_armed is not True`.
  - It has only two callers, `flash_firmware` (`mcp_server_flash.py:1369`) and
    `flash_recovery` (`:1827`). The ota_matrix and heat/clear tools use their own
    refusal paths and are unaffected.
  - A latched trip keeps K4 de-energized, and an ESP flash cannot clear a Pico latch, so
    allowing the flash is safe.
  - An energized relay is still a hazard (tested in `test_flash_recovery.py`).
  - The prefix match cannot swallow an "unreadable" line. `_read_armed_latch_conditions`
    emits "a safety trip is latched (trip_reason=N)" only when
    `diag.ever_received` is set and `trip_reason != 0`, and the unreadable variants use
    other wording.
  - Residuals: LOW-1, NIT-1, NIT-2.
- **Fail-closed on an unreadable executor:**
  - Debug guards (`mcp_server_debug.py:321-350`) now refuse on an exception or a `None`
    status, and on an unreadable or non-idle autotune.
  - Wi-Fi write guards (`mcp_server_wifi.py:71-83`) now refuse on an exception or `None`.
  - `debug_step` and `debug_read_*(leave_halted=True)` are now guarded. `debug_resume` is
    unguarded on purpose.
  - Residual: both guards use a deny-list for the profile state (LOW-4), and the Wi-Fi
    guard has no autotune check (LOW-4).
- **allow_flash exactly True: correct.**
  - `cases_fl.py:351` and `:386` use `is not True`.
  - `mcp_server_bench_test.py:104` passes the value through unchanged.
  - The facade gate-flag check also covers `allow_` names.
- **Expander writer gate: correct.**
  - `_expander_gate` (`mcp_server_io.py`) requires `confirm is True` and also runs
    `_profile_or_autotune_running_reason`.
  - That check is an allow-list (idle/done/faulted, autotune states 0/5/6) and fails
    closed on a query error.
  - It covers all 8 expander writers. `expander_read_reg` is a read and stays ungated.
- **kiln_config_apply id check: correct.**
  - The tool reports UNKNOWN when the reported `target_id` is an int different from the
    requested id.
  - This is safe because the firmware swap worker publishes RUNNING with `target_id`
    synchronously before queueing (`kiln_cfg_swap_worker.c:273`). A stale apply_status
    therefore cannot carry a different id for this request.
- **Read-back failures report FAILED/UNKNOWN:** mostly done:
  - wifi_forget, fixture_set_relay, profiles_save, profile_live_decide
    (`working_id == -1` now matches `profiles_live_http.c:259`), crash_report_clear,
    backup_import, the quarantine client and apply_safety_fields.
  - Remaining success-prefixed unverified results: LOW-2, LOW-3, LOW-5, LOW-6.
- **`2549d8bf0` ever_received tests: coverage kept.**
  - The happy-path tests in `test_safety_clear_trip.py` now use `ever_received=True`.
  - The refusal path is still pinned by
    `test_pctools_write_review_2026_10_09.py::SafetyClearTripLinkAndPollTests::test_refuses_when_no_diag_ever_received`.
  - Negtest `clear_trip_no_ever_received` was CAUGHT.

## LOW

### LOW-1: flash_firmware discards the board-state notes, so the MED-1 note is never shown

`mcp_server_flash.py:1369` unpacks `hazards, unreadable, _state_notes` and never uses the
notes. Only `flash_recovery` prints its notes (`mcp_server_flash.py:1906`).

Failure scenario:
1. An operator flashes the ESP while the safety link is down and an S2 overtemp trip is
   latched.
2. `flash_firmware` succeeds and says nothing about the latched trip, or about
   `LINK_DOWN_NOTE`.
3. MED-1 was raised about exactly this tool. The fix turned a refusal into an
   informational note, and that note is then dropped.
4. The operator is not told that a non-S6a trip is still latched and needs a deliberate
   `safety_clear_trip` after investigation.

### LOW-2: safety_clear_trip result starts "ok" when the clear is unverified or did not take

`mcp_server_safety.py:351-367`: the result is `f"{sent}\n..."`. `sent` is the UART ack
("ok ..."), and it comes first even when the poll result is either of these:
- `read-back failed (...)` (`:361`);
- `STILL LATCHED` (`:367`).

Failure scenario: a script or agent that tests `startswith("ok")` treats a trip that is
still latched (cause still present, or post-trip dwell) as cleared, then proceeds to a
firing preflight. The firing would then be refused later, but with a misleading history.
Other write tools in this batch moved to a `FAILED -` prefix for exactly this case.

### LOW-3: debug_write_memory keeps a success prefix when the read-back is unreadable

A read-back mismatch now returns `FAILED -`. But when `_write_readback_note`
(`mcp_server_debug.py:661-668`) returns "WARNING: read-back raised ..." or "read-back failed
or unparseable; write UNVERIFIED", the result still starts with `wrote 0x...`.

Failure scenario: an OpenOCD read error after the write leaves the caller with a
"wrote ..." result for a write that nothing verified. This is the "success on unverified
write" class this batch set out to remove.

### LOW-4: the debug and Wi-Fi running guards use a profile deny-list; Wi-Fi ignores autotune

- `mcp_server_debug.py:337` and `mcp_server_wifi.py:80` refuse only on
  `status.state in (1, 2)`.
- The PcTools state map (`devices_profiles.py:108`) knows 0-4.
- An unknown state from newer firmware (`unknown(N)`) passes as idle. By contrast,
  `_profile_or_autotune_running_reason` (`mcp_server_control.py:559`) allow-lists
  idle/done/faulted.
- `_wifi_write_refusal` also never reads the autotune state.

Failure scenarios:
- **Wi-Fi during autotune:** `wifi_connect`, `wifi_forget` or `wifi_add_network` during a
  relay-feedback autotune is allowed without `allow_running`, and can drop the link mid-run.
  The guard exists to prevent exactly this, and the debug guard was extended to autotune in
  the same batch.
- **New executor state:** a future state (for example a "soaking/holding" state) would let
  `debug_halt` freeze the ESP mid-firing.

### LOW-5: pico_gpio_write still reports "ok" when the read-back raises

`mcp_server_pico_gpio_probe.py:135`: a mismatch is now `FAILED -`, but an exception during
the read-back gives `ok - pico gpioN = high (read-back unavailable; UNVERIFIED)`.
`fixture_set_relay` in the same commit (`c769c8a6a`) maps the same case to `FAILED ...
UNVERIFIED`. This is inconsistent, and has the same success-prefix risk as LOW-3.

### LOW-6: post_commissioning does not map a generic OSError to "state UNKNOWN"

`safety_cfg_http_client.py:215-250` catches `HTTPError`, `URLError` and `TimeoutError`. A
`ConnectionResetError` or `http.client.RemoteDisconnected` raised while reading the
response (after the POST body was sent) escapes as a raw exception.

The caller then reports an "unexpected error" rather than "state UNKNOWN, may have
APPLIED". This is the same class the batch fixed in `kiln_configs_quarantine_http_client.py`
(OSError caught after URLError).

Failure scenario: a safety commissioning write lands on the Pico, the connection resets
before the response, and the operator reads a plain error and retries the write without
knowing the first one may have applied.

## NIT

- **NIT-1** (`recovery_flash.py:306-310`): link-down mode can be entered through the OTA
  interlock reason alone ("safety link is down" with `needs_ack`) while `link_up` reads
  True. A latched trip is then demoted to a note even with the link up. This is harmless,
  since the Pico latch still holds K4 off, but the mode name no longer matches the
  condition.
- **NIT-2** (`recovery_flash.py:332-340`): the demoted note does not distinguish S6a/S6b,
  which a reflash is expected to cause, from a real fault trip such as an overtemp. It does
  include `trip_reason`. Moot until LOW-1 makes notes visible.
- **NIT-3** (`mcp_server_debug.py:520`): the `debug_halt` docstring still says "If the ESP
  doesn't answer the status query at all, the halt is allowed". That is false since
  `3c3e12d9f`, which refuses unless `allow_running=True`.
- **NIT-4** (`mcp_server_safety.py:351-367`): `safety_clear_trip` sends the clear and
  reports `cleared` even when nothing was latched before (`trip_mask == 0`). It should say
  "nothing was latched".
- **NIT-5**:
  - `PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md`'s closure table (`e5b8d3172`) cites
    `19c22806d` for MED-1. That commit is test-only; the code fix is `6bf6d34b9`.
  - Separately, `test_gate_flag_strictness_partb.py:56-60` patches
    `kilnctrl.mcp_server_flash.flash_firmware`, but the registry dispatches the
    `entry.fn` captured at registration (`mcpkit/registry.py:520`). So
    `ff.assert_not_called()` is vacuous. The `assertIn("refused flash_firmware", ...)`
    assertions still pin the gate.
  - Also, `profiles_save` still compares name, segment count and zone_mask, not segment
    contents (`mcp_server_profiles.py:191`). This was part of the source LOW-3 and is only
    partly closed.

## INFO

- Pico `debug_halt` and `debug_step` have no running guard. This is pre-existing and
  consistent with the owner decision that probe halts are allowed if always resumed.
  Pico writes are gated by `pico_armed_state()`.
- negtest tooling: in an earlier `-Parallel` run whose baseline failed (a wrong test path),
  workers still printed per-mutation CAUGHT lines. The overall verdict was correctly ERROR,
  but the per-mutation lines are misleading in that case.

## Negative tests

`tools\negtest.ps1 -Preset pytest -Parallel`. Test files:
- `test_pctools_write_review_2026_10_09.py`
- `test_safety_clear_trip.py`
- `test_mcp_server_control_zone_limits.py`
- `test_mcp_server_control_zone_type.py`
- `test_zones_http_client.py`
- `test_ui_test_runner.py`
- `test_factory_reset_backup_first.py`
- `test_mcp_server_aux_convert.py`

| Mutation | Target | Result |
|---|---|---|
| preset_progress_skip | MED-3 per-zone progress not appended | CAUGHT |
| preset_partial_no_readback | MED-3 partial path skips PID read-back | CAUGHT |
| clear_trip_no_ever_received | safety_clear_trip ever_received refusal removed | CAUGHT |
| clear_trip_no_poll | read-back poll removed | CAUGHT |
| clear_trip_never_still_latched | STILL LATCHED never reported | CAUGHT |
| ui_generic_catch_reraises | ui_run_script generic catch re-raises | CAUGHT |
| ui_no_partial_note | partial-write note dropped | CAUGHT |
| ui_no_preset_skipped | preset_skipped marker dropped | CAUGHT |
| zone_strip_noop | omit-preserved zone keys not stripped | CAUGHT |
| keepkeys_no_filter | preset keep-keys filter removed | CAUGHT |
| keepkeys_diag_kept | coupling diagonal kept | CAUGHT |


## Status update: batch D (0e76d38b6 on origin/dev)

Fixed: LOW-1 (state notes shown by flash_firmware), LOW-2 (safety_clear_trip leads STILL LATCHED / UNVERIFIED),
LOW-3 (debug_write_memory UNVERIFIED lead), LOW-4 (running guards allow-list idle/done/faulted; Wi-Fi guard checks
autotune), LOW-5 (pico_gpio_write UNVERIFIED), LOW-6 (post_commissioning OSError = state UNKNOWN), NIT-3, NIT-4,
NIT-5 (profiles_save compares segment content). Still open: NIT-1, NIT-2 (recovery_flash note wording).
