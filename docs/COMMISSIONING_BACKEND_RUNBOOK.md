# Commissioning Backend Runbook

Prepared for the backend phase of M18 (`docs/COMMISSIONING_TEST_MATRIX.md`,
"Backend first" in that doc's "Owner's test order"). This runbook exists so a
bench agent can execute the backend phase mechanically, after the board is
reflashed to `origin/main`, without re-deriving route/tool mappings from
scratch.

Verification performed for this document (read-only, no board contact):

- Every route below was confirmed present in
  `firmware/KilnFW/App/drivers/http/route_tier_table.h`'s `kRouteTierTable[]`
  (the single authoritative tier source per that file's own header comment)
  at `origin/main`, and its tier column copied verbatim from that table.
- Every MCP tool name below was confirmed present as a `@_srv._tool()`
  -decorated function across `tools/PcTools/src/kilnctrl/mcp_server_*.py` at
  `origin/main` (170 tool functions found via that decorator; grep:
  `grep -rhA1 '@_srv\._tool()' tools/PcTools/src/kilnctrl/mcp_server_*.py |
  grep '^def '`).
- No route or MCP tool named in this document was found missing. Where the
  matrix already said "none direct", this document says so too and gives the
  raw-HTTP fallback instead of a tool name.

This document does not itself execute anything and was produced without
touching any board.

## How to use this

For each row: call the MCP tool (preferred) or the raw route via
`Invoke-RestMethod`/`curl` (OPEN-tier reads only, or ADMIN routes when no
tool exists and a session/AP-password client is available). State-changing
rows list the restore step to run immediately after, before moving to the
next row. Heating/unsafe rows are **not** part of this phase — they stay
listed under "Deferred" for the owner to schedule separately with the real
fixture load considerations in mind.

Credentials: read `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` (User scope)
and `KILNCTL_AP_PASSWORD` per `docs/agent_rules/COMMON.md`. Never print or
log their values; report only `[bool]` presence.

---

## Class A — Read-only (safe to run in any order, nothing to restore)

| # | Route | Tier | MCP tool | Notes |
|---|---|---|---|---|
| A1 | `GET /api/status` | OPEN | `get_board_state` | Superset read |
| A2 | `GET /api/readiness` | OPEN | none direct — `curl` GET | `capability_preflight_check` is the nearest facade equivalent, board-agnostic |
| A3 | `GET /api/history.csv` | OPEN | none direct — `curl` GET | |
| A4 | `GET /api/firing_history` | OPEN | none direct — `curl` GET | |
| A5 | `GET /api/profile_plan` | OPEN | none direct — `curl` GET | Feeds feasibility popup |
| A6 | `GET /api/board_temps` | OPEN | none direct — `curl` GET | `thermo_read` is the live-board equivalent, not identical |
| A7 | `GET /api/profile_exec` | OPEN | `profiles_get_exec_status` | |
| A8 | `GET /api/control` | ADMIN | `control_get_zones` | needs ADMIN session/client |
| A9 | `GET /api/profiles`, `/api/profiles/builtin`, `/api/profiles/favorites` | USER | `profiles_list` | |
| A10 | `GET /api/profile` | USER | `profiles_get` | |
| A11 | `GET /api/zones`, `GET /api/zones_diag` | ADMIN | `control_get_zones` | |
| A12 | `GET /api/zones/current_sweep/status` | ADMIN | `zone_current_sweep_status` | read only even before a sweep runs |
| A13 | `GET /api/zones/ct_channel_map` | ADMIN | none direct — `curl` GET | |
| A14 | `GET /api/autotune`, `GET /api/autotune/matrix` | ADMIN | `autotune_get_status` | |
| A15 | `GET /api/autotune/trace.csv` | ADMIN | none direct — `curl` GET | |
| A16 | `GET /api/adaptive_tune` | ADMIN | `adaptive_tune_get_status` | |
| A17 | `GET /api/tuning_recommendations` | ADMIN | none direct — `curl` GET | numbers are meaningless on the 4 W bench fixture but the route itself is safe to read |
| A18 | `GET /api/ramp_assist` | ADMIN | `ramp_assist_get_enabled` | |
| A19 | `GET /api/safety/rate_guard/auto` | ADMIN | `safety_get_rate_guard` | |
| A20 | `GET /api/status` safety fields | OPEN | `safety_get_status` | |
| A21 | `GET /api/safety/commissioning` | ADMIN | `safety_get_commissioning` | |
| A22 | `GET /api/thermo/faults` | ADMIN | `thermo_read_faults` | |
| A23 | `GET /api/crash_report` | ADMIN | (read half of) `crash_report_ack` / none direct | do this before any ack |
| A24 | `GET /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled` | |
| A25 | `GET /api/diagnostics/danger` | ADMIN | none direct — `curl` GET | status poll only, does not arm anything |
| A26 | `GET /api/dualwrite_window` | ADMIN | none direct — `curl` GET | |
| A27 | `GET /api/debug/lwip_stats` | ADMIN | none direct — `curl` GET | |
| A28 | `GET /api/diagnostics/timing` | ADMIN | none direct — `curl` GET | |
| A29 | `GET /api/saftyfw_stack_margin` | ADMIN | none direct — `get_stack_margin` covers ESP side only | |
| A30 | `GET /api/coredump/info`, `GET /api/coredump/chunk` | ADMIN | `read_esp_coredump` | needs `elf_archive/` + `project_description.json`, see `project_read_esp_coredump_needs_project_description` |
| A31 | `GET /api/cfgfs` | ADMIN | `get_cfgfs_status` | status only, not file read/write |
| A32 | `GET /api/kiln_configs` | USER | `list_config_presets` (local presets only; board-stored slots have no wrapper) | |
| A33 | `GET /api/kiln_configs/apply_status` | USER | none direct — `curl` GET | |
| A34 | `GET /api/backup/export` | ADMIN | none direct — `curl` GET (downloads a file; do not import it back without owner review) | read only — do not chain into `/api/backup/import` in this phase |
| A35 | `GET /api/ota/interlock`, `GET /api/ota/esp/status`, `GET /api/ota/pico/status` | ADMIN / OPEN(esp status) / ADMIN(pico status) | `ota_status` | |
| A36 | `GET /api/boot_guard` | ADMIN | none direct — `curl` GET | |
| A37 | `GET /api/partitions` | ADMIN | `debug_check_partition_table` | |
| A38 | `GET /api/ota/pico/rollback/status` | ADMIN | none direct — `curl` GET | |
| A39 | `GET /api/auth/config` | ADMIN | none direct — `curl` GET | |
| A40 | `GET /api/auth/session` | OPEN | none direct — `curl` GET | do not treat as an activity touch (deliberately excluded, see route table comment) |
| A41 | `GET /api/settings/display_power` | ADMIN | none direct — `curl` GET | |
| A42 | `GET /api/setup/progress` | ADMIN | none direct — `curl` GET | |
| A43 | `GET /wifi`, `GET /status`, `GET /networks`, `GET /scan` | OPEN | `wifi_get_status`, `wifi_get_networks`, `wifi_scan` | AP-mode captive index reads |
| A44 | `GET /api/relay_cycles` (implicit, feeds diagnostics page; no separate GET route listed — read via `/api/status`) | — | none direct | see A1 |
| A45 | `GET /theme.css`, `/nav.js`, `/app.js`, `/commissioning_shared.js` | OPEN | none — served assets, `check_lint_pages.ps1`/responsive sweep cover these, not this runbook | |

## Class B — State-changing but reversible (each row lists its restore step)

Run these one at a time; confirm the restore step succeeded (read back the
affected field) before moving on, per `feedback_negative_test_restore_by_hand`
style discipline — a restore that "should have worked" is not confirmed
until read back.

| # | Route | Tier | MCP tool | Change made | Restore |
|---|---|---|---|---|---|
| B1 | `POST /api/profile_exec/start` | USER | `profiles_start` | Starts a firing on the bench 4 W fixture | `POST /api/profile_exec/stop` (`profiles_stop`) immediately after confirming start; then `profiles_ack_last_run` |
| B2 | `POST /api/profile_exec/stop` | SAFETY_REDUCE | `profiles_stop` | Stops firing (paired with B1) | none needed — this IS the restore for B1 |
| B3 | `POST /api/profile_exec/pause` / `POST /api/profile_exec/resume` | USER | `profiles_pause` / `profiles_resume` | Pauses then resumes a firing started under B1 | Resume call itself restores; if abandoning, use B2 |
| B4 | `POST /api/profile_exec/ack_last_run` | USER | `profiles_ack_last_run` | Clears the "last run" dashboard banner | No inverse exists; not state anyone restores — informational ack only |
| B5 | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | Overwrites a zone's PID gains | Read current gains with `control_get_zones` FIRST, write them back after the test with the same call |
| B6 | `POST /api/zones` | ADMIN | none direct — raw HTTP | Overwrites zone config (channel/relay counts, names, etc.) | `GET /api/zones` first, POST the unmodified body back |
| B7 | `POST /api/autotune/start` then `POST /api/autotune/accept` | ADMIN | `autotune_start`, `autotune_accept` | Runs a real closed-loop step test and can overwrite PID gains if accepted | Read gains via `control_get_zones` before starting; if accepted, restore via B5's procedure. Prefer NOT accepting (skip `autotune_accept`) to avoid needing this restore at all |
| B8 | `POST /api/autotune/abort` | SAFETY_REDUCE | `autotune_abort` | Aborts a running autotune (pairs with B7) | none needed — this is itself a restore path |
| B9 | `POST /api/adaptive_tune/revert` | ADMIN | `adaptive_tune_revert` | Reverts an adaptive-tune adjustment | Read `adaptive_tune_get_status` before/after; this route is itself a restore action, so only run it if adaptive-tune has actually adjusted something, and note the before-state |
| B10 | `POST /api/ramp_assist` | ADMIN | `ramp_assist_set_enabled` | Toggles ramp-assist on/off | Read `ramp_assist_get_enabled` first, set back to the original value |
| B11 | `POST /api/safety/rate_guard/auto` | ADMIN | `safety_set_rate_guard` | Overwrites rate-guard config | `safety_get_rate_guard` first, POST the unmodified body back |
| B12 | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | Clears a latched safety trip | No restore — a clear is the desired end state; only run this on a real/expected trip (e.g. the S6a dual-reflash trip per CLAUDE.md), never to mask an unexplained one |
| B13 | `POST /api/safety/commissioning` (stage & commit) | ADMIN | `safety_set_commissioning_fields` | Overwrites guarded-value commissioning fields | `safety_get_commissioning` first, re-commit the unmodified fields after |
| B14 | `POST /api/safety/commissioning/bench_preset` | ADMIN | none direct — raw HTTP | Applies the bench-values preset (this IS the bench setup — see `project_ct_calibration_closed_and_presence_branch`) | Not expected to need restore on this bench; if run against an already-commissioned board, snapshot via `safety_get_commissioning` first |
| B15 | `POST /api/safety/commissioning/ct_auto_zero`, `.../ct_trim`, `.../ct_cal` | ADMIN | `safety_set_commissioning_fields` (ct_cal only) / raw HTTP (auto_zero, trim) | Adjusts CT calibration state | `safety_get_commissioning` before, compare after; per `project_ct_calibration_closed_and_presence_branch` this is already closed on bench — do not re-tune, just confirm the read-modify-write round trips |
| B16 | `POST /api/safety/commissioning/relay_type` | ADMIN | none direct — raw HTTP | Overwrites relay-type field | Read via `safety_get_commissioning` first, POST unmodified value back |
| B17 | `POST /api/crash_report/ack` | ADMIN | `crash_report_ack` | Acknowledges last-crash record | No restore — acknowledging a real, already-reviewed crash is the desired end state (tool itself refuses without `confirm=True` and verifies the ack landed) |
| B18 | `POST /api/crash_report/clear` | ADMIN | none direct — raw HTTP | Clears the crash log entirely | Irreversible (log content is gone); only run on a record already acknowledged and reviewed via B17 first |
| B19 | `GET`/`POST /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled` / `set_watchdog_panic_disabled` | Disables watchdog PANIC (masks real overflow crashes while set) | Read current value first, set back to `false` (enabled) immediately after the test — never leave disabled |
| B20 | `POST /api/relay_cycles/reset`, `POST /api/relay_cycles/restore` | ADMIN | none direct — raw HTTP | Resets/restores per-relay cycle counters | Read counters via `/api/status` before reset; `restore` is the paired inverse of `reset` — run reset then restore back-to-back and confirm counts match pre-reset |
| B21 | `POST /api/dualwrite_window/restore_verified` | ADMIN | none direct — raw HTTP | Restores a dual-write window record | Per `project_cfg_partition_and_user_data_move`, the `cfg` partition is unformatted/inert on this bench — verify this is observably a no-op (read `/api/dualwrite_window` before and after, confirm unchanged) rather than skipping it |
| B22 | `POST /api/zones/current_sweep/abort` | SAFETY_REDUCE | `zone_current_sweep_abort` | Aborts a current-sweep (only meaningful if B-class testing accidentally starts one — the start itself, `/api/zones/current_sweep/start`, is Class C, deferred) | none needed — abort is itself the restore |
| B23 | `POST /api/diagnostics/danger/stop` | SAFETY_REDUCE | none direct — `io_all_relays_off` is the safe fallback | Exits danger mode (only relevant if danger mode was entered — entering it is Class C, deferred) | none needed if danger mode was never entered; confirm via A25 first |
| B24 | `POST /api/ota/esp/rollback` | ADMIN | `ota_rollback_esp` | Rolls back running ESP image to previous OTA slot | Mind the `zones_cfg` schema-bump hazard in CLAUDE.md — read `control_get_zones` after rollback before any further test; reflash to `origin/main` restores the intended image (this phase runs after a fresh reflash, so a rollback here should be followed by another `flash_firmware()` pass, coordinate with the flashing session) |
| B25 | `POST /api/ota/pico/rollback` | ADMIN | `ota_rollback_pico` | Rolls back Pico image | Same restore approach as B24 — reflash after, coordinate with the flashing session |
| B26 | `POST /api/ota/esp/recovery_exit` | ADMIN | `ota_recovery_exit_esp` | Exits recovery mode | Only testable while actually in recovery mode; no restore needed — exiting recovery is the desired end state |
| B27 | `POST /api/sw_reset` | ADMIN | `sw_reset_esp` | Reboots both processors, no config change | Expected S6a trip during the dual reflash window per CLAUDE.md — confirm link-up and clear via B12 afterward, verifying `trip_mask` is only `SAFETY_TRIP_MAIN_FAULT` (bit 5, `0x0020`) before clearing |
| B28 | `POST /api/kiln_configs/apply` | ADMIN | `load_config_preset`/`capability_preflight_check` (facade presets are file-based, not identical to board-stored slots) | Swaps active kiln config | Note which config was active before (via A32/A1), re-apply it after the test |
| B29 | `POST /api/kiln_configs/save` (new / overwrite-existing) | ADMIN | none direct — raw HTTP | Creates or overwrites a stored config slot | If overwriting, snapshot the existing slot via `GET /api/kiln_configs/export` first; if new, delete the created slot after via `POST /api/kiln_configs/delete` |
| B30 | `POST /api/kiln_configs/clone` | ADMIN | none direct — raw HTTP | Creates a new slot | Delete the cloned slot after via `POST /api/kiln_configs/delete` |
| B31 | `POST /api/kiln_configs/rename` | ADMIN | none direct — raw HTTP | Renames a slot | Rename back to original name after |
| B32 | `POST /api/kiln_configs/delete` | ADMIN | none direct — raw HTTP | Deletes a slot | Only delete slots created in B29/B30 for this test; irreversible for pre-existing slots — never delete one that predates this test session |
| B33 | `POST /api/kiln_configs/import` | ADMIN | `convert_config` (offline conversion only, not the upload itself) | Imports a config as a new slot | Delete the imported slot after via B32 |
| B34 | `POST /api/settings/tz` | ADMIN | none direct — raw HTTP | Overwrites timezone setting | Read current tz first (page load / `/api/status` if exposed there, else `/settings` page state), write back after |
| B35 | `GET`/`POST /api/settings/display_power` | ADMIN | none direct — raw HTTP | Overwrites display power/timeout settings | Read via GET first, POST unmodified body back |
| B36 | `POST /api/unit_pref` | ADMIN | none direct — raw HTTP | Toggles C/F unit preference | Read current pref (main page state), set back after |
| B37 | `POST /api/auth/security` (password/PIN/policy saves, clear login credentials) | ADMIN | none direct — raw HTTP | Overwrites credentials/policy | **High caution**: per `project_web_auth_verified_and_blinds_pctools`, enabling auth policy blinds PcTools clients until they log in. Do not run this row unless coordinating with the session holding bench credentials; never write a new credential value into any log/doc. Restore by writing the prior policy/credential state back, confirmed present in env vars beforehand |
| B38 | `POST /api/auth/bootstrap_password` | ADMIN_BOOTSTRAP | none direct — raw HTTP | Sets the first-run admin password | Only relevant on a freshly-erased board (post `flash_firmware(erase_partitions=...)`); this establishes the bench credential, coordinate value with the session owning `KILNCTL_WEB_PASSWORD` |
| B39 | `POST /api/auth/login` | OPEN | none direct — raw HTTP | Establishes a session | No restore — a session naturally expires/extends; **never iterate logins** per `project_login_latency_measured_4s` and the 2026-09-21 login-ladder owner decision — one attempt only |
| B40 | `POST /api/auth/session/extend` | USER | none direct — raw HTTP | Extends session | No restore needed |
| B41 | `POST /api/profile` (save), `POST /api/profile/delete`, `POST /api/profile/import`, `POST /api/profile/builtin/hide`, `POST /api/profile/builtin/restore`, `POST /api/profile/favorite` | ADMIN | `profiles_save`, `profiles_delete`; hide/restore/favorite/import have no direct tool — raw HTTP | Creates/deletes/hides/favorites a profile | Use a throwaway profile name for save/delete round-trip; restore hide/favorite/restore flags to prior state read via `profiles_list` first |
| B42 | `POST /api/backup/import` | ADMIN | none direct — raw HTTP | Restores config from a backup file | **Do not run in this phase** unless the imported file is a backup taken from THIS board moments earlier via A34 — otherwise this silently overwrites live config. If run, use the just-exported A34 file so the round trip is a no-op per `project_backup_round_trip_coverage` |
| B43 | `POST /provision`, `POST /forget`, `POST /ip_config` | ADMIN | `wifi_add_network`, `wifi_forget`, none direct (ip_config) | Adds/removes a Wi-Fi network entry, changes IP mode | Only exercise against a throwaway/test SSID entry; forget it afterward via `wifi_forget`; do not touch the bench's real LAN network entry or its DHCP/static mode |
| B44 | `POST /api/cfgfs/format_confirm` (+ `GET .../format_pending`) | ADMIN | none direct — raw HTTP | Formats the `cfg` LittleFS partition | Per `project_cfg_partition_and_user_data_move`, this partition is unformatted/inert on this bench today and not yet mounted at boot — **defer this row to the owner**; formatting a partition that nothing currently uses is reversible in effect but changes persistent state with no read-back to confirm harmlessness yet |

## Class C — Heating / unsafe (deferred, not run in this phase)

These rows drive relays outside the normal safety-gated path, or their
result is only meaningful with a real heating-element load. Per the matrix's
own classification they are hardware-gated on this 4 W bench fixture, not
defects. Do not run them as part of the backend phase; the owner schedules
them separately if/when appropriate.

| # | Route | Tier | MCP tool | Why deferred |
|---|---|---|---|---|
| C1 | `POST /api/zones/current_sweep/start` | ADMIN | `zone_current_sweep_start` | Energizes each zone's relay in turn to measure real amp draw — meaningless with the bench's 4 W fixture, and it is a relay-driving operation |
| C2 | `GET /api/tuning_recommendations` follow-through (using the sweep result) | ADMIN | none direct | Derived from C1's result; the read itself is Class A, but acting on the recommendation is not part of this phase |
| C3 | `POST /api/diagnostics/danger/enable` | ADMIN | none direct | Explicit purpose is driving relays outside the normal safety-gated path |
| C4 | `POST /api/diagnostics/danger/relay` (+ `.../start`, `dangerFiringBtn` equivalent) | ADMIN | none direct (`io_set_relay`/`io_set_relay_mask` are the lower-level equivalents) | Same reason as C3 |
| C5 | `POST /api/estop/verify` | ADMIN | none direct | Bench E-stop jumper is fitted (NOT asserted, per `project_estop_jumper_is_fitted`) — verifying the real switch needs the jumper removed and a physical actuation |
| C6 | `POST /api/factory_reset` (any scope, especially `all`) | ADMIN | `factory_default_then_load_preset` | Destructive; erases live config/credentials — schedule deliberately, not as a mechanical sweep item, and re-provision Wi-Fi/credentials afterward |
| C7 | `POST /api/ota/esp` (Wi-Fi OTA flash) | OPEN(challenge) then ADMIN | `ota_get_challenge`, `ota_update_esp` | A real flash; CLAUDE.md's sanctioned path for a real flash is `flash_firmware()` (JTAG/OpenOCD) — this is a different mechanism worth exercising deliberately, not folded into a mechanical backend sweep, since it changes the running image |
| C8 | `POST /api/ota/pico` (Wi-Fi OTA to Pico) | ADMIN | `ota_update_pico` | Same reasoning as C7; also watch for the erase-watchdog-reset class fixed per `project_pico_ota_erase_watchdog_resets_safety_processor` |
| C9 | Setup wizard CT verification sweep start (`step8Start`) | ADMIN | `zone_current_sweep_start` | Same underlying route as C1 |
| C10 | `POST /api/autotune/start` → `POST /api/autotune/accept` (full accept path) | ADMIN | `autotune_start`, `autotune_accept` | Listed under Class B (B7) as testable-but-reversible if gains are read back and restored; escalate to deferred instead if the bench session cannot commit to the read-modify-write discipline in B5/B7 |

Note: B7/C10 overlap deliberately — the *start+abort* half of autotune is
Class B (reversible, no gain change), while *start+accept* carries a real
config mutation. A bench agent uncomfortable performing the gain read-back
in B5 should treat the accept step as deferred (C10) and only exercise
start/abort (B7/B8).

---

## Gaps found while preparing this runbook

No route named in `docs/COMMISSIONING_TEST_MATRIX.md`'s backend-phase rows
was missing from `route_tier_table.h`, and no MCP tool named there was
missing from `tools/PcTools/src/kilnctrl/mcp_server_*.py`. This matches the
matrix's own "Gaps and caveats" section, which already flags the routes with
no direct MCP tool (profile import/hide/restore/favorite, kiln_configs
save/clone/rename/delete/import, backup import/export, security saves,
danger-mode relay control, cfgfs file read/write) — those are carried
forward into this runbook's "raw HTTP" rows above rather than re-discovered.

Two additions beyond a literal copy of the matrix, made while cross-checking
against `route_tier_table.h` directly:

- `GET /api/ota/esp/status` is OPEN-tier (confirmed in the table), not ADMIN
  — the matrix's own page-load row already notes this split correctly.
- `POST /api/dualwrite_window/restore_verified` (B21) is present in the
  table with a comment noting it is not in any older plan document's route
  enumeration; it is included here since it is exactly the kind of
  state-changing route this phase should exercise once, with a read-back.
