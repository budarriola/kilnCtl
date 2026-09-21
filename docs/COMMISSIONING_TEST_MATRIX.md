# Commissioning Test Matrix

Exhaustive inventory of every web (HTTP/MCP) and LCD function KilnFW offers,
for working through the bench-commissioning pass authorized 2026-09-20 (see
`project_bench_update_and_commission_authorized_2026_09_20` memory note).
Bench context: 4 W test-fixture load (not a real kiln), E-stop jumper fitted
(NOT asserted), no consumable/real heating element, one MAX31856 per channel
on the daughterboards. Source snapshot: `origin/main` as of the `9ab1b9ac`
commit this worktree started from.

## Owner's test order

1. **Backend first** -- HTTP routes directly, or via MCP (`kiln_find` /
   `kiln_call` / `kiln_batch`, `kicad_*` not applicable here). Fastest signal,
   no browser or LCD involved.
2. **Web UI second** -- headless-Chrome/CDP infrastructure already in the
   tree: `tools/verify.ps1`'s lint stage (`tools/check_lint_pages.ps1` wraps
   `lint_pages.js`), and `firmware/KilnFW/App/test/check_ui_responsive_sweep.ps1`
   (drives real headless Chrome over CDP, phase-3-serial in
   `run_all_checks.ps1`). `mcp__kilnctrl` also has a `ui` group
   (`board_page_structure`, `list_buttons`, `press_button`) for structural/
   functional page checks without a human driving a browser.
3. **LCD last** -- `mcp__kilnctrl` `touch_inject`/`touch_get_state`/
   `touch_log_tap_targets` for synthetic taps, then
   `tools/PcTools/scripts/capture_lcd.ps1` (crop `X=296 Y=58 W=853 H=578` as
   of the 2026-09-19 camera re-aim) + `sample_lcd_region.ps1` for **numeric
   pixel sampling** -- never judge LCD colors/state by eye.

## Counts

| Metric | Count |
|---|---|
| Web pages (page-shell HTML files served) | 18 |
| LCD pages (`kiln_ui_register_page` registrations) | 16 |
| Distinct web controls inventoried below (buttons/inputs/selects) | ~120 (button scan; text/number/select inputs not separately enumerated per-page, see note) |
| HTTP routes (`route_tier_table.h`, authoritative) | 138 (`ROUTE_TIER_TABLE_COUNT`; +1 undocumented-in-plan `POST /api/dualwrite_window/restore_verified` already included) |
| Routes with an MCP facade tool | ~95 (estimate; every `safety`, `profiles`, `zones`, `ota`, `wifi`, `autotune`, `adaptive_tune`, `control`, `ramp`, `system` group route is wrapped; page-shell GETs and a handful of settings/backup/security POSTs are not) |
| Routes classified hardware-gated below | ~30 (relay/heat-driving, OTA/reboot, CT sweep, factory reset, E-stop verify, danger mode) |

Note on control counts: the button scan below is exhaustive for `<button>`
elements (161 raw matches across 18 pages, deduplicated per page). Free-text/
number inputs and `<select>` dropdowns exist on nearly every settings/zones/
profile page (PID gains, thresholds, names) but are not separately
enumerated one-by-one here -- they are covered by the "Save"/"Apply" button
that submits them, which the table lists.

## Legend for the Result column

`PASS` / `FAIL` / `BLOCKED` / `N-A`, followed by `YYYY-MM-DD` and an evidence
pointer (log path, screenshot, MCP call + response summary, or commit hash).
Leave blank until exercised. `BLOCKED` = hardware-gated on this bench, not a
defect.

---

## Page-by-page inventory

Each row: Control -- Route(s) + method -- Auth tier -- MCP facade tool -- LCD
equivalent -- Testable on bench vs hardware-gated -- Result.

### `/` -- main dashboard (`main_page.html`, `app.js`, `nav.js`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Dashboard poll (page load) | `GET /api/status` | OPEN | `get_board_state` (superset) | Home page | Testable | |
| Start firing | `POST /api/profile_exec/start` | USER | `profiles_start` | Home "Start" / profile picker | Testable (bench load) | |
| Stop firing | `POST /api/profile_exec/stop` | SAFETY_REDUCE | `profiles_stop` | Home stop control | Testable | |
| Pause firing | `POST /api/profile_exec/pause` | USER | `profiles_pause` | -- | Testable | |
| Resume firing | `POST /api/profile_exec/resume` | USER | `profiles_resume` | -- | Testable | |
| Dismiss last run (`ackLastRunBtn`) | `POST /api/profile_exec/ack_last_run` | USER | `profiles_ack_last_run` | -- | Testable | |
| Clear Trip (`clearTripBtn`) | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | Safety page clear-trip | Testable (bench can trip S6a) | |
| PID popup: Use these / Apply (`pidPopupUseProposed`/`pidPopupApplyBtn`) | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | |
| Profile feasibility icon / popup Proceed anyway | reads `GET /api/profile_plan` | OPEN | none (facade has no feasibility-specific tool; use `get_board_state`/plan reads) | -- | Testable | |
| Relay-life icon | reads status fields | OPEN | none | -- | Testable | |
| Theme toggle (`themeBtn`, every page) | client-side only, no route | -- | none | -- | Testable | |
| Live history chart/CSV | `GET /api/history.csv` | OPEN | none direct (`get_device_log`/`log_analyze` adjacent) | Home graph | Testable | |
| Control status | `GET /api/control` | ADMIN | `control_get_zones` | -- | Testable | |
| Firing history | `GET /api/firing_history` | OPEN | none direct | -- | Testable | |

### `/profiles` -- profile library (`profiles_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / list | `GET /api/profiles`, `GET /api/profiles/builtin`, `GET /api/profiles/favorites` | USER | `profiles_list` | "profiles" / "profile_picker" / "profiles_builtin_list" pages | Testable | |
| Save profile (`saveBtn`) | `POST /api/profile` | ADMIN | `profiles_save` | Profile builder review save | Testable | |
| New (`newBtn`) | client-side form reset, then Save | -- | -- | -- | Testable | |
| Import (`importBtn`) | `POST /api/profile/import` | ADMIN | none (facade has no import wrapper; use raw HTTP) | -- | Testable | |
| Export (`modeExportBtn` -> per-row) | `GET /api/profile/export` | USER | none | -- | Testable | |
| Delete (`modeDeleteBtn` -> per-row, `bulkActionBtn`) | `POST /api/profile/delete` | ADMIN | `profiles_delete` | -- | Testable | |
| Restore removed (`restoreBtn`) | `POST /api/profile/builtin/restore` | ADMIN | none direct | -- | Testable | |
| Hide builtin (row action, not a top button) | `POST /api/profile/builtin/hide` | ADMIN | none direct | -- | Testable | |
| Favorite toggle (row action) | `POST /api/profile/favorite` | ADMIN | none direct | -- | Testable | |
| Add segment / add out-of-band rule (`addSegBtn`/`addOoBtn`) | client-side, folded into Save | -- | -- | Profile builder segment page | Testable | |
| Bulk cancel (`bulkCancelBtn`) | client-side only | -- | -- | -- | Testable | |
| Profile detail read | `GET /api/profile` | USER | `profiles_get` | "profile_detail"/"profile_segments" pages | Testable | |

### `/live_profile` -- live in-run profile edit (`live_profile_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/profile/live` | ADMIN | none direct | none (LCD has no live-edit page) | Testable only while a firing is running | |
| Fork the running profile (`forkBtn`) | `POST /api/profile/live/fork` | ADMIN | none | -- | Testable | |
| Save changes (`saveBtn`) | `POST /api/profile/live` | ADMIN | none | -- | Testable | |
| Save as new / Overwrite original / Discard (`saveAsBtn`/`overwriteBtn`/`discardBtn`) | `POST /api/profile/live/decide` | ADMIN | none | -- | Testable | |
| Reload from board (`reloadBtn`) | `GET /api/profile/live` | ADMIN | none | -- | Testable | |

### `/settings/zones` -- zones & PID (`zones_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / zone config | `GET /api/zones`, `GET /api/zones_diag` | ADMIN | `control_get_zones` | -- (no LCD zones editor) | Testable | |
| Save (`saveBtn`) | `POST /api/zones` | ADMIN | none direct (raw HTTP; `control_set_zone_pid`/`control_set_zone_model` cover the PID/model sub-fields) | -- | Testable | |
| PID save | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | |
| Measure Normal Current (`sweepStartBtn`) | `POST /api/zones/current_sweep/start` | ADMIN | `zone_current_sweep_start` | -- | **Hardware-gated**: energizes each zone's relay in turn to measure real amp draw -- meaningful only with a real heating-element load; bench's 4 W fixture reads near-zero/noise | |
| Abort sweep (`sweepAbortBtn`) | `POST /api/zones/current_sweep/abort` | SAFETY_REDUCE | `zone_current_sweep_abort` | -- | Testable (abort path itself, even if the sweep's numbers are meaningless on bench) | |
| Sweep status poll | `GET /api/zones/current_sweep/status` | ADMIN | `zone_current_sweep_status` | -- | Testable | |
| CT channel map read | `GET /api/zones/ct_channel_map` | ADMIN | none direct | -- | Testable | |
| Recommend (`tuningRecGoBtn`) | `GET /api/tuning_recommendations` | ADMIN | none direct | -- | **Hardware-gated**: recommendations are derived from the current-sweep result above | |
| Start step test (`atStartBtn`) | `POST /api/autotune/start` | ADMIN | `autotune_start` | -- | Testable on bench (closed-loop step response exists even at 4 W, though gains found are not representative of a real kiln -- see `project_bench_identification_limits`) | |
| Abort (`atAbortBtn`) | `POST /api/autotune/abort` | SAFETY_REDUCE | `autotune_abort` | -- | Testable | |
| Accept proposed gains (`atAcceptBtn`) | `POST /api/autotune/accept` | ADMIN | `autotune_accept` | -- | Testable | |
| Autotune status/matrix | `GET /api/autotune`, `GET /api/autotune/matrix` | ADMIN | `autotune_get_status` | -- | Testable | |
| Autotune trace CSV | `GET /api/autotune/trace.csv` | ADMIN | none direct | -- | Testable | |
| Adaptive-tune revert (per-zone) | `POST /api/adaptive_tune/revert` | ADMIN | `adaptive_tune_revert` | -- | Testable | |
| Adaptive-tune status | `GET /api/adaptive_tune` | ADMIN | `adaptive_tune_get_status` | -- | Testable | |
| Ramp-assist toggle (also on diagnostics page) | `GET`/`POST /api/ramp_assist` | ADMIN | `ramp_assist_get_enabled`/`ramp_assist_set_enabled` | -- | Testable | |

### `/settings/safety` -- safety config page (`safety_config_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/safety/rate_guard/auto` | ADMIN | `safety_get_rate_guard` | Home safety/temperature page has read-only status only | Testable | |
| Save (`save`) | `POST /api/safety/rate_guard/auto` | ADMIN | `safety_set_rate_guard` | -- | Testable | |

### `/safety` -- safety status (`safety_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | reads `GET /api/status` safety fields | OPEN | `safety_get_status` | Home page safety banner | Testable | |
| Clear latched trip (`clearTripBtn`) + confirm dialog (`confirmYes`/`confirmNo`) | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | -- | Testable (E-stop jumper fitted means S3/estop trips are not reachable this way on bench; S6a mainFault from a dual reflash is, per CLAUDE.md) | |

### `/safety/commissioning` -- guarded-value commissioning wizard (`safety_commissioning_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/safety/commissioning` | ADMIN | `safety_get_commissioning` | -- | Testable | |
| Wizard Start/Next/Back (`gStartBtn`, `gGoto`) | client-side only | -- | -- | -- | Testable | |
| Stage & commit (`gCommitBtn`, `saveBtn`) | `POST /api/safety/commissioning` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | |
| Apply test preset (`benchBtn`, dev-only, hidden by default) | `POST /api/safety/commissioning/bench_preset` | ADMIN | none direct | -- | Testable -- this is literally the bench-values preset button | |
| Relay-type field (submitted with commit) | `POST /api/safety/commissioning/relay_type` | ADMIN | none direct | -- | Testable | |
| CT calibration Apply (`.ct-cal-apply`) | `POST /api/safety/commissioning/ct_cal` | ADMIN | `safety_set_commissioning_fields`(fields incl. ct_cal) / raw | -- | Testable (bench CT calibration already closed, see `project_ct_calibration_closed_and_presence_branch`) | |
| CT Auto-zero (`.ct-cal-auto-zero`) | `POST /api/safety/commissioning/ct_auto_zero` | ADMIN | none direct | -- | Testable | |
| CT trim Apply (`.ct-trim-apply`) | `POST /api/safety/commissioning/ct_trim` | ADMIN | none direct | -- | Testable | |

### `/diagnostics` -- diagnostics (`diagnostics_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / thermo faults | `GET /api/thermo/faults` | ADMIN | `thermo_read_faults` | "diagnostics" LCD page | Testable | |
| Crash report banner + Acknowledge (`crashAckBtn`) | `GET /api/crash_report`, `POST /api/crash_report/ack` | ADMIN | `crash_report_ack` | -- | Testable | |
| Clear crash log (`crashClearBtn`) | `POST /api/crash_report/clear` | ADMIN | none direct | -- | Testable | |
| Watchdog PANIC toggle (`wdPanicToggleBtn`) | `GET`/`POST /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled`/`set_watchdog_panic_disabled` | -- | Testable but disruptive -- disabling PANIC masks real overflow-class crashes; use deliberately | |
| Ramp-assist toggle (`rampAssistToggleBtn`) | `GET`/`POST /api/ramp_assist` | ADMIN | `ramp_assist_*` | -- | Testable | |
| Enter Danger Mode (`dangerEnterBtn`) | `POST /api/diagnostics/danger/enable` | ADMIN | none direct | -- | **Hardware-gated**: explicit purpose is driving relays outside the normal safety-gated path -- exercise only with the bench fixture's 4 W load, never near a real element |
| Per-relay danger control (`dangerRelayBtn<N>`, `dangerFiringBtn`) | `POST /api/diagnostics/danger/relay` | ADMIN | none direct (`io_set_relay`/`io_set_relay_mask` are the lower-level equivalents) | -- | **Hardware-gated** (same reason) | |
| Exit Danger Mode now (`dangerExitBtn`) | `POST /api/diagnostics/danger/stop` | SAFETY_REDUCE | none direct (`io_all_relays_off` is the safe fallback) | -- | Testable (the exit path itself) | |
| Danger status poll | `GET /api/diagnostics/danger` | ADMIN | none direct | -- | Testable | |
| E-stop verify | `POST /api/estop/verify` | ADMIN | none direct | -- | **Hardware-gated**: bench E-stop jumper is fitted (NOT asserted, per `project_estop_jumper_is_fitted`) -- verifying the real switch needs the jumper removed and a physical actuation |
| Relay cycle counters: reset/restore per-relay (`relay-reset-btn`) | `POST /api/relay_cycles/reset`, `POST /api/relay_cycles/restore` | ADMIN | none direct | -- | Testable | |
| Dual-write window record restore (`dwwRecordRestoreBtn`) | `POST /api/dualwrite_window/restore_verified` | ADMIN | none direct | -- | Testable (per `project_cfg_partition_and_user_data_move`, `cfg` partition is unformatted/inert on this bench, so verify this is a true no-op there) | |
| Dual-write window status | `GET /api/dualwrite_window` | ADMIN | none direct | -- | Testable | |
| lwIP stats | `GET /api/debug/lwip_stats` | ADMIN | none direct | -- | Testable | |
| Timing diagnostics | `GET /api/diagnostics/timing` | ADMIN | none direct | -- | Testable | |
| SaftyFW stack margin | `GET /api/saftyfw_stack_margin` | ADMIN | none direct (`get_stack_margin` covers the ESP side only) | -- | Testable | |
| Coredump info/chunk (implicit, feeds crash report tooling) | `GET /api/coredump/info`, `GET /api/coredump/chunk` | ADMIN | `read_esp_coredump` | -- | Testable | |
| cfg filesystem status / file read+write | `GET /api/cfgfs`, `GET`/`POST /api/cfgfs/file` | ADMIN | `get_cfgfs_status` (status only; file get/post has no wrapper) | -- | Testable | |

### `/settings` -- settings hub (`settings_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Reboot both processors, no config change (`swResetBtn`) | `POST /api/sw_reset` | ADMIN | `sw_reset_esp` | -- | Testable (watch for the expected S6a trip during a dual reflash, per CLAUDE.md) | |
| Reset Wi-Fi only (`data-scope="wifi"`) | `POST /api/factory_reset` (scope param) | ADMIN | none direct (`factory_default_then_load_preset`/`wifi_forget` are the nearest facade equivalents) | -- | Testable | |
| Reset kiln config only (`data-scope="kiln"`) | `POST /api/factory_reset` | ADMIN | `factory_default_then_load_preset` | -- | Testable | |
| Reset fire profiles only (`data-scope="profiles"`) | `POST /api/factory_reset` | ADMIN | none direct | -- | Testable | |
| Factory default -- erase everything (`data-scope="all"`) | `POST /api/factory_reset` | ADMIN | `factory_default_then_load_preset`(scope=all) | -- | Testable, but destructive -- re-provision Wi-Fi/credentials afterward | |
| Format cfg partition (`cfgFsFormatConfirmBtn`) | `GET /api/cfgfs/format_pending`, `POST /api/cfgfs/format_confirm` | ADMIN | none direct | -- | Testable (`cfg` partition is unformatted/inert on this bench today) | |
| Timezone save | `POST /api/settings/tz` | ADMIN | none direct | -- | Testable | |

### `/settings/display` -- display settings (`settings_display_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/settings/display_power` | ADMIN | none direct | LCD screen-idle/timeout behavior (`screen_idle.c`) | Testable | |
| Save (`kcDpSave`) | `POST /api/settings/display_power` | ADMIN | none direct | -- | Testable | |
| Unit preference (temp C/F, submitted from main page, not this page) | `POST /api/unit_pref` | ADMIN | none direct | Home page unit toggle | Testable | |

### `/settings/security` -- credentials & auth policy (`security_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/auth/config` | ADMIN | none direct | -- (no LCD credentials editor) | Testable | |
| Save administrator/user password, admin/user PIN, policy (`kcSecAdminPwSave` etc.), Clear login credentials (`kcSecClearCreds`) | `POST /api/auth/security` | ADMIN | none direct | -- | Testable -- see `project_web_auth_verified_and_blinds_pctools`: enabling auth policy blinds PcTools clients until they log in, plan the session accordingly | |
| Bootstrap admin password (first-run, `login_page.html`'s "Set password" form) | `POST /api/auth/bootstrap_password` | ADMIN_BOOTSTRAP | none direct | -- | Testable | |
| Log in (`login_page.html`) | `POST /api/auth/login` | OPEN | none direct | -- | Testable; see `project_login_latency_measured_4s` and `project_owner_decisions_2026_09_21_login` for expected latency/lockout behavior -- never iterate logins | |
| Session status poll / extend | `GET /api/auth/session`, `POST /api/auth/session/extend` | OPEN / USER | none direct | -- | Testable | |

### `/settings/kiln_configs` -- config presets (`kiln_configs_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / list | `GET /api/kiln_configs` | USER | `list_config_presets` (local presets; board-stored slots have no direct wrapper) | -- | Testable | |
| Apply (`kcApplyBtn`) | `POST /api/kiln_configs/apply` | ADMIN | `load_config_preset`/`capability_preflight_check` (facade presets are file-based, not identical to these board-stored slots) | -- | Testable | |
| Apply status poll | `GET /api/kiln_configs/apply_status` | USER | none direct | -- | Testable | |
| Save as new (`kcSaveNewBtn`) | `POST /api/kiln_configs/save` | ADMIN | none direct | -- | Testable | |
| Overwrite selected (`kcOverwriteBtn`) | `POST /api/kiln_configs/save` (existing id) | ADMIN | none direct | -- | Testable | |
| Clone selected (`kcCloneBtn`) | `POST /api/kiln_configs/clone` | ADMIN | none direct | -- | Testable | |
| Rename (`kcRenameBtn`) | `POST /api/kiln_configs/rename` | ADMIN | none direct | -- | Testable | |
| Delete selected (`kcDeleteBtn`) | `POST /api/kiln_configs/delete` | ADMIN | none direct | -- | Testable | |
| Download selected (`kcDownloadBtn`) | `GET /api/kiln_configs/export` | ADMIN | none direct | -- | Testable | |
| Upload/import (`kcUploadBtn`) | `POST /api/kiln_configs/import` | ADMIN | `convert_config` (offline conversion only, not the upload itself) | -- | Testable | |

### `/settings/backup` -- backup/restore (`backup_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / Export (implicit download link) | `GET /api/backup/export` | ADMIN | none direct | -- | Testable -- see `project_backup_round_trip_coverage`, only the Wi-Fi password is irreducible across a round trip | |
| Restore from file (`restoreBtn`) | `POST /api/backup/import` | ADMIN | none direct | -- | Testable | |

### `/ota` -- firmware update (`ota_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / interlock status | `GET /api/ota/interlock`, `GET /api/ota/esp/status`, `GET /api/ota/pico/status` | ADMIN / OPEN(esp status) / ADMIN(pico status) | `ota_status` | -- (no LCD OTA UI) | Testable | |
| Update ESP (`espUpdateBtn`) | `GET /api/ota/challenge` then `POST /api/ota/esp` | OPEN then ADMIN | `ota_get_challenge`, `ota_update_esp` | -- | Testable, but CLAUDE.md's sanctioned path for a real flash is `flash_firmware()` (JTAG/OpenOCD) -- this route is the Wi-Fi OTA path, a different mechanism, both worth exercising | |
| Roll back ESP (`espRollbackBtn`) | `POST /api/ota/esp/rollback` | ADMIN | `ota_rollback_esp` | -- | Testable; mind the `zones_cfg` schema-bump rollback hazard in CLAUDE.md | |
| Exit recovery mode & reboot now (`recoveryExitBtn`) | `POST /api/ota/esp/recovery_exit` | ADMIN | `ota_recovery_exit_esp` | -- | Testable only while actually in recovery mode | |
| boot_guard_reset (fired automatically by `flash_firmware()`, no dedicated button) | `POST /api/ota/esp/boot_guard_reset` | ADMIN | wired into `flash_firmware()`'s `reset_boot_guard` path, no standalone MCP tool | -- | Testable | |
| boot_guard status | `GET /api/boot_guard` | ADMIN | none direct | -- | Testable | |
| Update Pico (`picoUpdateBtn`) | `POST /api/ota/pico` | ADMIN | `ota_update_pico` | -- | Testable; watch for the erase-watchdog-reset class, fixed per `project_pico_ota_erase_watchdog_resets_safety_processor` | |
| Roll back Pico (`picoRollbackBtn`) | `POST /api/ota/pico/rollback` | ADMIN | `ota_rollback_pico` | -- | Testable | |
| Pico rollback status | `GET /api/ota/pico/rollback/status` | ADMIN | none direct | -- | Testable | |
| Partition table read (support for above, no button) | `GET /api/partitions` | ADMIN | `debug_check_partition_table` | -- | Testable | |

### `/readiness` -- pre-firing readiness (`readiness_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load (no buttons besides theme) | `GET /api/readiness` | OPEN | none direct (`capability_preflight_check` is the nearest facade equivalent, board-agnostic) | -- | Testable | |

### `/setup` -- first-run setup wizard (`setup_wizard_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / progress | `GET /api/setup/progress` | ADMIN | none direct | -- | Testable | |
| Resume where left off (`resumeBtn`) / Refresh (`loadAll`) / step navigation (`stepgo`) | client-side + `GET /api/setup/progress` | ADMIN | -- | -- | Testable | |
| Step Save & mark done buttons (`step0Continue`..`step6Save`) | `POST /api/setup/progress` (+ underlying config routes: zones, safety, security depending on step) | ADMIN | matches whichever underlying group the step edits | -- | Testable | |
| Apply channel/relay count (`s2rebuild`/`s5rebuild`) | `POST /api/zones` (channel/relay count fields) | ADMIN | none direct | -- | Testable | |
| CT verification sweep (`step8Start`/`step8Abort`/`step8Skip`/`step8SkipLater`) | `POST /api/zones/current_sweep/start`/`abort` | ADMIN / SAFETY_REDUCE | `zone_current_sweep_start`/`_abort` | -- | **Hardware-gated** start (same as zones-page sweep); abort/skip testable | |
| CT calibration Apply (`.ct-cal-apply`) | `POST /api/safety/commissioning/ct_cal` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | |
| Save gains per zone (`s10save`) | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | |
| Save credentials & policy (`step11Save`) / Skip (`step11Skip`) | `POST /api/auth/security` / `POST /api/setup/progress` | ADMIN | none direct | -- | Testable | |
| Stage & commit (`step7Save`) | `POST /api/safety/commissioning` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | |

### `/`, `/wifi`, `/status`, `/scan`, `/provision`, `/networks`, `/forget`, `/ip_config` -- Wi-Fi provisioning (`wifi_provision_page.html`, AP-mode index)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load (AP-mode captive index) | `GET /` (AP context), `GET /wifi` | OPEN | `wifi_get_status` | "network" LCD page | Testable | |
| Home Wi-Fi / Access Point mode (`modeHomeBtn`/`modeApBtn`) | implicit via `POST /provision` or mode switch | ADMIN | `wifi_set_mode` | "network_manage" LCD page | Testable | |
| Scan (`scanBtn`) | `GET /scan` | OPEN | `wifi_scan` | -- | Testable | |
| Connect (`connectSubmitBtn`) | `POST /provision` | ADMIN | `wifi_add_network` | -- | Testable | |
| Cancel (`connectCancelBtn`) | client-side only | -- | -- | -- | Testable | |
| Forget network (per-row, not a top button) | `POST /forget` | ADMIN | `wifi_forget` | -- | Testable | |
| DHCP/Static (`ipModeDhcpBtn`/`ipModeStaticBtn`) + Save | `POST /ip_config` | ADMIN | none direct | -- | Testable | |
| Networks list | `GET /networks` | OPEN | `wifi_get_networks` | -- | Testable | |
| Status poll | `GET /status` | OPEN | `wifi_get_status` | -- | Testable | |

### Static/shared assets (no page shell of their own)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| `theme.css`, `nav.js`, `app.js`, `commissioning_shared.js` | `GET /theme.css`, `GET /nav.js`, `GET /app.js`, `GET /commissioning_shared.js` | OPEN | none (served assets, checked by `check_lint_pages.ps1`/responsive sweep, not the MCP facade) | -- | Testable | |

---

## LCD pages (`kiln_ui_register_page`, `firmware/KilnFW/App/drivers/ui/kiln_ui.c`)

16 registered pages. Each is reached via `touch_inject`/physical tap and
verified with `capture_lcd.ps1` + `sample_lcd_region.ps1` numeric sampling
(never by eye, per CLAUDE.md and `project_st7796_rgb565_byte_order`).

| LCD page name | Source | Purpose | Web equivalent | Bench class |
|---|---|---|---|---|
| `home` | `ui_page_home.c` (+`_actions`, `_chart`, `_graph`, `_rail`, `_refresh`) | Live status, start/stop, graph | `/` | Testable |
| `config` | `ui_page_config.c` | Device config menu | `/settings` (partial) | Testable |
| `temperature` | `ui_page_temperature.c` (+`_safety`) | Per-zone temperature + safety status | `/safety`, `/api/status` | Testable |
| `network` | `ui_page_network.c` | Wi-Fi status | `/status`, `/wifi` | Testable |
| `network_manage` | `ui_page_network_manage.c` | Wi-Fi scan/connect/forget | `/scan`, `/provision`, `/forget` | Testable |
| `diagnostics` | `ui_page_diagnostics.c` | On-device diagnostics | `/diagnostics` (subset) | Testable |
| `touch_cal` | `ui_page_touch_cal.c` | Touch calibration | none (LCD-only; `KILNCTL_TOUCH_CAP_*` knobs, not the inert `KILNCTL_TOUCH_CAL_SWAP_XY`) | Testable |
| `touch_test` | `ui_page_touch_test.c` | Raw touch test/tap-target dump | none | Testable via `touch_log_tap_targets`/`touch_inject` |
| `profiles` | `ui_page_profiles.c` | Profile library | `/profiles` | Testable |
| `profile_picker` | `ui_page_profile_picker.c` (+`_format`) | Choose profile to fire | `/profiles` | Testable |
| `profiles_builtin_list` | `ui_page_profiles_builtin_list.c` | Built-in profile list | `/profiles` builtin section | Testable |
| `profile_detail` | `ui_page_profile_detail.c` | View one profile | `/api/profile` | Testable |
| `profile_segments` | `ui_page_profile_segments.c` | Segment list | `/profiles` segment editor | Testable |
| `profile_builder_zones` | `ui_page_profile_builder_zones.c` | New profile: zone selection | `/profiles` new-profile flow | Testable |
| `profile_builder_segment` | `ui_page_profile_builder_segment.c` | New profile: segment entry | same | Testable |
| `profile_builder_review` | `ui_page_profile_builder_review.c` | New profile: review/save | same, `POST /api/profile` | Testable |

Note: no LCD page exists for live-profile editing, credentials/security,
backup/restore, OTA, kiln_configs presets, or the safety-commissioning
wizard -- those are web-only, hence "--" in the web tables above.

---

## API-only routes (no UI control anywhere -- web, LCD, or otherwise)

These are reachable only via direct HTTP or the MCP facade; no button/page
submits to them directly (some are read by JS polling loops rather than a
click, which is noted).

| Route | Tier | MCP tool | Notes |
|---|---|---|---|
| `GET /api/profile_exec` | OPEN | `profiles_get_exec_status` | Polled by dashboard JS, not a button |
| `GET /api/profile_plan` | OPEN | none direct | Feeds the feasibility popup, no direct control |
| `GET /api/board_temps` | OPEN | none direct (`thermo_read` is the live-board equivalent) | |
| `POST /api/unit_pref` | ADMIN | none direct | Submitted by the main-page unit toggle, not a dedicated Save button |
| `GET /api/dualwrite_window` | ADMIN | none direct | Read by diagnostics page JS |
| `GET /api/cfgfs/format_pending` | ADMIN | none direct | Polled before the format-confirm button enables |
| `GET /api/ota/challenge` | OPEN | `ota_get_challenge` | Fetched automatically before an OTA POST |
| `GET /api/logs/firing`, `GET /api/logs/autotune` | ADMIN | `fetch_event_log`/`get_device_log`/`get_device_log_json` (adjacent, not identical) | No page renders these directly today |
| `POST /api/safety/log_level` | ADMIN | `safety_set_log_level` | No web control; CLAUDE.md notes this had no caller anywhere as of 2026-09-19 |
| `GET /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled` | Paired with the diagnostics toggle's POST, itself a button |

---

## Gaps and caveats found while building this matrix

- Several ADMIN POST routes (profile import/hide/restore/favorite, kiln_configs
  save/clone/rename/delete/import, backup import/export, security saves,
  danger-mode relay control, cfgfs file read/write) have **no direct MCP
  facade tool** -- exercising them from an agent means raw HTTP, not
  `kiln_call`. This matches `feedback_prioritize_mcp_improvements`: closing
  these gaps is tooling work, not a one-off workaround, if repeated testing
  of these paths turns out to be common.
- `route_tier_table.h` is the single authoritative tier source (see its own
  header comment on the "reset one side of a pair" risk of a second, hand-
  maintained tier table) -- this document's tier column is copied from it
  verbatim, not re-derived.
- The `/status` GET route legitimately serves two roles (Wi-Fi status page AND
  a page-shell OPEN route) under one `ROUTE_TIER_OPEN` entry, per the
  2026-09-17 audit finding 7 fixed in that file -- do not read that as a
  missing row.
- Live pixel judgment (LCD color/state) must go through
  `sample_lcd_region.ps1`'s numeric RGB sampling against a bezel reference,
  never eyeballing a screenshot -- `project_st7796_rgb565_byte_order` records
  a prior incident where this was gotten wrong (byte order, not inversion).
