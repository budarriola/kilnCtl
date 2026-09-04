# Completed work — September 2026

Detailed closeout writeups relocated out of `ROADMAP.md` per its own
maintenance rule ("A finished item leaves this plan"). ROADMAP.md keeps
one-line closed entries with links back here; this file holds the full
file maps, audit methods, and negative-test descriptions verbatim as
they were originally written.

## M15 architecture hardening findings full detail

Moved from `ROADMAP.md` M15 on 2026-09-04. All items below carry that
date unless the item text itself says otherwise. See `ROADMAP.md`'s M15
section for the current one-line status of each.

## M15 — Architecture hardening · *opened 2026-09-04*

Findings from a four-agent architecture review, coordinator spot-verified.
Owner: unassigned. Everything below is an open suggestion, nothing is done.

- [x] **`SX1509.h` is public by accident.** `App/drivers/CMakeLists.txt:316`'s
      `INCLUDE_DIRS "."` exposes it to all 8 `uart_bridge*.c` files instead of
      just the owner module. Split the header, move the rest to
      `PRIV_INCLUDE_DIRS`. M — **CLOSED 2026-09-04**: `PRIV_INCLUDE_DIRS`
      can't fence sibling `.c` files in a flat single-component directory
      (quote-`#include` always searches the including file's own directory
      first), so the write/config API moved to `SX1509_internal.h`, gated by
      a `#error` unless the including file `#define`s `SX1509_OWNER_BUILD`
      first — only `kiln_io.c`/`kiln_io_owner.c`/`SX1509.c`/`main.c`'s bring-up
      do. `SX1509.h` now carries only the struct, constants, and read/status
      calls. All 8 `uart_bridge*.c` files turned out to need none of it
      directly (their `#include "SX1509.h"` was dead — verified by grepping
      each for `SX1509_` symbol use) and had it removed; `uart_bridge_io.c`
      keeps `kiln_io.h` for `SX1509_PIN_COUNT`/`SX1509_SENSE_FOR_PIN`.
      Negative-tested: added `#include "SX1509_internal.h"` to
      `uart_bridge_thermo.c` and confirmed `build_kilnfw` fails on the
      `#error`, then reverted. Runtime behavior unchanged — declarations
      moved, nothing rewritten. `build_kilnfw` and all 21/21 host test
      executables pass.
- [x] **`GET /api/status` allocates from internal DRAM.** `dashboard_http.c:582`
      uses plain `malloc` while sibling handlers in the same file use
      `heap_caps_malloc` SPIRAM; same gap in `backup_http.c:178`. This is the
      most-polled handler against the tightest heap. S — **CLOSED 2026-09-04**:
      both converted to `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
      (`dashboard_http.c:582`, `backup_http.c:178` and `:1679`); swept the rest
      of the HTTP handler files and found no other plain `malloc` of a
      response/scratch buffer.
- [ ] **12 files exceed the 1500-line rule.** `autotune_engine.c` (4120),
      ~~`wifi_prov.c` (2820)~~, ~~`dashboard_http.c` (2572)~~, ~~`ota_http.c` (2508)~~,
      ~~`ui_page_home.c` (2279)~~, ~~`panel_spi.c` (2174)~~, ~~`profiles_http.c` (2097)~~,
      ~~`zones_config_json.c` (1867)~~, `main.c` (1820), ~~`backup_http.c` (1756)~~,
      ~~`uart_bridge_ext.c` (1727)~~, ~~`zones_http_handlers.c` (1598)~~. Split
      `wifi_prov.c` and `autotune_engine.c` first — riskiest per
      `ARCHITECTURE.md`, and autotune has four separable concerns, on the
      `profile_executor` 8-file split's precedent. M-L — **`wifi_prov.c` part
      CLOSED 2026-09-04**: move-only split into `wifi_prov.c` (559 lines,
      command-queue infra/bring-up/owner_task), `wifi_prov_nvs.c` (591,
      NVS load/save/migration), `wifi_prov_link.c` (802, driver config/event
      handlers/timers/DNS hijack) and `wifi_prov_api.c` (748, network/mode/
      AP/IP-mode command bodies + blocking scan), sharing state via
      `wifi_prov_internal.h` on the `profile_executor_internal.h` precedent
      (shared statics as `extern`, former `static` helpers widened to
      file-scope-internal). Symbol audit: grepped every widened symbol for
      exactly one non-static definition repo-wide before trusting the link;
      caught and fixed two real collisions the widening exposed
      (`nvs_partition_init`/`migrate_from_default_partition` clashing with
      `zones_config_store.c`'s own same-named statics-turned-would-be-globals
      once no longer `static` — renamed to `wifi_prov_nvs_partition_init`/
      `wifi_prov_migrate_from_default_partition`). `build_kilnfw` compiles
      and links all four wifi_prov files clean (the build's only remaining
      failure is `autotune_engine.c`'s own in-progress, unrelated split —
      left open per this item). All 21/21 host test executables pass,
      `test_wifi_prov.c` unchanged except #including the three new files
      alongside `wifi_prov.c`, same convention as
      `test_profile_executor_prestart.c`. `autotune_engine.c` part —
      **CLOSED 2026-09-04**: move-only split into `autotune_engine.c` (1462
      lines, task/lifecycle, the shared tick state machine, run setup,
      plain lifecycle/status API), `autotune_engine_guard.c` (515, shared
      relay-apply/escalate/abort plumbing plus the two persistence entry
      points `autotune_engine_abort()`/`accept()`), `autotune_engine_step_
      identify.c` (1067, STEP method settle/onset detection, the FOPDT fit
      and its guards, target-mode probe handling, pre-start thermal
      readiness), `autotune_engine_relay.c` (281, RELAY method bang-bang
      law/fit/`run_relay()`) and `autotune_engine_coupling.c` (74,
      coupling-matrix persistence job + getter + RGA), sharing state via
      `autotune_engine_internal.h` on the same precedent (shared statics as
      `extern`, former `static` helpers widened to file-scope-internal).
      Symbol audit: grepped every widened symbol for exactly one non-static
      definition repo-wide before trusting the link; caught and fixed real
      collisions the widening exposed the same way the wifi_prov split
      did — `TAG`, `apply_relay`, `ticks_to_s`/`ticks_to_ms`,
      `escalate_and_abort`, `unpack_zone_trace`, `finalize_fit` and
      `begin_run_locked` all clashed with same-named statics-turned-would-be-
      globals elsewhere in the tree (`profile_executor*.c`, `zones_http.c`,
      `safety_link.c`, `pid_autotune.c`, every other file's own `static
      const char *TAG`) — renamed to `AT_TAG`, `autotune_apply_relay`,
      `at_ticks_to_s`/`at_ticks_to_ms`, `autotune_escalate_and_abort`,
      `autotune_unpack_zone_trace`, `autotune_finalize_fit`,
      `autotune_begin_run_locked`. `build_kilnfw` compiles and links clean
      (first attempt caught the collisions above via `ld`'s "multiple
      definition" — real bugs, not tooling noise). All 21/21 host test
      executables pass; `test_autotune_engine_prestart.c` unchanged except
      #including the four new files alongside `autotune_engine.c` and
      following the same renames at its own direct call sites, same
      convention as `test_wifi_prov.c`. `dashboard_http.c` part —
      **CLOSED 2026-09-04**: move-only split into `dashboard_http.c` (774
      lines, `s_dash` board-object storage, `DASH_TAG`, flash facts,
      `reset_reason_name()`, `dashboard_get_status()`,
      `dashboard_http_get_hw_ready()`, `dashboard_set_relay()`, and
      `dashboard_http_start()` which registers every handler defined in the
      other four files), `dashboard_status_http.c` (605, `json_f()` +
      `GET /api/status`), `dashboard_settings_http.c` (133, `POST
      /api/unit_pref` + `POST /api/safety/log_level`), `dashboard_exec_http.c`
      (778, the profile-executor family: `GET /api/profile_exec`,
      `/api/profile_plan`, `/api/control`, `/api/firing_history`,
      `/api/history.csv`; `POST /api/profile_exec/{start,stop,pause,resume,
      ack_last_run}`, `POST /api/safety/clear_trip`) and
      `dashboard_autotune_http.c` (386, `GET /api/autotune`,
      `/api/autotune/matrix`, `/api/autotune/trace.csv`; `POST
      /api/autotune/{start,abort,accept}`), sharing state via
      `dashboard_http_internal.h` on the same `profile_executor_internal.h`
      precedent (shared statics as `extern`, former `static` helpers
      widened to file-scope-internal). Symbol audit: grepped every widened
      symbol for exactly one non-static definition repo-wide before
      trusting the link; only one real collision found —
      `status_get_handler` (already `static` in `adaptive_tune_http.c` and
      `wifi_provision_http.c`) — renamed to `dashboard_status_get_handler`
      per the audit rule even though today's check came back silent (both
      others stay `static`, so no link-time clash exists yet, but the rule
      says rename regardless). `build_kilnfw` compiles and links clean.
      `dashboard_http.c` is never pulled into a host-test translation unit
      (its `lvgl_port.h` include drags in a GCC-only attribute MSVC's host
      toolchain rejects, per `build_host_tests.ps1`'s own comment), so no
      test file needed updating; all 21/21 host test executables still
      build and pass. `profiles_http.c` part — **CLOSED 2026-09-04**:
      move-only split into `profiles_http.c` (1151 lines, `s_profiles`
      storage, on-flash blob versioning/decode, NVS load/save/erase/
      migrate, `PROFILES_TAG`, the plain-C API
      `profiles_http_get/save/delete/get_bounds()` `profile_executor.c`/
      `uart_bridge_ext.c` call directly, and `profiles_http_start()` which
      registers every handler defined in the other two files),
      `profiles_catalog_http.c` (438, read side: `GET /profiles` page,
      `GET /api/profiles`, `GET /api/profile`, `GET /api/profiles/builtin`)
      and `profiles_edit_http.c` (526, write side: `POST /api/profile`,
      `POST /api/profile/delete`, `POST /api/profile/builtin/{hide,
      restore}`), sharing state via `profiles_http_internal.h` on the same
      `profile_executor_internal.h` precedent (shared statics as `extern`,
      former `static` helpers widened to file-scope-internal). Symbol
      audit: grepped every widened symbol (`s_profiles`, `nvs_save_slot`,
      `nvs_erase_slot`, `profile_exceeds_zone_ceiling`,
      `validate_io_segment`, `profiles_list_get_handler`,
      `profile_detail_get_handler`, `builtin_list_get_handler`,
      `profile_post_handler`, `profile_delete_post_handler`,
      `builtin_hide_post_handler`, `builtin_restore_post_handler`) for
      exactly one non-static definition and any same-named `static` one
      repo-wide before trusting the link — one real, already-live
      collision found: `page_get_handler` (bare widening would have hit
      `zones_http_handlers.c`'s own non-static `page_get_handler`, an
      immediate multiple-definition error, not a latent one) — renamed
      `profiles_page_get_handler`. `TAG` renamed `PROFILES_TAG` per the
      `DASH_TAG` precedent regardless of collision (every driver file in
      App/drivers has its own `static const char *TAG`). `build_kilnfw`
      compiles `profiles_http.c`/`profiles_catalog_http.c`/
      `profiles_edit_http.c` clean (confirmed by their absence from two
      consecutive `build_kilnfw` failure logs, both of which stopped only
      on `ota_http_esp.c`/`ota_http_pico.c` — `autotune_engine.c`'s sibling
      `ota_http.c` split, open under this same item, left alone per this
      task's own scope). All 21/21 host test executables pass;
      `test_profiles_http.c` unchanged except #including the two new files
      alongside `profiles_http.c`, same convention as `test_wifi_prov.c`/
      `test_autotune_engine_prestart.c`. `ota_http.c` part — **CLOSED
      2026-09-04**: move-only split into `ota_http.c` (1096 lines: HMAC/KDF
      helper, nonce+lockout state, challenge issue/verify
      `ota_http_verify_request()`, the update-claim mutex, the interlock
      snapshot/refusal glue, `ota_http_authenticate_request()`, the shared
      `ota_http_get_client_ip`/`ota_http_send_json_clamped`/`ota_http_set_
      fail_reason` helpers, and `ota_http_start()`), `ota_http_esp.c` (714,
      `POST /api/ota/esp` transfer/status/rollback), `ota_http_pico.c` (653,
      `POST /api/ota/pico` stage/relay/status/rollback plus the pico-
      rollback async state) and `ota_http_recovery.c` (226, boot-recovery
      exit and the unauthenticated `GET /api/ota/interlock`), sharing state
      via `ota_http_internal.h` on the `profile_executor_internal.h`/
      `wifi_prov_internal.h` precedent (shared statics as `extern`, former
      `static` helpers widened to file-scope-internal). Symbol audit:
      grepped every widened symbol both ways (a clash with a non-static
      definition elsewhere fails the link; a clash with a same-named
      `static` elsewhere links silently and breaks later) across all of
      `App/drivers/` before trusting the link — `TAG`, `s_safety`, `s_io`/
      `s_thermo_bus`-style names, `now_ms`, `get_client_ip`,
      `send_json_clamped`, `set_fail_reason` were the ones with real
      same-named `static`s elsewhere (`ota_pico_relay.c`'s own `now_ms()`
      and `set_fail_reason()` mention, `kiln_io_owner.c`'s own `s_io`/
      `s_safety`, dozens of files' own `static const char *TAG`) and were
      renamed `OTA_HTTP_TAG`/`ota_http_safety`/`ota_http_get_client_ip`/
      `ota_http_send_json_clamped`/`ota_http_set_fail_reason`; `now_ms` and
      `s_thermo_bus`/`s_io` turned out to be needed only inside
      `ota_http.c` itself once the seams were drawn, so they stayed
      `static`, unrenamed. The nine cross-file route handlers
      (`ota_esp_post_handler` etc.) were widened too, purely so
      `ota_http_start()`'s `httpd_uri_t` table can name them from the other
      files; audited the same way, zero collisions found (only two
      doc-comment mentions, in `ota_pico_relay.h`/`heat_interlock.h`, not
      symbols). `build_kilnfw` compiles and links all four files clean.
      All 21/21 host test executables pass; `test_ota_http.c` updated to
      `#include` the three new files alongside `ota_http.c` (same
      convention as `test_wifi_prov.c`) and to use the renamed
      `ota_http_safety`/`ota_http_pico_rollback_async` at its own direct
      call sites. `zones_config_json.c` part — **CLOSED 2026-09-04**:
      move-only split into `zones_config_convert.c` (633 lines,
      `convert_zone_v1()`..`convert_zone_v14()` per-historical-layout field
      converters, `zones_cfg_expected_len_for_version()`,
      `set_default_timing_profile()` — no public entry point of its own,
      called only from the next file), `zones_config_migrate.c` (589,
      the per-version dispatch switch `convert_versioned_blob_to_current()`,
      `zones_config_json_compute_crc()`, `raise_heater_timing_to_floors()`,
      and the public decode entry point `zones_config_json_decode_blob()`)
      and `zones_config_json.c` (674, kept: the settings_source chain-walk
      wrapper/normalizer, `zones_config_json_validate()`, and the HTTP-free
      field parsers), sharing state via `zones_config_json_internal.h` on
      the `profiles_http_internal.h` precedent (shared statics as `extern`,
      former `static` helpers widened to file-scope-internal). Symbol audit:
      grepped every widened symbol both ways (a clash with a non-static
      definition elsewhere fails the link; a clash with a same-named
      `static` elsewhere links silently and breaks later) across all of
      `App/drivers/` before trusting the link — `convert_zone_v1()`..
      `convert_zone_v14()` and `set_default_timing_profile()` came back
      clean (kept their names); `expected_len_for_version` already has its
      own `static` definition with the identical signature in
      `profiles_http.c` — not an immediate link error since both stayed
      `static` from each other's point of view, but exactly the latent-
      collision shape the `page_get_handler`/`nvs_partition_init` precedents
      warn about, so renamed on sight to `zones_cfg_expected_len_for_
      version()`. `TAG` renamed `ZONES_CFG_TAG` per the `DASH_TAG`/
      `PROFILES_TAG`/`OTA_HTTP_TAG` precedent regardless of collision (every
      driver file in `App/drivers` has its own `static const char *TAG`).
      `convert_versioned_blob_to_current()` and `raise_heater_timing_to_
      floors()` stayed `static` — each is called only from its own file's
      `zones_config_json_decode_blob()`, no widening needed.
      `build_kilnfw` compiles and links clean. All 21/21 host test
      executables pass; `test_zones_http.c`'s existing separate-executable
      build (it `#include`s `zones_http.c` directly) updated to compile
      `zones_config_convert.c`/`zones_config_migrate.c` in alongside
      `zones_config_json.c`, same convention as `test_profiles_http.c`
      picking up `profiles_catalog_http.c`/`profiles_edit_http.c`. The
      run's one failure (`test_st7796_panel.c`, in the `main` executable)
      is unrelated display-file territory, out of this item's scope.
      `backup_http.c` part — **CLOSED 2026-09-04**: move-only split into
      `backup_http.c` (96 lines, `BACKUP_TAG` + `backup_http_start()` route
      registration only), `backup_json.c` (198, the generic hand-rolled JSON
      reader — `backup_json_skip_ws`/`_value`, `_obj_find`, `_arr_first`/
      `_next`, `_field_num`/`_opt_num`/`_str`), `backup_export.c` (350, GET
      `/settings/backup` page + GET `/api/backup/export` streamed writer,
      `json_escape` kept `static`) and `backup_import.c` (1118,
      `backup_import_apply()`'s two-pass validate-then-commit parser + POST
      `/api/backup/import`'s upload handler), sharing state via
      `backup_http_internal.h` on the `ota_http_internal.h`/
      `zones_config_json_internal.h` precedent (shared statics as `extern`,
      former `static` helpers widened to file-scope-internal, every widened
      symbol renamed with a `backup_`/`BACKUP_` prefix). Symbol audit:
      grepped every widened symbol (`BACKUP_TAG`, `backup_page_get_handler`,
      `backup_export_get_handler`, `backup_import_post_handler`, and the
      eight `backup_json_*` reader functions) across all of `App/drivers/`
      for both a non-static definition and a same-named `static` — none
      found, all clean under their new prefix. One deliberate
      **non-widening**: `json_escape` was left `static` in `backup_export.c`
      rather than promoted, because `dashboard_json.c` already defines a
      non-static global `json_escape()` — widening this file's copy would
      have been an immediate link error (the exact collision class this
      audit exists to catch), and since the import side never calls it,
      keeping it file-local costs nothing. First pass missed that
      `backup_import_post_handler()` still carried its original `static`
      keyword after the move, which failed the ESP-IDF build with "static
      declaration follows non-static declaration" against its
      `backup_http_internal.h` prototype; fixed by dropping the leftover
      `static`. `build_kilnfw` compiles and links clean. All 21/21 host
      test executables pass; `test_backup_import.c` (the file this split's
      brief called out to keep green) updated to `#include` all four split
      files instead of the one original, same convention as
      `test_profiles_http.c`/`test_zones_http.c` above. `uart_bridge_ext.c`
      part — **CLOSED 2026-09-04**: move-only split into `uart_bridge_ext.c`
      (539 lines, file banner, `uart_bridge_ext_retry_task_create_pinned()`,
      the flash-safe executor — `bx_worker_task`, `uart_bridge_ext_worker_
      ensure_started()`, `uart_bridge_ext_start_flash_worker()`,
      `bx_run_on_internal_stack()`, `uart_bridge_ext_run_on_flash_worker()`,
      `uart_bridge_ext_is_on_flash_worker()` — and the shared little-endian/
      reply-framing helpers), `uart_bridge_ext_control.c` (600, CONTROL task 8
      + PROFILES task 9), `uart_bridge_ext_autotune.c` (246, AUTOTUNE task 10)
      and `uart_bridge_ext_wifi.c` (410, WIFI task 11 — the one task family
      that never touches the flash-safe worker), sharing state via
      `uart_bridge_ext_internal.h` on the `ota_http_internal.h` precedent
      (shared statics as `extern`, former `static` helpers widened to
      file-scope-internal, widened symbols renamed with a `uart_bridge_ext_`
      prefix). `uart_bridge_ext_start_flash_worker()`/`_run_on_flash_worker()`/
      `_is_on_flash_worker()` were already public via `uart_bridge.h` and
      needed no further widening. Symbol audit: grepped every widened symbol
      (`TAG`, `retry_task_create_pinned`, `bx_worker_ensure_started`,
      `bx_put_u16_le`/`bx_u32_le`/`bx_put_u32_le`/`bx_f32_le`/`bx_put_f32_le`,
      `bx_args_ok`, `bx_put_lstring`, `bx_reply`, `bx_reply_ok_err`) across all
      of `App/drivers/` for both a non-static definition and a same-named
      `static` — `TAG` collides with every other file's own `static const
      char *TAG` as expected; `bx_put_lstring`/`bx_reply_ok_err`/
      `retry_task_create_pinned` turned up only in *comments* in
      `uart_bridge.c`/`uart_bridge_internal.h`/`gpio_probe.c` referencing this
      file's functions by name, not real definitions — renamed to the
      `uart_bridge_ext_` prefix anyway per the audit rule (rename even when
      currently clean). `control_task`/`profiles_task`/`autotune_task`'s calls
      to the file-local `bx_run_on_internal_stack()` were changed to go
      through the existing public `uart_bridge_ext_run_on_flash_worker()`
      wrapper instead (identical signature and behavior), avoiding the need to
      widen that one too. `build_kilnfw` compiles and links clean. All 21/21
      host test executables pass; `uart_bridge_ext.c` itself is pulled into no
      host-test translation unit (see its own S4 comment — its includes reach
      too much hardware-driving surface for a host stub set), so no test file
      needed updating. `zones_http_handlers.c` part — **CLOSED 2026-09-04**:
      move-only split into `zones_http_get.c` (402 lines, `page_get_handler`/
      `zones_json_escape`/`safety_config_page_get_handler`/
      `zones_get_handler` — the two static pages plus GET /api/zones),
      `zones_http_post_parse.c` (692, the per-zone POST field parser, was
      `static bool parse_zone_fields()`), `zones_http_post.c` (403,
      `zones_post_handler()`, POST /api/zones's whole-page-submit body) and
      `zones_http_pid.c` (147, `zones_pid_post_handler()`, the narrow
      PID-only POST /api/zones/pid endpoint that stays legal while a firing
      is running), sharing state via the existing `zones_http_internal.h`
      (already the shared seam header for this file's siblings since the
      2026-09-01 `zones_http.c` split — reused rather than duplicated, per
      this task's own instruction). Symbol audit: grepped for both a
      non-static definition and a same-named `static` across all of
      `App/drivers/` before trusting the link — only one symbol needed
      widening out of `static`, `parse_zone_fields()` (every other function
      moved was already non-static from the 2026-09-01 split and stayed
      that way); the audit found zero collisions of any kind for that name
      repo-wide, but it was renamed to `zones_http_parse_zone_fields()`
      anyway per the "rename even when currently clean" rule this pass's
      brief restates. `build_kilnfw` compiles and links clean. All 21/21
      host test executables pass; `test_zones_http.c` (its own separate
      executable, `#include`s `zones_http.c`'s split family directly)
      updated to `#include` the four new files instead of the one original
      and to call `zones_http_parse_zone_fields()` at its ~43 direct call
      sites, same convention as `test_profiles_http.c`/
      `test_backup_import.c` above. `ui_page_home.c` part — **CLOSED
      2026-09-04**: move-only split into `ui_page_home.c` (1024 lines, the
      page's header-comment design history, every shared `static` widget/
      state variable, `ui_home_format_duration()`/`ui_home_freezing_point_
      disp()` and `ui_page_home_build()`, the public entry point),
      `ui_page_home_actions.c` (297, Start/Stop/Pause action handlers, the
      confirm dialogs, `ui_home_menu_nav_cb()`, `ui_home_fire_btn_cb()`/
      `ui_home_pause_resume_btn_cb()` and the `ui_home_build_button()`
      helper), `ui_page_home_chart.c` (310, `ui_home_plan_lookup()`, the
      dashed-planned-line/Y-tick-mark LVGL draw-event hooks, and the Y/X-
      tick-label and legend layout helpers) and `ui_page_home_refresh.c`
      (649, `ui_home_refresh_cb()`, the 1 Hz timer callback that repaints
      every live number on the page), sharing state via a new
      `ui_page_home_internal.h` on the same precedent (shared statics as
      `extern`, former `static` helpers widened to file-scope-internal);
      the sibling `ui_page_home_graph.c`/`.h` (pure host-tested chart-axis/
      lag-notice logic, already its own file since before this pass) was
      checked and needed no change. Symbol audit: grepped every widened
      symbol — ~30 statics and ~14 functions, all prefixed `ui_home_`/
      `s_ui_home_` regardless of outcome per the "rename even when
      currently clean" rule — for both a non-static definition and a
      same-named `static` across all of `App/drivers/`; four real
      collisions found against other files' own same-named `static`s
      (`s_zone_count` in `ui_page_temperature.c`, `s_progress_label` in
      `ui_page_touch_cal.c`, `s_topbar` in `ui_page_config.c`/
      `ui_page_diagnostics.c`, `s_status_label` in `ui_page_network.c`) —
      all renamed along with everything else. `TAG` renamed `UI_HOME_TAG`
      per the `DASH_TAG`/`PROFILES_TAG`/`OTA_HTTP_TAG`/`ZONES_CFG_TAG`/
      `ZONES_HTTP_TAG`/`UART_BRIDGE_EXT_TAG` precedent. `flash_worker_
      lint.py` stayed clean before and after (this file family makes no
      NVS/flash-worker calls, so the FILENAME-keyed allowlist was never in
      play here). `build_kilnfw` compiles and links clean end to end (a
      concurrent, unrelated `screen_idle.c` edit from another in-progress
      session briefly blocked the same build; reconfiguring picked up the
      new source list and the next `build_kilnfw` run succeeded once that
      file's own edit landed). `ui_page_home.c`/`ui_page_home_graph.c` are
      the only members of this family pulled into a host-test translation
      unit (`test_ui_page_home_graph.c`, unchanged — it only ever included
      `ui_page_home_graph.c`, never `ui_page_home.c` itself), so no test
      file needed updating; 20/21 host test executables build and pass
      (the one non-build, `main`, fails on a concurrent, unrelated session's
      new `test_display_power_wiring.c`, an untracked file this pass never
      touched). `panel_spi.c` part — **CLOSED 2026-09-04**: move-only split
      into `panel_spi.c` (536 lines, bus/device setup, the D/C-batched/
      chunked-SPI transport primitives, the bounds/blit-state guards, the
      5x7 font and vendor init-byte tables, and — moved here rather than
      with the rest of bring-up, since a static initializer needs its
      table's actual values in the same translation unit — the MADCTL-by-
      rotation table and the `panel_desc_t` descriptor/`ILI9488_get_panel_
      desc()`), `panel_spi_bringup.c` (663, init-table execution via
      `panel_codec_init_step()`, rotation/MADCTL application, hard/soft
      reset, and `ILI9488_init/start/deinit/reset`/`set_power`/
      `set_rotation`/`set_invert`/`get_dimensions`), `panel_spi_draw.c`
      (442, rect/pixel/line drawing and the text renderer) and
      `panel_spi_blit.c` (589, the streaming-blit protocol, sync + async
      flush, and RDDID read-back), sharing state via a new `panel_spi_
      internal.h` on the `zones_http_internal.h`/`profile_executor_
      internal.h` precedent (shared statics as `extern`, former `static`
      helpers widened to file-scope-internal — command opcodes/COLMOD/
      MADCTL-bit/reset-timing `#define`s also moved there since three of
      the four files need them, though a macro has no linkage to audit).
      Symbol audit: grepped every widened symbol (`TAG`→`PANEL_SPI_TAG`,
      `panel_spi_ready`, `panel_spi_chunk`, `panel_spi_chunk_pixels`,
      `panel_spi_lock`, `panel_spi_unlock`, `panel_spi_set_dc`,
      `panel_spi_tx`, `panel_spi_write_cmd`, `panel_spi_begin_ram_write`,
      `panel_spi_push_color_run`, `panel_spi_rect_in_bounds`, `panel_spi_
      blit_clear_state`, `panel_spi_reject_if_blitting`, `panel_spi_
      font5x7`) for both a non-static definition and a same-named `static`
      across all of `App/drivers/` before trusting the link — the audit
      came back clean for all fifteen (no prior use of any `panel_spi_`
      name anywhere in the tree), so every one was still renamed with the
      `panel_spi_` prefix per the "rename even when currently clean" rule.
      Two symbols initially widened were walked back instead: `ili9488_
      init_bytes[]`/`ili9488_madctl_by_rotation[]` looked like they needed
      `extern` for the descriptor in another file, but an extern array's
      element values (unlike its `sizeof`) are not a compile-time constant,
      so a static-initializer descriptor built from them cannot live in a
      different translation unit than the tables themselves — both tables
      and the descriptor were kept together in `panel_spi.c` instead, and
      both stayed `static` (caught by two real `build_kilnfw` failures,
      "initializer element is not constant", not by the symbol-collision
      audit). The one `static inline` (`panel_spi_encode_pixel()`, the
      RGB565->wire encoder) moved into the shared header verbatim: each
      including file gets its own internal-linkage copy, so it was never a
      linkage-widening question. Byte order, MADCTL color order (0x00,
      f493bf8), INVON's deliberate absence and the 20 MHz write clock
      (07cad60) are all unchanged — moved, not touched; the async-flush/
      zero-copy-flush KNOWN BROKEN branches moved into `panel_spi_blit.c`
      with their warning comments intact. `flash_worker_lint.py` stayed
      clean before and after (180 driver files scanned, 22 allowlisted --
      no new filename needed adding). `build_kilnfw` compiles and links
      clean (a stale CMake cache from before this pass's `CMakeLists.txt`
      edit needed one explicit `build_kilnfw(target="reconfigure")` to pick
      up the three new source files; plain rebuilds before that reconfigure
      failed with undefined references to every symbol this split moved,
      which is what surfaced the missing reconfigure rather than a real
      code defect). All 21/21 host test executables pass; `panel_spi.c` and
      its new siblings are never pulled into a host-test translation unit
      (`test_st7796_panel.c`/`test_panel_codec.c` exercise `st7796_panel.c`/
      `panel_codec.c` directly, not this driver), so no test file needed
      updating. Still open under this item: `autotune_engine.c` and
      `ota_http.c`'s own remaining pieces (each already has an in-progress
      split noted above) and `main.c` (1820).
- [x] **Stand-in stubs sit above the polarity/decode layer.**
      `SaftyFW/src/tasks/discrete_task.c:91-97` documents the shipped E-stop
      polarity bug that 378/378 host checks could not see because
      `virtual_dut` stands in above translation, not below it. Audit the
      remaining stand-ins (current sense, TC SPI) and push stubs below the
      decode layer. S to audit, M per stub — **AUDIT PART CLOSED 2026-09-04**,
      full table in `SaftyFW/docs/GUARD_TEST_MATRIX.md` §10: no `virtual_dut`
      module exists (that name is inherited from the deleted `SimFW`/
      `kilnsim`); the pattern was audited against every `safety_guard_input_t`
      field instead. Current sense (`current_presence_policy.c`,
      `ct_amps_cal.c`) and every already-existing decode/policy layer
      (`discrete_pin_policy.c`, `max31856_*_policy.c`, `snapshots.c`, the
      `kilnlink_*_decode()` codecs) were already relocated below the stub in
      prior sessions and are host-tested directly. **One new relocation this
      pass:** `safety_core_build_input()`'s `.relay_deenergized =
      !relay_owner_is_energized()` line (S9) had a bare, single `!` with zero
      coverage — the same shape as the shipped S7 bug — closed with a new
      source-text-scan test, `test/test_safety_core_polarity_wiring.c`
      (same technique `test_safety_core_s8_wiring.c` uses, since
      `safety_core.c` itself is not host-compilable), which also pins
      `.estop_pressed`/`.main_fault_asserted`'s required non-negation.
      Negative-tested: each of the three lines' negation was flipped one at a
      time and the matching check failed (2067/2068), then restored (2068/2068).
      **Documented, not touched:** the TC SPI raw-decode layer inside
      `max31856.c` — another agent is extracting its fault-pin polarity
      concurrently, so left alone per instruction; the
      `current_sensing_disabled` forcing gap was already recorded honestly in
      GUARD_TEST_MATRIX.md §9 before this pass. `test/test_safety_guards.c`
      itself (the by-design injection seam for the whole
      `safety_guard_input_t` contract) is owned by another concurrent session
      and was not touched.
- [x] **`safety_link.h` hand-mirrors CommonFW frame constants.** POWER/DIAG/
      UPDATE_STATUS flag blocks (`safety_link.h:243-249,257-280,632-654`, 9
      "mirrored here" comments) duplicate `kilnlink_power.h`/`kilnlink_diag.h`
      by hand because KilnFW cannot `#include` SaftyFW's headers. Either call
      the CommonFW codecs directly or add a CI diff against the source-of-
      truth headers. M — **CLOSED 2026-09-04**: calling the codecs directly
      was judged not low-risk here — `kilnlink_power.c`/`kilnlink_diag.c`
      aren't compiled into KilnFW's ESP-IDF component today
      (`components/kilnlink/CMakeLists.txt`), so wiring them in means both a
      build-system change and rewriting `safety_apply_power()`/
      `safety_apply_diag()` in `safety_link_frames.c` — safety-adjacent frame
      parsing — for a payoff this static check already covers. Added
      `App/test/power_diag_flag_mirror_drift_check.py` (+
      `check_power_diag_flag_mirror_drift.ps1`, picked up by
      `run_all_checks.ps1`'s glob, same pattern as
      `frame_a_offset_drift_check.py`): extracts the 18 mirrored POWER/DIAG
      constants (frame lengths, channel count, flag/boot/state bytes, the
      context-age sentinel) from both sides and fails naming the exact
      constant and both values on any diff; fails closed (treated as a
      failure, not a skip) if a regex stops matching either side. UPDATE_STATUS
      is unchanged — SaftyFW's `update_task.c` is its own source of truth with
      no CommonFW header to diff against (see that block's own comment).
      Negative-tested: perturbed `SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED`
      from `0x02u` to `0x03u`, confirmed the check fails naming that exact
      constant and both values, reverted, confirmed clean.
- [x] **No shared bounded-wait/unknown-outcome helper.** The discipline behind
      `safety_link_rollback_boot_id_changed()`'s rollback path (`safety_link.c`
      ~262, "a rollback that fully succeeded into a permanent
      UNKNOWN_TIMEOUT") exists only there. Extract a shared await-reply-or-
      unknown helper for the new-ESP/old-Pico skew case generally. S-M —
      **CLOSED 2026-09-04**: `safety_link_await_or_unknown()` (declared
      `safety_link.h`, cross-TU decl `safety_link_internal.h`, defined
      `safety_link.c`) is a generic "poll at an interval until a callback
      reports ACKED/UNKNOWN or the timeout elapses" helper, documented
      against `LINK_PROTOCOL.md`'s "a timeout must never be misreported as
      success" skew rule, with a doc comment requiring future Pico-bound
      commands to route through it rather than hand-roll a poll loop.
      `safety_link_send_rollback_ex()`'s boot_id-reconnect watch
      (`safety_link_commands.c`) ported onto it: the old inline `for(;;)`
      loop body became `rollback_boot_watch_poll()`, a
      `safety_link_await_poll_fn`, with a `rollback_boot_watch_ctx_t`
      closure carrying what used to be captured locals — same per-iteration
      decisions, same log lines, zero behavior change. `build_kilnfw` and
      21/21 host test executables pass (one pre-existing, unrelated
      `test_safety_link` failure noted below, present before this change and
      untouched by it).
- [x] **`KILNLINK_MIN_COMPATIBLE` is prose-argued per bump.**
      `kilnlink_version.h:20-24` documents this is a human judgement call, not
      a hash or a check. Add a synthetic-old-peer host test that asserts
      dispatch-table coverage per historical version. M — **CLOSED
      2026-09-04**: `test_safety_link_compile.c` gained a table (one row per
      Pico->ESP frame, cited against `kilnlink_version.h`'s own per-bump
      history comments) driving `safety_drain_inbox_ex()`'s real dispatch
      switch (`safety_link_inbox.c`) for every protocol version from
      `KILNLINK_MIN_COMPATIBLE` (7) to `KILNLINK_PROTOCOL_VERSION` (10): a
      frame that falls through to the switch's `default:` now fails the
      build instead of reading as a silently dropped/dead link. Reverse
      direction also asserted (a frame gated above a version, e.g.
      `ROLLBACK_RESULT` at protocol 9, is not expected from an older-but-
      still-compatible peer). KilnFW side only — SaftyFW's own dispatch
      (`link_task.c`) needs real RP2040/pico-sdk headers this tree has no
      off-target harness for; documented as out of reach in the test file's
      own header comment. Negative-tested: commenting out the
      `KILNLINK_ROLLBACK_RESULT_CMD` case failed the new test naming the
      exact frame and both affected protocol versions (9 and 10), then
      passed again once restored. `build_kilnfw` and all 21 host test
      executables green.
- [x] **Duty composition has no single breakdown struct.** Four stages —
      `profile_executor_feedforward.c:291-515`,
      `profile_executor_pid_tick.c` PID clamp then load-cap boost
      (~78-125, ~139-174, boost applied AFTER the clamp so duty can exceed
      1.0 invisibly to `pid_terms_t`, `pid.h:144-149`), and `heater_output.c`
      quantization — with nothing recording the breakdown. Add a
      `zone_duty_breakdown_t` populated through the pipeline and exposed on
      `/api/control`. M — **CLOSED 2026-09-04**: `zone_duty_breakdown_t`
      (`profile_executor.h`) added with one field per real transform
      (ff_hold/ff_climb/coupling_correction, pre/post-taper rate, effective
      kp/ki/kd, post_clamp_total, load_cap_boost, final_commanded — p/i/d/ff
      and pre_clamp_total deliberately not duplicated, already on
      `pid_terms_t`/derivable from it). Populated read-only in
      `profile_executor_feedforward.c` (`zone_feedforward()`) and
      `profile_executor_pid_tick.c` (`pid_family_zone_tick()`) — no control
      math changed. Wired onto `/api/control` in `dashboard_json.c`
      (`append_zone_status_json()`, not `dashboard_http.c` — the actual
      per-zone JSON now lives in the split-out file); `DASHBOARD_JSON_
      CONTROL_BUF_SIZE` raised 448->900/zone with the byte math in
      `dashboard_json.h`. Host tests: `test_dashboard_json.c`'s worst-case
      render extended, plus a new internal-consistency test in
      `test_profile_executor_prestart.c` with a negative-test mutation
      proving the check has teeth. 21/21 host test executables and
      `build_kilnfw` pass.
- [x] **Mode-state sprawl.** >=5 independent enums/booleans describe system
      mode; the dwelling/ramp-lock caveat is re-derived identically at
      `profile_executor_feedforward.c:243-244` and `:566-576`. Document a
      legal-state table or add a runtime assertion — not a forced single enum.
      M — **CLOSED 2026-09-04**: legal/illegal-state table documented as a
      comment block in `profile_executor_internal.h` (6 illegal rules, 3
      called-out legal-but-tricky rows: ramp-lock stall without dwelling,
      PAUSED mid-dwell, autotune SETTLING with no_setpoint). `exec_mode_
      state_check()` (`profile_executor.c`) asserts the illegal rows every
      control tick under the existing lock; host build has no NDEBUG
      convention to plug an on-target assert into (verified by grep — only
      compile-time `_Static_assert` exists elsewhere in `App/drivers`), so
      the call site uses plain `assert()`, live on host, log-only-if-Kconfig-
      disables-it on target — documented as such rather than invented.
      9 new host tests in `test_profile_executor_prestart.c` (5 illegal
      combinations incl. the 3 required, 4 legal-but-tricky including the
      two named in the ROADMAP item). Negative-tested: disabled rule 4's
      check, confirmed 2 of its tests failed, restored — 21/21 host test
      executables and `build_kilnfw` pass.
- [x] **No lint against flash/NVS writes outside the flash worker.** Direct
      writes bypassing `kiln_cfg_store.c`'s worker dispatch (`nvs_set_blob` at
      `kiln_cfg_store.c:356`, `kiln_cfg_store_apply()` at `:681`) have panicked
      hardware 3x and host tests cannot see the hazard (no lock in the stub).
      Add a grep-based CI lint to the host-test script. S — **CLOSED
      2026-09-04**: `App/test/flash_worker_lint.py` greps `drivers/*.c` for
      `nvs_set_*`/`nvs_commit`/`esp_partition_write`/`esp_partition_erase_range`
      outside a 19-file allowlist (each entry justified inline against one of
      three sanctioned patterns: worker dispatch, local
      `caller_stack_is_external()` guard, or init-time-only from `app_main`
      before the scheduler starts). `check_flash_worker_lint.ps1` wires it
      into `tools/run_all_checks.ps1`'s `check_*.ps1` discovery glob.
      Negative-tested: added a bare `nvs_set_u8()` call to `MAX31856.c` (not
      allowlisted), lint failed naming `drivers\MAX31856.c:1374`; reverted,
      confirmed clean again.
- [x] **`zones_http_client.py` hand-types its field table instead of reading
      it live.** `zones_http_client.py:320-350`'s `_TOP_FIELD_FORM_KEY` /
      `_TOP_INT_FIELDS` maps drift from firmware JSON keys by hand;
      `safety_cfg_http_client.py:136-213`'s `params_by_name()` +
      `build_post_body()` already use a live-GET lookup instead. Port zones to
      that pattern, or extend `selfcheck.py` (already parses
      `UART_PROTOCOL_VERSION` from firmware headers, `selfcheck.py:74-89`) to
      diff the dict against `zones_http_handlers.c` literals. S-M — **CLOSED
      2026-09-04**: `selfcheck_zones_fields.py` extracts both `zones_get_handler`'s
      top-level JSON keys (walks the concatenated `APPEND()` string-literal
      template with a brace/bracket-depth tracker, so a `"key":` only counts
      at depth 1 -- nested `safety_wiring`/per-zone/per-profile keys are
      excluded without hand-listing them) and `zones_post_handler`'s literal
      top-level POST field names, and diffs both against
      `_TOP_FIELD_FORM_KEY`/`_TOP_READONLY_OR_STRUCTURAL_KEYS`. Wired into
      `selfcheck.py`'s `main()`. Negative-tested: removed
      `safety_tc_type` from `_TOP_FIELD_FORM_KEY`, both new checks failed
      naming it, reverted, confirmed clean again.
- [x] **No stub-vs-real-IDF signature check.** `App/test/stubs/*.h` can drift
      from the real ESP-IDF headers they stand in for with nothing catching
      it. Add a signature-diff script. M — **CLOSED 2026-09-04**:
      `App/test/stub_signature_drift_check.py` extracts name+arity for every
      stub prototype, matches each stub header to its real counterpart under
      the configured IDF root (`--idf-root`, else `$IDF_PATH`, else
      `build/project_description.json`'s `idf_path`, else common install
      paths — SKIPS gracefully, exit 0, when none resolve), and reports any
      arity mismatch as stub-file:line. 2026-09-04 audit: 26/26 stub headers
      matched, tree clean, so `check_stub_signature_drift.ps1` (wired into
      `tools/run_all_checks.ps1` the same way) passes `--fatal-on-clean`.
      Negative-tested: added a bogus second argument to `stubs/driver/ledc.h`'s
      `ledc_timer_config()`, check failed naming
      `stubs/driver/ledc.h:43`; reverted, confirmed clean again.
- [x] **Campaign runner has no board-config restore on abnormal exit.**
      `run_queue.py` has atomic state and resume, but the `finally` path
      (~1203-1213) only closes the capture file and removes a stray empty
      log — it never re-applies a safe preset via `_apply_preset_http_only`
      (~836). Add a restore-on-exit hook plus a verify-arms-differ preflight
      as built-ins. M — **CLOSED 2026-09-04**: `run_queue()`'s entry loop is
      wrapped in `try/except BaseException` — `_handle_abnormal_exit` skips
      restore if the board was never touched, re-applies the campaign's
      baseline preset (last entry's, or `--baseline-preset`) once
      `_board_is_idle()` confirms `profile_exec` is not running/paused
      (never races an active firing), and records
      attempted/succeeded/error into the state file either way; a failed
      restore logs an unmissable banner naming the preset and fields to
      check by hand. `_check_arms_differ`, wired into `_preflight_campaign`
      before any HTTP probe, compares every pair of distinct local preset
      payloads (excluding `name`) and refuses naming the identical pair.
      `tools/PcTools/tests/test_run_queue_restore_and_arms.py` (10 tests).
      Negative-tested both: disabling `_check_arms_differ`'s call site
      failed the identical-arms test; disabling the `except` block's
      restore call failed 4/6 restore tests; both reverted, full
      `test_run_queue*.py` suite (119 passed, 2 skipped) confirmed clean
      again.
- [x] **Four hand-rolled JSONL parse loops disagree on malformed-line
      handling.** `coupling_pair_log.py:187-192,239-245`,
      `http_capture_log.py:67-72`, `link_hub.py:197-202,588-595`,
      `relay_ku_tu_check.py:97-101`. Factor a shared `iter_jsonl` helper. S —
      **CLOSED 2026-09-04**: `kilnctrl/jsonl_util.py`'s `iter_jsonl(source,
      on_error=..., with_line=...)` accepts either a path or an already-open
      line iterable (e.g. `link_hub.py`'s socket `makefile()`), and preserves
      each caller's prior malformed-line behaviour explicitly per call site:
      `on_error="skip"` (coupling_pair_log/http_capture_log/link_hub's second
      loop), `on_error="raise"` (relay_ku_tu_check, which never caught
      `json.loads()` before), `on_error=<callable>` (link_hub's first loop,
      which logs before skipping). `with_line=True` covers
      `load_thermo_samples_any_format`'s two-format fallback, which needs the
      raw line when the primary `{"t","s"}` parse fails. All five call sites
      ported, no behaviour change. Negative-tested: made the "raise" path a
      no-op, `test_jsonl_util.py`'s raise test failed as expected; reverted.
- [x] **`tuning_campaign.py`'s `make_plant` generates continuous floats.**
      (~126-268) No MAX31856 0.0078125 C quantization, unlike the real
      sensor path. Add a quantize pass, or document explicitly why continuous
      is intentional. S — **CLOSED 2026-09-04**: `run_step_test`/
      `run_relay_test` gained a `_quantize()` helper (rounds to
      `plant_sim.MAX31856_QUANTUM_C`, 0.0078125 C = 1/128 C) applied to every
      measured-temperature output, on by default via a new `quantize=True`
      parameter (`quantize=False` restores the old continuous behaviour).
      Negative-tested: made `_quantize()` a no-op, `test_tuning_campaign_
      quantize.py` failed (3 of 6 tests), reverted, confirmed clean again.
- [x] **Vendored `mcpkit_registry.py` has no drift guard.** Currently
      byte-identical to `tools/PcTools/src/mcpkit/registry.py`, the source of
      truth it's vendored from — add a one-line diff check to `selfcheck.py`
      so it stays that way. S — **CLOSED 2026-09-04**: `pytest`-side coverage
      already existed (`tests/test_mcpkit_vendored_copy.py`), but nothing
      covered it in `selfcheck.py`, which runs in contexts pytest doesn't
      (per this item's own request). Added `_mcpkit_vendored_copy_check()` --
      same byte-identical (line-ending normalized) comparison, skips cleanly
      when the `mykicadMcp` submodule isn't checked out.
- [x] **Frame A's field layout is hand-duplicated across firmwares.**
      `SaftyFW/src/tasks/link_frame.h:1-16` (pack side) says it is
      "byte-for-byte the layout `safety_link.h` already parses" against
      `KilnFW/App/drivers/safety_link_frames.c:600-641`'s independent
      hand-written offset table — an offset mismatch passes CRC and silently
      misdecodes temperatures. Lift the offsets into a shared CommonFW header,
      the way `kilnlink_rollback_result.h` already does for that result type.
      M — **CLOSED 2026-09-04**: added `CommonFW/include/kilnlink/
      kilnlink_frame_a_offsets.h` (header-only, `KILNLINK_FRAME_A_CMD`/
      `_LEN_V1/_V2/_V3`/`_OFF_FLAGS`/`_OFF_TC_TEMP_C`/`_OFF_CJ_TEMP_C`/
      `_OFF_TC_FAULT`/`_OFF_AMPS1-3`/`_OFF_TX_DROPPED_SAT`/`_OFF_FLAGS2`/
      `_OFF_BORROWED_ZONE_INDEX`). All three sites now index through it
      instead of a literal `p[N]`/`out[N]`: `SaftyFW/src/tasks/link_frame.c`'s
      `link_frame_pack_status()`, `KilnFW/App/drivers/safety_link_frames.c`'s
      `safety_apply_status()`, and `SaftyFW/test/test_link_frame_wire.c`'s
      `mirror_apply_status()` — plus `safety_link.h`'s and `link_frame.h`'s
      own `_LEN_V1/_V2/_V3` defines, which now alias the shared macro.
      `App/test/frame_a_offset_drift_check.py` rewritten: it no longer diffs
      three offset tables (there's only one left, and the C compiler already
      enforces agreement) — it now regex-scans the same three functions for a
      reintroduced bare literal `out[N]`/`p[N]`/`payload[N]` offset (N != 0)
      and fails closed if any of the three target functions can no longer be
      located at all. Negative-tested: changed one `out[KILNLINK_FRAME_A_OFF_
      AMPS2]` back to `out[15]`, check failed naming the exact file/line/
      literal, reverted, confirmed clean. SaftyFW host tests 2068/2068,
      KilnFW host tests unaffected by this change (pre-existing, unrelated
      `test_st7796_panel.c` failures untouched — out of this item's scope),
      both `build_kilnfw`/`build_saftyfw` target builds pass.
- [x] **The drift test for the item above is itself a third hand-copy.**
      `SaftyFW/test/test_link_frame_wire.c:93` `mirror_apply_status()` is a
      transcription of `safety_apply_status()`/`safety_parse_fw_version()`
      (KilnFW `safety_link.c`), not a link to them — it can drift green
      exactly like the two functions it's meant to catch drifting from each
      other. Add a CI check that diffs the `p[N]` offset lists between the
      mirror and the real function. M — **CLOSED 2026-09-04**:
      `App/test/frame_a_offset_drift_check.py` extracts the ordered
      `p[N]`/`out[N]`/`&x[N]` offset table for Frame A's six numeric fields
      (`tc_temp_c`/`cj_temp_c`/`tc_fault`/`amps1-3`) from all three copies —
      `SaftyFW/src/tasks/link_frame.c`'s `link_frame_pack_status()` (pack),
      `KilnFW/App/drivers/safety_link_frames.c`'s `safety_apply_status()`
      (parse), and `SaftyFW/test/test_link_frame_wire.c`'s
      `mirror_apply_status()` (the drift test itself) — and fails naming the
      exact file/field/offset on any disagreement.
      `check_frame_a_offset_drift.ps1` wires it into
      `tools/run_all_checks.ps1`'s `check_*.ps1` discovery glob, same
      pattern as `check_flash_worker_lint.ps1`. Negative-tested: changed
      `safety_link_frames.c`'s `tc_fault` read from `p[10]` to `p[9]`, check
      failed naming `safety_link_frames.c: 'tc_fault' read from offset 9,
      expected 10`; reverted, confirmed clean again.
- [x] **MAX31856 fault-pin polarity is inline and host-untested.**
      `SaftyFW/src/max31856.c:231`,
      `out->fault_pin_asserted = (s_fault_gpio >= 0) && (gpio_get(s_fault_gpio) == 0)`
      — same class as the shipped S7 e-stop polarity bug. `discrete_task.c`
      already shows the fixed pattern: pure, host-tested
      `discrete_pin_policy_*_asserted()` helpers
      (`discrete_task.c:98-100`). Extract the same pattern for the MAX31856
      fault pin and host-test it; feeds S5. M — **CLOSED 2026-09-04**: split
      into `max31856_fault_pin_policy.h/.c`
      (`max31856_fault_pin_asserted(fault_gpio_high)`), same convention as
      `max31856_tc_type_policy.h`/`max31856_tc_range_policy.h`; `max31856.c:231`
      now calls it instead of the inline `== 0` check. Confirmed asserted-low
      against KilnFW's `MAX31856.c:531`/`MAX31856.h:482` ~FAULT comments
      (open-drain, active-low). Added `test_max31856_fault_pin_policy.c`
      pinning both directions, feeding S5. Negative-tested: inverted the
      predicate, 2/2064 host checks failed (exactly the two new ones), then
      reverted. Zero behaviour change; full SaftyFW host suite 2064/2064
      green.
- [x] **Dead blocking fixed-length `uart_read_bytes` branch stays loaded.**
      `KilnFW/App/drivers/espInterfaces/uart_owner.c:139`'s `rx_buffer`/
      `rx_length` branch is the exact pattern behind the 100%-timeout
      incident. No current caller passes `rx_buffer` (grep across
      `App/drivers/*.c` turns up nothing), so it's dead today, but nothing
      stops a future caller reintroducing the hazard. Delete the branch, or
      assert it unreachable once a `uart_protocol_t` is attached. S —
      **CLOSED 2026-09-04**: the struct's `rx_buffer`/`rx_length`/
      `rx_length_out` fields and public `uart_owner_transfer()` signature are
      used by test stubs, so full deletion was awkward; the branch now fails
      loudly (`ESP_LOGE` + `ESP_ERR_NOT_SUPPORTED`) instead of blocking, per
      `LINK_PROTOCOL.md` §3.
- [x] **`LINK_PROTOCOL.md` section 10's completion checklist is stale.**
      TRIP_EVENT dedup, the POWER/DIAG frames, and the 30 s firing-abort are
      all listed unchecked (`CommonFW/docs/LINK_PROTOCOL.md` sec 10) though
      implemented — `safety_link_frames.c:280-295` (TRIP_EVENT dedup),
      `safety_apply_power()`/`safety_apply_diag()` (same file, POWER/DIAG),
      and `profile_executor.c:1112-1147` plus
      `test_safety_link.c:77-92`/`:86-92` (30 s abort, tested and pinned).
      `CommonFW/docs` is owned by CommonFW, not KilnFW — someone with edit
      access there needs to tick these. S (doc-only) — **CLOSED 2026-09-04**:
      each verified directly in code before ticking —
      `safety_apply_trip_event()` (`safety_link_frames.c:883-928`) dedups on
      `trip_seq` via `safety_trip_decision.c`, with the boot-reboot dedup
      reset at `:280-295`; `safety_apply_power()` (`:685`) and
      `safety_apply_diag()` (`:793`) both exist and apply their frames;
      `profile_executor.c:1201-1236`'s `safety_link_silent_30s` /
      `SAFETY_LINK_FIRING_ABORT_SILENCE_MS` implements the 30 s abort,
      pinned by `test_safety_link.c:77-92`
      (`test_firing_abort_ms_constant_is_30000`). `LINK_PROTOCOL.md` sec 10
      updated: TRIP_EVENT, DIAG, POWER, and the 30 s firing-abort line items
      ticked with file:line citations; the two still-genuinely-open liveness
      items (pre-first-frame-down, bench-escape doc) left unchecked.
