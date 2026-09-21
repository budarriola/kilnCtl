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
  raw-HTTP fallback instead of a tool name. (An opus review of the first
  pass found rows this document had omitted outright, not merely mistiered
  — see "Gaps found" at the end of this document for what was added.)

This document does not itself execute anything and was produced without
touching any board.

## How to use this

For each row: call the MCP tool (preferred) or the raw route via
`Invoke-RestMethod`/`curl` (OPEN-tier reads only, or ADMIN routes when no
tool exists and a session/AP-password client is available). State-changing
rows list the restore step to run immediately after, before moving to the
next row. Class C rows (heating, relay-driving outside the profile
executor's own gated path, safety-config writes, and anything else needing
owner coordination) are **not** part of this phase — they stay listed under
"Deferred / owner-scheduled" for the owner to schedule separately.

Credentials: read `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` (User scope)
and `KILNCTL_AP_PASSWORD` per `docs/agent_rules/COMMON.md`. Never print or
log their values; report only `[bool]` presence.

---

## Class A — Read-only (48 rows; safe to run in any order, nothing to restore)

| # | Route | Tier | MCP tool | Notes |
|---|---|---|---|---|
| A1 | `GET /api/status` | OPEN | `get_board_state` | Superset read |
| A2 | `GET /api/readiness` | OPEN | `get_readiness` | Renders the commissioning checklist directly; `capability_preflight_check` is a board-agnostic adjacent facade, not needed here |
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
| A44 | `GET /theme.css`, `/nav.js`, `/app.js`, `/commissioning_shared.js` | OPEN | none — served assets, `check_lint_pages.ps1`/responsive sweep cover these, not this runbook | |
| A45 | `GET /api/profile/live` | ADMIN | `profile_live_get` | Only meaningful while a firing is running; reads the forked working copy's status/content |
| A46 | `GET /api/profile/export` | USER | none direct — `curl` GET | Per-profile export, distinct from `GET /api/backup/export` (A34) |
| A47 | `GET /api/logs/firing`, `GET /api/logs/autotune` | ADMIN | none direct — `fetch_event_log`/`get_device_log`/`get_device_log_json` are adjacent, not identical | No page renders these directly today |
| A48 | `GET /api/cfgfs/file?name=<name>` | ADMIN | none direct — `curl` GET | Read half of the cfg-partition file route; see C28 for the write half — superseded 2026-09-21: `cfg` is now mounted and populated with 7 files on this bench (per `GET /api/cfgfs`), so a read of an existing file name is expected to come back with real content, not empty/not-found |

## Class B — State-changing but reversible, non-heating, non-safety-config (32 rows; each row lists its restore step)

Run these one at a time; confirm the restore step succeeded (read back the
affected field) before moving on, per `feedback_negative_test_restore_by_hand`
style discipline — a restore that "should have worked" is not confirmed
until read back. Nothing in this class starts a firing, drives a relay
outside the profile executor's own safety-gated path, or writes a
safety-config field (commissioning, CT calibration, relay type, trip
clearing outside the one gated exception below) — those moved to Class C.

| # | Route | Tier | MCP tool | Change made | Restore |
|---|---|---|---|---|---|
| B1 | `POST /api/profile_exec/stop` | SAFETY_REDUCE | `profiles_stop` | Stops a firing | none needed — this IS the restore path; only exercised as part of the Class C, owner-scheduled start (C11) since starting a firing is deferred, not this phase's own action |
| B2 | `POST /api/profile_exec/ack_last_run` | USER | `profiles_ack_last_run` | Clears the "last run" dashboard banner | No inverse exists; not state anyone restores — informational ack only |
| B3 | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | Overwrites a zone's PID gains | Read current gains with `control_get_zones` FIRST, write them back after the test with the same call |
| B4 | `POST /api/zones` | ADMIN | none direct — raw HTTP, but use `tools/PcTools/src/kilnctrl/zones_http_client.py`'s `get_zones()`/`build_post_body()`/`post_zones()` (used internally by `load_config_preset`/`config_presets.apply_preset()` and by `run_queue.py`'s bench harness), not a hand-built form body | Overwrites zone config (channel/relay counts, names, etc.) | `GET /api/zones` first, POST the unmodified body back. **Form shape established 2026-09-21 (M18 B6, `docs/BENCH_TEST_LOG.md`'s dated section — PASS):** `application/x-www-form-urlencoded`, ~2561 B for this board's 3-zone config. WHOLE-PAGE SUBMIT — most per-zone fields (`z<N>_*`) are OMITTED-MEANS-ZERO (`zones_post_handler()`'s `tmp` starts zero-initialized, `zones_http_post.c`/`zones_http_post_parse.c`), so a body must always be built by GET-then-merge, never assembled from only the fields you intend to change. Required top-level: `thermo_count`, `relay_count`, and at least one timing profile (`tp0_name..`, every field within a named profile required once `tp<N>_name` is present). Optional/OMITTED-MEANS-KEEP-CURRENT: `max_simultaneous_relays`, `continue_on_zone_trip`, `z%u_tctype`, `pc_link_abort_silence_ms`, `relay<N>_name` (a separate struct — omitting it is what protects it from `/settings/safety`'s whole-page save, which posts to this same endpoint with no relay-name inputs), `safety_tc_type` (fully read-only/ignored since the 2026-09-15 owner decision — the Pico's own commissioning page is the sole writer). `build_post_body(current, preset)` does the GET-merge for you: pass a small preset dict, e.g. `{"zones":[{"index":0,"name":"..."}]}`, and it echoes every other field from `current` unchanged. A successful POST returns the plain-text body `"ok"` (not JSON) and bumps `generation` by 1 — expect that one field to differ on every read-back, it is not a config change. `zones_http_client.py`'s own module docstring and `_PRESET_ZONE_OVERRIDE_FIELDS`/`_ZONE_FIELD_FORM_KEY` are the authoritative field-by-field map; do not re-derive it from the wire format by hand a third time. |
| B5 | `POST /api/autotune/abort` | SAFETY_REDUCE | `autotune_abort` | Aborts a running autotune | none needed — this is itself a restore path; only meaningful once an autotune is started, which is Class C (C10) |
| B6 | `POST /api/adaptive_tune/revert` | ADMIN | `adaptive_tune_revert` | Reverts an adaptive-tune adjustment | Read `adaptive_tune_get_status` before/after; this route is itself a restore action, so only run it if adaptive-tune has actually adjusted something, and note the before-state |
| B7 | `POST /api/ramp_assist` | ADMIN | `ramp_assist_set_enabled` | Toggles ramp-assist on/off | Read `ramp_assist_get_enabled` first, set back to the original value |
| B8 | `POST /api/safety/rate_guard/auto` | ADMIN | `safety_set_rate_guard` | Overwrites rate-guard config | `safety_get_rate_guard` first, POST the unmodified body back |
| B9 | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | Clears a latched safety trip | No restore — a clear is the desired end state. Run this **only when the S6a dual-reset trip is confirmed by the reset row (B17), never standalone**: read `safety_get_status` first and confirm `trip_reason`/`trip_mask` show only `SAFETY_TRIP_MAIN_FAULT` (bit 5, `0x0020`) before clearing; clearing any other/unexplained trip is out of scope for this mechanical phase |
| B10 | `GET`/`POST /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled` / `set_watchdog_panic_disabled` | Disables watchdog PANIC (masks real overflow crashes while set) | Read current value first, set back to `false` (enabled) immediately after the test — never leave disabled |
| B11 | `POST /api/safety/log_level` | ADMIN | `safety_set_log_level` | Overwrites the safety-processor log verbosity level | Read the current level first (no dedicated GET exists today — capture the value you are about to overwrite from wherever it was last set, or from the safety link's own status if it echoes level), set it back after |
| B12 | `POST /api/relay_cycles/reset`, `POST /api/relay_cycles/restore` | ADMIN | none direct — raw HTTP | Resets/restores per-relay cycle counters | `restore` takes `c0..c4` (plus optional `allow_lower`) per `diagnostics_http.c` — capture the pre-reset per-relay counts from `/api/status`'s `relay_life` array FIRST and pass those exact five values back to `restore`. Per the MONOTONIC GUARD comment above `relay_cycles_restore_post_handler()`, `restore` does **not** refuse a value below the board's live count — it silently clamps that relay's value back up to its live count and names every clamped relay in the response body; only a malformed `allow_lower` fails the call outright. Check the response body for any clamped-relay list after calling `restore` — a clamp there means the round trip did NOT land the exact pre-reset counts and the mismatch must be reported, not silently accepted |
| B13 | `POST /api/dualwrite_window/restore_verified` | ADMIN | none direct — raw HTTP | Restores a dual-write window record | Superseded 2026-09-21: `cfg` is now mounted and populated on this bench (per `GET /api/cfgfs`), so this is no longer a true no-op — read `/api/dualwrite_window` before and after and confirm the actual effect |
| B14 | `POST /api/zones/current_sweep/abort` | SAFETY_REDUCE | `zone_current_sweep_abort` | Aborts a current-sweep (only meaningful if testing accidentally starts one — the start itself, `/api/zones/current_sweep/start`, is Class C, deferred) | none needed — abort is itself the restore |
| B15 | `POST /api/diagnostics/danger/stop` | SAFETY_REDUCE | none direct — `io_all_relays_off` is the safe fallback | Exits danger mode (only relevant if danger mode was entered — entering it is Class C, deferred) | none needed if danger mode was never entered; confirm via A25 first |
| B16 | `POST /api/ota/esp/recovery_exit` | ADMIN | `ota_recovery_exit_esp` | Exits recovery mode | Only testable while actually in recovery mode; no restore needed — exiting recovery is the desired end state |
| B17 | `POST /api/sw_reset` | ADMIN | `sw_reset_esp` | Reboots both processors, no config change | Expected S6a trip during the dual reflash window per CLAUDE.md — confirm link-up, then clear via B9 (only after this row confirms the reset happened) |
| B18 | `POST /api/kiln_configs/apply` | ADMIN | `load_config_preset`/`capability_preflight_check` (facade presets are file-based, not identical to board-stored slots) | Swaps active kiln config | Note which config was active before (via A32/A1), re-apply it after the test |
| B19 | `POST /api/kiln_configs/save` (new / overwrite-existing) | ADMIN | none direct — raw HTTP | Creates or overwrites a stored config slot | If overwriting, snapshot the existing slot via `GET /api/kiln_configs/export` first; if new, delete the created slot after via `POST /api/kiln_configs/delete`. If the store is quarantined (wrong-size persisted blob), every route in this row family refuses until `POST /api/kiln_configs/quarantine_clear` (`confirm=1`; `kiln_configs_quarantine_clear` MCP tool) — that discards whatever could not be read and starts a fresh, empty store, so only clear it on a board known to be safe to lose saved slots on |
| B20 | `POST /api/kiln_configs/clone` | ADMIN | none direct — raw HTTP | Creates a new slot | Delete the cloned slot after via `POST /api/kiln_configs/delete` |
| B21 | `POST /api/kiln_configs/rename` | ADMIN | none direct — raw HTTP | Renames a slot | Rename back to original name after |
| B22 | `POST /api/kiln_configs/delete` | ADMIN | none direct — raw HTTP | Deletes a slot | Only delete slots created in B19/B20 for this test; irreversible for pre-existing slots — never delete one that predates this test session |
| B23 | `POST /api/kiln_configs/import` | ADMIN | `convert_config` (offline conversion only, not the upload itself) | Imports a config as a new slot | Delete the imported slot after via B22 |
| B24 | `GET`/`POST /api/settings/display_power` | ADMIN | none direct — raw HTTP | Overwrites display power/timeout settings | Read via GET first, POST unmodified body back |
| B25 | `POST /api/unit_pref` | ADMIN | none direct — raw HTTP | Toggles C/F unit preference | Read current pref (main page state), set back after |
| B26 | `POST /api/auth/security` with `cmd=set_policy` only | ADMIN | none direct — raw HTTP | Overwrites `web_enabled`/`lcd_enabled`/`web_timeout_min`/`lcd_timeout_min` | Restricted to these four fields because they are, along with `admin_username` (also echoed but not writable via `set_policy`), the only ones `GET /api/auth/config` reads back (per `security_http.c`'s `security_config_get_handler`); `cmd=set_web_password`/`set_lcd_pin`/`clear_credentials` write one-way-hashed credentials with **no readable-back value at all** — those commands are Class C (C16), not exercised here. Read `GET /api/auth/config` first, POST the unmodified policy fields back, confirm via a second GET. **High caution**: per `project_web_auth_verified_and_blinds_pctools`, enabling `web_enabled` blinds PcTools clients until they log in — coordinate with the session holding bench credentials before toggling it |
| B27 | `POST /api/auth/bootstrap_password` | ADMIN_BOOTSTRAP | none direct — raw HTTP | Sets the first-run admin password | Only relevant on a freshly-erased board (post `flash_firmware(erase_partitions=...)`); this establishes the bench credential, coordinate value with the session owning `KILNCTL_WEB_PASSWORD` |
| B28 | `POST /api/auth/login` | OPEN | none direct — raw HTTP | Establishes a session | No restore — a session naturally expires/extends; **never iterate logins** per `project_login_latency_measured_4s` and the 2026-09-21 login-ladder owner decision — one attempt only |
| B29 | `POST /api/auth/session/extend` | USER | none direct — raw HTTP | Extends session | No restore needed |
| B30 | `POST /api/profile` (save), `POST /api/profile/delete`, `POST /api/profile/import`, `POST /api/profile/builtin/hide`, `POST /api/profile/builtin/restore`, `POST /api/profile/favorite` | ADMIN | `profiles_save`, `profiles_delete`; hide/restore/favorite/import have no direct tool — raw HTTP | Creates/deletes/hides/favorites a profile | Use a throwaway profile name for save/delete round-trip; restore hide/favorite/restore flags to prior state read via `profiles_list` first |
| B31 | `POST /provision`, `POST /forget`, `POST /ip_config` | ADMIN | `wifi_add_network`, `wifi_forget`, none direct (ip_config) | Adds/removes a Wi-Fi network entry, changes IP mode | Only exercise against a throwaway/test SSID entry; forget it afterward via `wifi_forget`; do not touch the bench's real LAN network entry or its DHCP/static mode |
| B32 | `POST /api/ota/esp/boot_guard_reset` | ADMIN | wired into `flash_firmware()`'s `reset_boot_guard` path, no standalone MCP tool | Clears the boot_guard recovery counter | Read `GET /api/boot_guard` before and after; this route is idempotent (clearing an already-clear counter is a no-op) and is normally fired automatically by `flash_firmware()` after a verified flash. The route is HMAC-signed (`X-Ota-Mac` header, context `OTA_HTTP_CONTEXT_BOOT_GUARD_RESET`) — a plain `curl` POST gets 401; it is executable only via `flash_firmware(reset_boot_guard=...)`, never a credential printed or logged |

Note: B1/B26 have no inverse route of their own — B1's restore is simply
that a firing was never (re-)started in this phase, since starting one is
Class C, and B26's restore is a read-then-write-back of the same
`GET`/`POST /api/auth/config`/`/api/auth/security` pair, not a separate
undo endpoint.

## Class C — Deferred / owner-scheduled (28 rows; not run in this phase)

These rows either start a firing, drive a relay outside the profile
executor's own safety-gated path, write a safety-config field (commissioning,
CT calibration, relay type), touch credentials with no readable-back value,
or are otherwise judged to need owner coordination (a real flash, an OTA
rollback, a partition format, a config import) rather than mechanical sweep
execution. Do not run them as part of the backend phase; the owner schedules
each one separately if/when appropriate.

| # | Route | Tier | MCP tool | Why deferred |
|---|---|---|---|---|
| C1 | `POST /api/zones/current_sweep/start` | ADMIN | `zone_current_sweep_start` | Energizes each zone's relay in turn to measure real amp draw — meaningless with the bench's 4 W fixture, and it is a relay-driving operation |
| C2 | `GET /api/tuning_recommendations` follow-through (using the sweep result) | ADMIN | none direct | Derived from C1's result; the read itself is Class A, but acting on the recommendation is not part of this phase |
| C3 | `POST /api/diagnostics/danger/enable` | ADMIN | none direct | Explicit purpose is driving relays outside the normal safety-gated path |
| C4 | `POST /api/diagnostics/danger/relay` (+ `.../start`, `dangerFiringBtn` equivalent) | ADMIN | none direct (`io_set_relay`/`io_set_relay_mask` are the lower-level equivalents) | Same reason as C3 |
| C5 | `POST /api/estop/verify` | ADMIN | none direct | Bench E-stop jumper is fitted (NOT asserted, per `project_estop_jumper_is_fitted`) — verifying the real switch needs the jumper removed and a physical actuation |
| C6 | `POST /api/factory_reset` (any scope, especially `all`) | ADMIN | `factory_default_then_load_preset` | Destructive; erases live config/credentials — schedule deliberately, not as a mechanical sweep item, and re-provision Wi-Fi/credentials afterward |
| C7 | `POST /api/ota/esp` (Wi-Fi OTA flash) | OPEN(challenge) then ADMIN | `ota_get_challenge`, `ota_update_esp` | A real flash; CLAUDE.md's sanctioned path for a real flash is `flash_firmware()` (JTAG/OpenOCD) — this is a different mechanism worth exercising deliberately, not folded into a mechanical backend sweep, since it changes the running image. **Note (2026-09-21):** declined in the owner-authorized M18 Class C run despite being named in the authorized range — the same message's "still forbidden" list separately named reflashing with no carve-out for Wi-Fi OTA, and it was read literally. The owner authorization of C7 as a row stands; the next run must be told this row is the carve-out that instruction meant to exempt |
| C8 | `POST /api/ota/pico` (Wi-Fi OTA to Pico) | ADMIN | `ota_update_pico` | Same reasoning as C7; also watch for the erase-watchdog-reset class fixed per `project_pico_ota_erase_watchdog_resets_safety_processor`. **Note (2026-09-21):** declined in the same M18 run for the same "still forbidden: reflashing" reason as C7 — owner authorization stands; the next run must be told this row is the carve-out |
| C9 | Setup wizard CT verification sweep start (`step8Start`) | ADMIN | `zone_current_sweep_start` | Same underlying route as C1 |
| C10 | `POST /api/autotune/start` → `POST /api/autotune/accept` (full accept path) | ADMIN | `autotune_start`, `autotune_accept` | Runs a real closed-loop step test on the bench fixture and can overwrite PID gains if accepted — a real config mutation, not a mechanical sweep item; only `autotune_abort` (B5) is in Class B |
| C11 | `POST /api/profile_exec/start` | USER | `profiles_start` | Starts a firing on the bench 4 W fixture — heating rows are deferred per this runbook's own scope, not run as part of the backend phase |
| C12 | `POST /api/profile_exec/pause` / `POST /api/profile_exec/resume` | USER | `profiles_pause` / `profiles_resume` | Only meaningful against a firing started under C11 |
| C13 | `POST /api/profile/live/fork` | ADMIN | `profile_live_fork` | Reachable only while a firing runs (per C11); refuses without `confirm=True`; a 409 here means no firing is active, or the fork itself failed (e.g. no free working slot) (per `profiles_live_http.c:313`) |
| C14 | `POST /api/profile/live` (save changes) | ADMIN | `profile_live_edit` | Reachable only while a firing runs; refuses without `confirm=True`; per `profiles_live_http.c:365` a 400 signals a bound violation and a 409 a window violation |
| C15 | `POST /api/profile/live/decide` (save as new / overwrite original / discard) | ADMIN | `profile_live_decide` | Reachable only while a firing runs; refuses without `confirm=True`; an overwrite decided against a builtin-origin profile is refused 403 |
| C16 | `POST /api/auth/security` with `cmd=set_web_password`/`set_lcd_pin`/`clear_credentials` | ADMIN | none direct — raw HTTP | Writes one-way-hashed credentials with no readable-back value at all (unlike `cmd=set_policy`, Class B26) — owner coordinates the value with the session holding bench credentials; never write a new credential value into any log/doc |
| C17 | `POST /api/safety/commissioning` (stage & commit) | ADMIN | `safety_set_commissioning_fields` | Overwrites guarded-value commissioning fields — safety config, owner-scheduled |
| C18 | `POST /api/safety/commissioning/bench_preset` | ADMIN | none direct — raw HTTP | Applies the bench-values preset (this IS the bench setup — see `project_ct_calibration_closed_and_presence_branch`); safety config, owner-scheduled. **Precondition (confirmed 2026-09-21):** on this bench's build, `GET /api/safety/commissioning` reports `dev_tools_enabled:false` — the handler is compiled out behind `CONFIG_KILNCTL_DEV_TOOLS`, which defaults `n` (`firmware/KilnFW/App/Kconfig.projbuild:16-18`). Expected result on a default build: 404, N/A unless the build enables dev tools |
| C19 | `POST /api/safety/commissioning/ct_auto_zero`, `.../ct_trim`, `.../ct_cal` | ADMIN | `safety_set_commissioning_fields` (ct_cal only) / raw HTTP (auto_zero, trim) | Adjusts CT calibration state; per `project_ct_calibration_closed_and_presence_branch` this is already closed on bench — do not re-tune without owner sign-off |
| C20 | `POST /api/safety/commissioning/relay_type` | ADMIN | none direct — raw HTTP | Overwrites relay-type field — safety config, owner-scheduled |
| C21 | `POST /api/crash_report/ack` | ADMIN | `crash_report_ack` | Acknowledging a crash record is a real disposition of diagnostic evidence — owner reviews the record first (tool itself refuses without `confirm=True` and verifies the ack landed). **Note (2026-09-21):** declined in the owner-authorized M18 Class C run — the same message's "still forbidden" list separately named "acknowledging crash reports", read as controlling despite C21 falling inside the nominally authorized range. Owner authorization stands; the next run must be told this row is the carve-out |
| C22 | `POST /api/crash_report/clear` | ADMIN | none direct — raw HTTP | Clears the crash log entirely; irreversible (log content is gone) — only after C21's ack and owner review. **Note (2026-09-21):** declined transitively when C21 was declined (this row requires C21's ack first) — same carve-out disclosure applies |
| C23 | `POST /api/ota/esp/rollback` | ADMIN | `ota_rollback_esp` | Rolls back running ESP image to previous OTA slot. Mind the `zones_cfg` schema-bump hazard in CLAUDE.md — a rollback past a `ZONES_CFG_VERSION` bump runs on firmware-default PID gains, not the tuned ones, until reflashed; read `control_get_zones` after any rollback before further test, and coordinate a `flash_firmware()` pass with the flashing session afterward. **Note (2026-09-21):** declined in the owner-authorized M18 Class C run — reflashing was separately forbidden that session, leaving no safe restore path if a rollback landed on stale/default gains. Owner authorization stands; the next run must be told this row is the carve-out (and that a `flash_firmware()` pass is available to it) |
| C24 | `POST /api/ota/pico/rollback` | ADMIN | `ota_rollback_pico` | Same restore approach as C23 — reflash after, coordinate with the flashing session. **Note (2026-09-21):** declined in the same M18 run for the same no-safe-restore-path reason as C23; owner authorization stands, next run must be told this row is the carve-out |
| C25 | `POST /api/backup/import` | ADMIN | none direct — raw HTTP | Restores config from a backup file — silently overwrites live config unless the imported file is a backup taken from THIS board moments earlier via A34; owner-scheduled rather than run mechanically |
| C26 | `POST /api/cfgfs/format_confirm` (+ `GET .../format_pending`) | ADMIN | none direct — raw HTTP | Formats the `cfg` LittleFS partition. **Superseded 2026-09-21 — this precondition is now stale, not merely owner-gated:** the partition is mounted and file-backed with an NVS mirror (`get_readiness`'s `cfg_fs: ok`, confirmed via `GET /api/cfgfs` showing 7 files; `project_cfg_partition_and_user_data_move`). Formatting it today is a genuinely destructive action against live mounted config data, not the inert no-op this row originally assumed. NVS remains authoritative throughout (`docs/CONFIG_FILESYSTEM.md`), so no data is unrecoverable, but the `cfg` mirror is **not** rewritten as a side effect of the format or of remounting — `finish_mount_after_register()` only re-registers the write-function seam on the freshly (empty) formatted filesystem (`firmware/KilnFW/App/drivers/persist/cfg_fs_mount.c:199-223`); each of the 11 dual-write items (zones config, profiles, kiln_cfg_store fields, firing stats, unit preference, ramp-assist enable, display power/backlight policy, adaptive-tune Ki baseline, relay cycle counters — full list in `docs/CONFIG_FILESYSTEM.md`'s dual-write table) only reappears in `cfg` the next time that store's own `*_cfg_fs_save()` is called (e.g. `zones_config_cfg_fs_save()`, `firmware/KilnFW/App/drivers/persist/zones_config_cfg_fs.h:122`). Still defer to the owner, now on destructiveness grounds rather than the old "inert" premise |
| C27 | `POST /api/settings/tz` | ADMIN | none direct — raw HTTP | No `GET /api/settings/tz` route exists in `route_tier_table.h` — there is no concrete read source to restore from (checked `firmware/KilnFW/App/drivers/http/settings_http.c`: only the POST handler is registered, and neither `/api/status` nor any other route echoes the current tz value) — defer until a read path exists or the owner supplies the pre-test value out of band |
| C28 | `POST /api/cfgfs/file?name=<name>` | ADMIN | none direct — raw HTTP | Write half of the cfg-partition file route; superseded 2026-09-21: `cfg` is now mounted and populated on this bench (per `GET /api/cfgfs`), so a write here has a real, verifiable effect — read the file back afterward and restore its prior content |

---

## Gaps found while preparing this runbook

An opus review of the first pass found rows missing entirely, not just
mistiered: the live-profile-edit route group (`GET /api/profile/live`,
`POST /api/profile/live/fork`, `POST /api/profile/live`, `POST
/api/profile/live/decide` — A45 and C13-C15, and the MCP tools `profile_live_get`/
`_fork`/`_edit`/`_decide` from `docs/LIVE_PROFILE_EDIT_PLAN.md` section 10),
`GET /api/profile/export` (A46), `POST /api/ota/esp/boot_guard_reset` (B32),
`GET /api/logs/firing`/`GET /api/logs/autotune` (A47), `POST
/api/safety/log_level` with its `safety_set_log_level` tool (B11), and
`GET`/`POST /api/cfgfs/file` (A48/C28) were all present in `route_tier_table.h`
and/or the matrix's own "API-only routes" table but absent from this
document's first pass. The original "no route or MCP tool named in this
document was found missing" claim was true only of the routes that *were*
already named here — it did not mean every backend-phase route from the
matrix had been carried over, and is corrected accordingly: those six
additions are now included above, and the matrix's own live-profile-edit
group tool column (previously "none") is corrected to name the four tools.

No route or MCP tool actually named anywhere in this document (after the
additions above) was found missing from `route_tier_table.h` or
`tools/PcTools/src/kilnctrl/mcp_server_*.py`. This matches the matrix's own
"Gaps and caveats" section, which already flags the routes with no direct
MCP tool (profile import/hide/restore/favorite, kiln_configs
save/clone/rename/delete/import, backup import/export, security saves,
danger-mode relay control, cfgfs file read/write) — those are carried
forward into this runbook's "raw HTTP" rows above rather than re-discovered.
One further gap the review surfaced: `POST /api/settings/tz` (C27) has no
corresponding `GET` route anywhere in `route_tier_table.h`, so unlike the
other Class C rows there is no concrete read source to snapshot the
pre-test value from — it is deferred for that reason specifically, not only
because it is a config write.

Two further corrections made while cross-checking against
`route_tier_table.h` directly:

- `GET /api/ota/esp/status` is OPEN-tier (confirmed in the table), not ADMIN
  — the matrix's own page-load row already notes this split correctly.
- `POST /api/dualwrite_window/restore_verified` (B13) is present in the
  table with a comment noting it is not in any older plan document's route
  enumeration; it is included here since it is exactly the kind of
  state-changing route this phase should exercise once, with a read-back.
