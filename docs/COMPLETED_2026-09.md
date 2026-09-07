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
      `heap_caps_malloc` SPIRAM; same gap in `backup_export.c:138` (that file
      was `backup_http.c` at the time; split since). This is the
      most-polled handler against the tightest heap. S — **CLOSED 2026-09-04**:
      both converted to `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
      (`dashboard_http.c:582`, `backup_export.c:138` and `backup_import.c:1079`
      -- both since split out of what was then `backup_http.c`);
      swept the rest
      of the HTTP handler files and found no other plain `malloc` of a
      response/scratch buffer.
- [x] **12 files exceed the 1500-line rule.** `autotune_engine.c` (4120),
      ~~`wifi_prov.c` (2820)~~, ~~`dashboard_http.c` (2572)~~, ~~`ota_http.c` (2508)~~,
      ~~`ui_page_home.c` (2279)~~, ~~`panel_spi.c` (2174)~~, ~~`profiles_http.c` (2097)~~,
      ~~`zones_config_json.c` (1867)~~, ~~`main.c` (1859)~~, ~~`backup_http.c` (1756)~~,
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
      updating. `main.c` part — **CLOSED 2026-09-04**: move-only split, by
      boot phase, into `main.c` (218 lines — `app_main()` plus the two
      helpers shared across every phase, `main_heap_stage()` and
      `main_kiln_enter_safe_state()`), `main_internal.h` (126 — the new
      `main_boot_ctx_t` struct, which replaced app_main's flat stack of
      `static` locals so each phase function can take one pointer instead of
      an ever-growing argument list, plus the phase prototypes),
      `main_boot_early.c` (704 — entry through "heap stage display+touch":
      reset-reason/coredump diagnostics, the ESP32-S3 die-temp sensor,
      time_sync, Wi-Fi+mDNS, the I2C bus and SX1509/kiln_io board layer,
      boot_guard/rtc_watchdog/watchdog_cfg/boot_button, i2c_scan, the shared
      SPI bus, the MAX31856 thermocouple channels, and display/touch/
      screen_idle/backlight-pwm bring-up), `main_control_bringup.c` (218 —
      through "heap stage executor+autotune": safety_link, danger_mode,
      heat_enable, kiln_io_owner, relay_cycles, profile_executor,
      autotune_engine, the flash-safe executor), `main_network_http.c`
      (644 — through "heap stage uart_owner+proto": every HTTP route
      registration (dashboard/board-temps/settings/zones/profiles/factory-
      reset/readiness/log/adaptive-tune/diagnostics/partition-info/ota/kiln-
      cfg/backup/safety-cfg/sim), the OTA rollback-confirmation task, the
      monitor task, and the PC-link UART owner+protocol stack),
      `main_bridges_bringup.c` (228 — through "heap stage app_main_done":
      the first- and second-wave UART bridge tasks, `lvgl_port_start()`, and
      the fail-safe link watchdog). `app_main()` calls the four phase
      functions in exactly the order it used to inline their bodies; each
      phase ends on the identical `heap_stage()` checkpoint name it logged
      before the split, so the boot log's stage sequence is unchanged.
      SX1509Class needed pulling out of the owner-gated `SX1509_internal.h`
      into plain `SX1509.h` for `main_internal.h` to reference the type
      without every including file having to `#define SX1509_OWNER_BUILD`;
      only `main_boot_early.c` (the actual SX1509 bring-up) defines that
      macro and includes the gated header. Symbol audit: only three symbols
      needed widening from `static` to non-static (used from more than one
      of the new files) — `TAG`→`MAIN_TAG`, `heap_stage`→`main_heap_stage`,
      `kiln_enter_safe_state`→`main_kiln_enter_safe_state`; grepped across
      all of `App/` for both a non-static definition and a same-named static
      elsewhere and found neither (the only hits were comments in other
      files referencing `main.c`'s functions by their old names, since
      updated). `kiln_drdy_provider`, `alloc_fail_trace_cb`,
      `ota_rollback_confirm_task`/`ota_confirm_ctx_t`/`OTA_CONFIRM_POLL_MS`/
      `OTA_CONFIRM_WARN_MS` are each used from only one new file and stayed
      `static`, renamed `main_`/`MAIN_`-prefixed anyway per the "rename even
      when currently clean" rule. Boot-order preservation verified two ways:
      (1) every phase function's body was transcribed as a byte-for-byte
      relocation of the original block between the same two `heap_stage()`
      checkpoints (only local-variable references rewritten to
      `ctx->field`), diffed by eye against the pre-split file section by
      section while writing it; (2) a script stripped comments/strings from
      both the pre-split `main.c` and the five post-split files (concatenated
      in `app_main()`'s call order) and extracted the ordered sequence of
      every bring-up call (`*_start`/`*_init`/`heap_stage`/`xTaskCreate`,
      whitelisted by name) — 105/105 tokens identical in sequence; the one
      apparent mismatch in a naive unfiltered version of the same diff was
      the scanner also matching a call inside `kiln_enter_safe_state()`'s
      own definition body (present in both versions, just relocated), not
      an actual reordering. `build_kilnfw` OK; 21/21 host test executables
      pass (none of the five new files are pulled into a host-test
      translation unit, so no test file needed updating);
      `flash_worker_lint.py` stayed clean, 180 driver files scanned, 22
      allowlisted, no new filename needed adding. **No real hardware boot
      was observed for this split** — the board is shared with other
      in-flight work (a verification agent running builds/suites, another
      split in progress on `panel_spi.c`) and was deliberately not flashed;
      correctness rests on the move-only diff and the two order-preservation
      checks above, not a bench boot. This closes the item: `autotune_engine.c`
      and `ota_http.c` each already had their own in-progress split land
      separately (see their own entries above/below).
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
      `KilnFW/App/drivers/safety/safety_link_frames.c:600-641`'s independent
      hand-written offset table — an offset mismatch passes CRC and silently
      misdecodes temperatures. Lift the offsets into a shared CommonFW header,
      the way `kilnlink_rollback_result.h` already does for that result type.
      M — **CLOSED 2026-09-04**: added `CommonFW/include/kilnlink/
      kilnlink_frame_a_offsets.h` (header-only, `KILNLINK_FRAME_A_CMD`/
      `_LEN_V1/_V2/_V3`/`_OFF_FLAGS`/`_OFF_TC_TEMP_C`/`_OFF_CJ_TEMP_C`/
      `_OFF_TC_FAULT`/`_OFF_AMPS1-3`/`_OFF_TX_DROPPED_SAT`/`_OFF_FLAGS2`/
      `_OFF_BORROWED_ZONE_INDEX`). All three sites now index through it
      instead of a literal `p[N]`/`out[N]`: `SaftyFW/src/tasks/link_frame.c`'s
      `link_frame_pack_status()`, `KilnFW/App/drivers/safety/safety_link_frames.c`'s
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
      `KilnFW/App/drivers/safety/safety_link_frames.c`'s `safety_apply_status()`
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
      `KilnFW/App/drivers/owners/uart_owner.c:139`'s `rx_buffer`/
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
- [x] **Nine checks/tests broke silently on the 2026-09-04 file splits above
      (source-file path drift).** All twelve splits above (`profile_executor`,
      `autotune_engine`, `wifi_prov.c`, `dashboard_http.c`,
      `zones_config_json.c`, `uart_bridge_ext.c`, `zones_http.c`,
      `ui_page_home.c`, `main.c`, etc.) were clean moves, but nine separate
      guard scripts and host tests kept a hardcoded reference to a source
      file PATH or a pre-rename IDENTIFIER that a split moved, and none of
      them failed — build and host tests stayed green throughout. Found one
      at a time, by accident: `flash_worker_lint.py`'s allowlist,
      `check_link_impl_isolation.ps1`'s allowlist (`zones_config_json.c`
      after `compute_crc()` moved to `zones_config_migrate.c`),
      `check_heat_enable_wiring.ps1`'s singular `autotune_engine.c` glob
      (function moved to `autotune_engine_guard.c`),
      `test_autotune_wire_layout.py` (hardcoded `uart_bridge_ext.c` path AND
      pre-rename `bx_put_*` identifiers), `test_autotune_rules_drift_guard.py`
      (hardcoded `dashboard_http.c`), and the worst case,
      `test_selfcheck_zones_fields.py`, which guarded its hardcoded path
      with `if not path.is_file(): skipTest(...)` — the split turned it into
      a silently-skipping green test with zero real coverage — plus a stale
      `.obj` for a deleted source tripping a duplicate-symbol check. S —
      **CLOSED 2026-09-04**: all nine fixed (globs instead of single
      filenames where a family of siblings now exists, updated single paths
      where a file moved outright, updated identifiers to their post-rename
      spelling). New standing guard added:
      `firmware/KilnFW/App/test/source_path_drift_check.py` (wrapped by
      `check_source_path_drift.ps1`, auto-discovered by
      `tools/run_all_checks.ps1`'s `check_*.ps1` glob) scans
      `tools/*.ps1`, `firmware/*/tools/*.ps1`,
      `firmware/KilnFW/App/test/*.py`, and `tools/PcTools/tests/*.py` for
      hardcoded source-file path references (PowerShell `Join-Path` calls,
      Python `/`-joined path chains, and flash_worker_lint-style bare
      filename allowlist entries) and fails naming file:line and the
      missing path whenever the referenced file no longer exists; it also
      flags the `test_selfcheck_zones_fields.py` skip-guard pattern
      specifically (a missing hardcoded path guarded by `skipTest`/
      `pytest.skip`/`if not ...is_file()` instead of failing). Fails closed
      if its own extraction regexes stop matching (a floor on total
      references found, not just zero problems). Negative-tested both
      variants (a missing `Join-Path` literal, and the reproduced skip-guard
      case), each reverted clean afterward.
      **GUIDANCE FOR THE NEXT SPLIT**: after splitting any file, grep
      `tools/` and `tools/PcTools/tests/` for the OLD filename and for any
      identifier that moved or was renamed in the split — do not rely on
      build/host-test green alone. `source_path_drift_check.py` now catches
      the path half of this automatically; it does not catch a moved-but-
      still-existing identifier (that class is `stub_signature_drift_check.py`
      and this check's own job description do not overlap there), so the
      identifier grep is still a manual step.

## HTTP connection resets under concurrency — root cause and fix, 2026-09-04

Moved from `ROADMAP.md`'s "Software, doable now" table on 2026-09-04, per the
upkeep rule. Root cause was `CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE` defaulting to 6
— lwIP aborts (RST) any handshake-completed connection that can't post into
that fixed-size mailbox, independent of `backlog_conn`, `max_open_sockets`,
and the OS socket table (all three chased and refuted across four prior
passes, including an earlier "not a heap failure" finding that turned out to
be a step toward this one, not a separate mechanism). Raised 6 → 16; reset
rate went from 22.5% to 0.0% at the same concurrency levels that were
failing.

## M11 — UI consolidation, full detail, moved 2026-09-04

Moved from `ROADMAP.md`'s M11 milestone per the upkeep rule (M11 itself is
fully closed — see ROADMAP.md for the one-line summary).

Batch of direct owner requests, all landed: manual relay-control web page
removed (the diagnostics Danger Zone is the sanctioned hand control,
`POST /api/relay` removed with it); board-health and thermocouple-fault
pages folded into `/diagnostics`; safety pages grouped into one expanding
nav group; shared per-zone timing profiles (the nine per-zone timing fields
moved into named profiles zones point at, `ZONES_CFG_VERSION` 8→9, lossless
migration that de-duplicates identical value sets); relay/IO segments added
to firing profiles, blocking or non-blocking with a per-segment leave-state
choice (`PROFILE_VERSION` 2→3, `73c03c0`, migration verified byte-identical
against the owner's live board); the rules engine deleted outright
(`56dfa07`) — sequenced deliberately AFTER segments so there was never a
window with no way to drive a non-zone relay, with its watchdog role
replaced by `profile_executor.c`'s `io_segs_force_all_off(false)`; names
added for non-zone relays; LCD pages folded the same way as the web
(safety/board-health/thermocouple-fault pages combined into six
Prev/Next-paged diagnostics screens, kiln setup/thermocouple-type/kiln
config pages removed, profiles moved to top-left of a one-page main menu);
a planned-profile preview added to the profile-detail page (the LCD could
previously only draw a planned curve for an already-running profile — the
preview went on the detail page rather than the home chart, which stays
intentionally coupled to the running executor's snapshot).

Durable constraint this work ran into and left behind: `zones_cfg_t` is
500 B against a hard 512 B `ZONES_CONFIG_BLOB_MAX_SIZE` (also
`kiln_cfg_store.c`'s buffer size, not a free knob) — the timing-profile work
spent the remaining slack reordering the struct to kill alignment padding
and cutting the profile-name length to 7. And the hazard every item here
shares: two persisted structs embed arrays by value
(`zones_cfg_t.zones[]`, `profile_t.segments[]`), so adding one field to an
element displaces every element after the first — each migration needs a
FROZEN snapshot struct of the old layout and a field-by-field walk.
`profiles_http.c` once shipped the version of this bug that used `sizeof`
the *current* struct for the *old* version, which rejected every profile on
the owner's board and marked them unused; only a hardware flash caught it.

## M13/M14 — fault reporting and verification findings, full detail, moved 2026-09-04

Moved from `ROADMAP.md`'s M13/M14 milestones per the upkeep rule (M13's
standing rule and clearing-semantics note stay in ROADMAP.md; M14 is fully
closed).

**M13 landed, 2026-08-28**: S6a decodes its fault-source bitmask everywhere
it's reported (web, diagnostics, LCD) through one shared table,
`safety_trip_words.h`, ending a prior drift where the LCD and web showed
different text for the same trip. The mask is captured AT TRIP TIME, not
read live, and flagged invalid when untrustworthy (an ESP reboot with a trip
still latched would otherwise misattribute this boot's sources to an older
trip). Every `safety_trip_t` cause line carries real numbers — S1's
temperature vs. ceiling, S3's channel currents vs. threshold, S6b's elapsed
silence, S11's reading vs. window — pulled from data that was already
arriving on the wire and simply never copied into the message; where a
number genuinely doesn't exist the sentence says so (caught by a negative
test that found S12 about to print the wrong sensor's reading). S9 (a
possibly-welded contactor) says explicitly it is NOT clearable from the UI.
A repo-wide sweep for raw hex fault codes found and fixed two operator-facing
misses on the first pass (autotune/profile-executor "heat is blocked"
refusals reading `fault sources 0x%02X` — a different phrase than the
`reason 0x%02X` the first sweep grepped for), verified by negative-testing
the fix itself.

**M14, 2026-08-28**: `build_kilnfw`'s PowerShell wrapper never propagated
ninja's real exit code (`powershell.exe -Command` needs an explicit
`exit $LASTEXITCODE`), so a failed build printed "OK" — caught live with
"OK in 4.8s" printed over a log containing `ninja: build stopped`. This sat
above every other guard, host suite, and review in the repo, since all of
them ran through it; every sibling tool was checked rather than assumed
safe, and only this one had the bug. `flash_firmware` was checking only
that the `.bin` existed, not that it matched HEAD — fixed to compare the
recorded build commit against HEAD, catching a real staleness on its first
run. Two stack overflows found (`safety_poll` crashed twice, `safety_proto_
rx` found at 25% by reading margins before it crashed) because both tasks
were unregistered with the stack-margin instrumentation that already
existed. Two new CI guard scripts (source-in-CMakeLists, no relay write
outside `kiln_io_owner`) both found real violations on their first run, one
of which three rounds of opus review had missed because reviews read the
diff and the violation wasn't in the diff.

## M10 — instrumentation findings, full detail, moved 2026-09-04

Moved from `ROADMAP.md`'s M10 milestone per the upkeep rule; ROADMAP.md keeps
one line per finding.

- HTTP route table (`max_uri_handlers`) silently overflowed at 84 vs. 85 real
  routes, so one route 404'd with no visible cause; had fallen behind four
  times before. Raised to 95, guarded by `tools/check_uri_handler_cap.ps1`
  which recounts from source. `2026-08-24`.
- A quarter of every safety poll logged as a timeout on a healthy link:
  `safety_drain_still_waiting()` didn't recognize an out-of-turn STATUS
  frame, so the poll abandoned its budget early — the same bug a prior fix
  had closed for three other callers, missing this fourth. Fixed; 310 polls,
  0 timeouts afterward. `80f473d`.
- Internal-DRAM low-water alarm split into a standing WARN and a `DRAM
  REGRESSION` ERROR. Real trough is `app_main_done`, ~1 kB below the
  previously-quoted `uart_bridges_1` figure, and sits below the one
  documented real failure (free=11903 B, largest=8704 B — `/app.js`
  truncated, pages stuck "Loading..."). `8d1b015`.
- Display-power feature's ~3.25 kB DRAM cost measured under HTTP load (not
  just idle) against that failure floor: min_free 23195 B, largest free
  block 15360 B — ~11.3 kB of margin over the failure case, comfortable.
  `get_stack_margin()` checked in passing: no resize needed.
- `check_stack_margin_registration.ps1` went blind to three real tasks when
  `main.c` split into `main_network_http.c`, because it scanned
  `App/drivers/**` + `App/main.c` *by name* rather than globbing `App/*.c` —
  the tenth instance of the "split breaks a filename-keyed check" class this
  project has hit. Fixed to glob `App/*.c`. `ead4123`. Measured after: three
  previously-invisible tasks all 48–76% headroom.
- `info_uart_bridge`'s worst-case stack use found, measured, fixed: its
  `GET_STACK_MARGIN` reply is always truncated at ~25 registered tasks, so
  every call takes the log-heavy truncation-warning branch — worst case
  476 B free of 3072 (15.5%, `[LOW]`), reproducible under adversarial HTTP
  load. Bumped to 3584 B (PSRAM-backed, doesn't touch the internal-DRAM
  budget above); re-measured at 988 B free (27.6%, `[OK]`).
- Separately, `rules_task` (which gates heating) was reporting 45.3%
  headroom when it was actually 10.9%, because the instrumentation had
  inherited vanilla FreeRTOS's word units while ESP-IDF's high-water-mark
  API returns bytes — a stray ×4. Raised 3072→4096. `d90986c`.
- `GET_STACK_MARGIN` pagination shipped without a `UART_PROTOCOL_VERSION`
  bump even though it inserted header bytes an old client would silently
  misread as entry data — bumped 10→11 (PC↔ESP only, independent of the
  isolated-link version). Also added missing test coverage of the
  client-side paging *loop* itself (only single-page decode had been
  tested), with a negative test proving the repeat-guard is load-bearing.
  Hardware-verified: all registered tasks visible in one call, no task
  below 25% headroom.
- `/api/readiness` and `GET /api/zones` could return short, invalid JSON on
  a buffer overflow and look like a pass — the one endpoint where "absent"
  reads as "approved". Fixed to reserve space for the terminator and report
  a drop as an item (or a named 500) instead of truncating silently.
  Negative-tested with the buffer artificially cut. `4e8e1f1`, `88f12e0`.
- Stack margins re-read after a real (heaters-disconnected) firing that
  walked the full executor state machine — all margins held.
- `rules_task`'s callees audited transitively for NVS/flash writes: none
  found, but `dashboard_get_status()` (called every tick) did a
  cache-disabling flash *read* behind a first-caller-wins static — safe
  only by which task happened to run first, a race not a guarantee. Primed
  at startup instead; stack moved to PSRAM. Standing DRAM regression
  (6771→10675 B free at the same checkpoint) resolved. `47b004c`.
- Four internal-DRAM task stacks were sized by one constant
  (`UART_OWNER_STACK_SIZE`) that actually covered two pairs, not one —
  `safety_link.c`'s pair (carrying the telemetry that gates all heating)
  had never been registered for high-water reporting, so trimming on the
  visible two would have resized two unmeasured tasks. Both pairs
  registered, then trimmed 4096→3072 (~70% headroom on all four, 4 kB DRAM
  reclaimed). `8ad7d5b`.
- Cross-language (Python producer / C consumer) PC-tool heartbeat contract
  had no guard — the fourth instance of the "consumer without producer"
  class where the producer lives in the other language. Added
  `tools/check_heartbeat_contract.ps1`, negative-tested against all three
  failure shapes it catches (producer commented out, timing margin
  violated, task-id collision).

## Sustained heat impossible on the bench — S6a firing within ~1s — full diagnosis, 2026-08-29

Moved from `ROADMAP.md`'s "Software, doable now" table on 2026-09-04
(originally closed `63cc741`). Root cause was not the wire, not the Pico, and
not a consumer-cannot-keep-up shape: nothing was ever dropped (0 CRC errors,
0 resyncs, 0 discards throughout), the frames were only ever *late*.
`KilnFW`'s `uart_protocol_rx_task` read with
`uart_read_bytes(port, chunk, sizeof(chunk), 200 ms)` where `chunk` had been
sized to the worst-case stuffed frame (528 B) by `3149393`. That call
re-blocks until `length` bytes arrive or the timeout expires, and on a link
whose frames are ~40 B at ~10 frames/s, 528 B never arrive — so every read
held its bytes the full 200 ms, against a ~345 ms reply budget. By the time
it was characterised it was timing out on **100%** of polls (42/42), not
20%. Fixed by reading only what `uart_get_buffered_data_len()` reports with
a zero timeout, blocking for a single byte (bounded 100 ms) only when
nothing is buffered. Measured after: **0 timeouts in 340+ polls**,
request/reply back to 1:1, config fetch converging in 3 page requests
instead of 25. A bounded firing run held `running` with no heat block for a
full 60 s sweep (regression-asserted in `test_live_bench_tuning.py`). DMA
was evaluated and rejected as the fix — see `LINK_PROTOCOL.md` §3 "Never
wait on a receive buffer filling."

The apparent remaining bench limit ("no heating element fitted, PV does not
move") was itself wrong, corrected 2026-08-29: every zone has a real heater.
PV did not move for two firmware reasons, both since fixed: K4 was never
requested (`heat_enable.c`), and zone 0's 2 s time-proportioning window could
not render any fractional duty against the 10 s minimum on-time. "It must be
the hardware" was the third wrong diagnosis this one symptom attracted.

## "What is actually left" closeout narratives, moved 2026-09-04

The paragraphs below were struck from `ROADMAP.md`'s "What is actually left"
section per the upkeep rule; each is closed and carries no open action.

**Closed 2026-08-27/28:** the task stacks were re-read after a real firing
and 4 kB of internal DRAM reclaimed; `rules_task`'s callees were audited
(nothing writes flash — the real finding was a cache-disabling *read* via
`dashboard_get_status()`) and its stack moved to PSRAM; the guard scripts now
run from `tools/run_all_checks.ps1` and the `run_repo_checks` MCP tool.

**Closed 2026-08-30:** `PID_EXPANSION_PLAN.md` Phases 1-4 landed — Cohen-Coon
is a selectable, reachable tuning rule alongside SIMC, and the fuzzy-PID
layer (`pid_fuzzy.c`) is wired into `profile_executor.c` and selectable from
`zones_page.html`. SNTP/NTP time sync landed. The first autotune runs ever to
complete on real hardware fitted all three zones, and the first full 3x3
cross-zone coupling matrix and RGA were measured — see
`PID_EXPANSION_PLAN.md` §2 for the numbers (superseded since). Proposed gains
reviewed, not accepted.

**PID/adaptive tuning status as of 2026-09-02** (detail owned by
`PID_EXPANSION_PLAN.md`): coupled identification and the diagonal model
refine are built and hardened, but their hardware clearance was **withdrawn
2026-09-02** — the solve is sound, but the harvest layer feeding it records
non-steady duties as DC-gain observations (fired on 12 of 12 joint
observations on the `coupid6` capture). Never yet run on the kiln. The Ki
diagnosis layer is built with all code blockers closed — also never yet run
on the kiln. Dynamics-from-ramps was tried and **shelved**: its two-point fit
reduces analytically to `0.524·K·Δduty/ramp_rate`, an artifact of the
commanded ramp rate with no plant content. A real board-wide deadlock was
found and fixed — pressing Accept on an autotune result over the UART bridge
re-entered the flash worker and hung it permanently, taking down every UART
bridge, `safety_cfg_store`'s deferred NVS flush, and `adaptive_tune`'s
persistence with it. All adaptive layers stay per-zone opt-in, default OFF.

**Closed 2026-08-31/09-03 — ramp assist, landed end to end** (detail owned by
`PID_EXPANSION_PLAN.md` §7): a pyrometric cone table (`cone_table.c`/`.h`,
Orton 022-14 incl. half-cones, Arrhenius heat-work weighting), sustained-lag
detection and auto-stretch in the executor, and a dwell heat-work credit that
shortens the following dwell when it was earned lagging — the SPEND is gated
on `ramp_assist_enabled` so disabled behaviour is bit-identical to before
this landed. Web banner, event log, and LCD lag notice all wired to the same
richer sustained-lag fields. Control surface is done and **defaults OFF** —
gated on a real cone-temperature firing (see the GATED table in `ROADMAP.md`).
A same-day defect sweep found and fixed a duplicated band-width formula, a
band-cliff bug in `cone_table.c` itself, and a dwell-credit crash, plus ten
wrong cone temperatures in the Orton table (mirror test added). Further
hardening, still pre-real-firing: dwell credit was found **unreachable** —
its gate was tied to the 25 °C ramp-lock band instead of schedule lag — and
fixed (`cf3763c`); accrual was then extended to keep crediting past the
nominal ramp end (`0402ecb`). Separately, three ramp-lock/guard interaction
bugs surfaced and were fixed: a hot-start stall (one-sided lock + a guard-4
arming backstop, `8f12449`), a false guard-4 trip during autotune's SETTLING
phase that the backstop itself introduced (`7911f26`), and guard 4 mistaking
autotune's synthetic setpoint for a real one (`1bfd5ee`).

**Also closed in the same window:**
- Relay-autotune thermal guards 1/2 now use an amplitude discriminator
  (`relay_min_swing_c`) in place of a directional test that could never fire
  mid-limit-cycle — `df3b31b`.
- A coupled-solve `use_measured_diag_k_dc` flag shipped, **default OFF**.
- Measurement tooling: `noise_floor.py` gained a start-temperature covariate
  and cooldown-sidecar refusal; `pid_ab_compare` now gates on a start-temp
  confound instead of ignoring it.
- `run_queue.py` hardening: capture opens before the start POST, a real
  wait-until-actually-finished replaces the old "first idle sample = done"
  logic (`0a0ccd7`), campaigns are resumable via a durable state file, a
  clobber is refused rather than silently overwritten, and a capability
  preflight fails a stale-firmware preset before the campaign starts rather
  than mid-run.
- Web UI: firing-flow profile feasibility warnings, a live flash partition
  table on the diagnostics page, a new-profile segment preview graph, the
  duty axis in percent with a rotated label, a falling-behind-schedule
  banner, and RGA/status cells that no longer rely on colour alone (plus an
  extended colour-only checker guard script).
- `PID_EXPANSION_PLAN.md`'s fuzzy-layer hardware-run blocker (a
  `zones_http_client` field-mapping bug) resolved (`b1ea749d`).

**Closed 2026-09-04 — safety-case and guard-coverage hardening** (full detail
owned by `GUARD_TEST_MATRIX.md`, `SAFETY_MODEL.md` and `docs/SAFETY_CASE.md`):
- 27 previously-unfuzzed `kilnlink` payload decoders got seeded corpus+random
  fuzz targets in `firmware/CommonFW/test/test_fuzz_payloads.c`, canary-guarded
  and ASan-clean (`ccb23ac`).
- §1 nuisance-rejection coverage completed for S3/S4/S10 (`7e2dc1c`);
  `discrete_task.c`'s debounce extracted into a pure `debounce_policy.c/.h`,
  closing the S6a/S7 host gaps (`d506a54`); S9's decay behaviour, which
  overturned a prior "hardware-only" verdict (`7e84534`). SaftyFW host checks
  went 2077 → 2131.
- Every `SAFETY_MODEL.md` summary-table row got a per-row verification state
  (`ecdad62`); `SAFETY_CASE.md` was corrected the same day — it had
  understated thermal_guard guards 1/2/4/5/7 as argued-only when they are
  genuinely host-tested, and guard 9's "found on the bench 2026-08-25" claim
  was retracted after a git-history search found nothing behind it (`a2df81a`).
- Triage of 89 unchecked items across `UPDATE_PROTOCOL.md`/`LINK_PROTOCOL.md`/
  `REPO_LAYOUT.md`: ~66 were already done and merely unticked, now cited
  (`503ab3b`), plus four protocol/safety doc defects fixed, including
  documenting `SAFETY_CMD_ANNOUNCE_REBOOT` (0x18).
- New `tools/wire_protocol_fingerprint_check.py`: fails when a wire-relevant
  declaration changes without a protocol version bump — two such bumps had
  already shipped missing one (`bf401f4`).

**The fuzzy-PID A/B campaign was found structurally inert, fixed, then found
inert again for a different reason.** Both original presets had
`control_mode: 2`, so the fuzzy layer (only runs under mode 3) never engaged
and the campaign was silently comparing PID against itself (`8906686`).
Presets fixed and the campaign restarted as `fuzzy_ab_20260904c`, with live
`bd_*` proof effective gains now differed from base (`91c5d6d3`). A separate
audit of the *ease-off* A/B against the same inert-campaign bug class
confirmed it was **not** inert and its "indistinguishable" conclusion stands;
it also added a standing pre-flight check (`PID_EXPANSION_PLAN.md` §3.6b)
requiring live `bd_*` proof before any future control-law A/B (`51e3d59`).
**Superseded 2026-09-04:** `778ad64` found the resulting `fuzzy_ab_20260904c`
data itself never leaves the membership layer's centre rule cell (n=29
firings, 37,008 zone-samples, 100.000% ZERO/STEADY) — the campaign ran
cleanly but was measuring a fixed gain rescale, not fuzzy adaptation. See
`ROADMAP.md`'s "Blocked on you" table for the resulting owner decision.
Cross-campaign tracking was separately synthesized across all 27 usable
coupling captures: the coupling-matrix fix is confirmed (n=12 vs 5), z0's
dwell-entry overshoot is the worst tracked case at 2.18 °C, and no capture
anywhere has yet exceeded 60 °C (`d5ae465`) — detail in
`PID_EXPANSION_PLAN.md`.

## M12a — the commissioning surface lied, full postmortem — opened and closed 2026-08-28

Moved from `ROADMAP.md` on 2026-09-04. Found by an opus audit on 2026-08-28,
triggered by a routine attempt to set `abs_max_temp_c = 80` on the bench.
**Three `POST /api/safety/commissioning` requests returned `{"ok":true}` and
not one value changed**, including a control write to a harmless parameter.
The decisive evidence, all live: `live_config_crc` never moved (a successful
`config_store_write()` bumps `seq` and therefore the CRC), while the Pico's
own histogram read `commit_config_rejected=2` — it refused, it said so, and
the ESP discarded the refusal.

Four defects, each verified against source:

1. **`ok` cannot fail.** `apply_pairs()` returns true when the send returns
   `ESP_OK` (`safety_cfg_http.c:434/446/472`), but SET_PARAM and COMMIT_CONFIG
   both go out as broadcasts (`safety_link_commands.c:862` and `:950`; that
   code lived in `safety_link.c` at the time, since split out) and
   `uart_protocol.c:795` returns `ESP_OK` for "the local UART accepted the
   bytes" — no ack wait, no retry.
2. **The rejection is caught in a ~144 ms race and then thrown away.**
   `safety_link_commands.c:968-969` (was `safety_link.c` before the split)
   waits `SAFETY_LINK_REPLY_TIMEOUT_MS`; a late
   REJECTED frame reaches `safety_drain_inbox_ex()` and is counted and
   dropped, with no stash slot the way CONFIG_PAGE has one. Silence was
   defined as acceptance.
3. **The GET is an ESP-local NVS cache presented as current.** Refreshed only
   when CRCs disagree; they agreed, so there were zero fetches this boot.
   `fetched_ms_ago` was board uptime, not fetch age.
4. **`"set": true` is unconditional** — the Pico emitted every field without
   consulting `rec.fields_set` and the ESP set `set = 1` for every entry
   received. So `abs_max_temp_c {set:true, value:0}` asserted a *commissioned
   ceiling of 0 °C* (which means NEVER TRIP) for a field the Pico had never
   had set.

Fixed (`ddbd024`, `3149393`): stashed unclaimed REJECTED frames; commits
confirmed by reading `config_crc` back rather than trusting the ACK; refetch
after commit reports the CONFIRMED value; `fields_set` carried through the
CONFIG_PAGE codec (protocol 7→8, floor held at 7) so `set` can be false;
`fetched_ms_ago` reports real fetch age; the ARMED/GRACE write window is
surfaced in the page itself.

**Fixing it immediately exposed an older defect it had been hiding**: with
the read-back real for the first time, page 1 of the config fetch timed out
on every attempt. `cmd_config_page` had been **0** for the life of the
project — the ESP had never once fetched a page. Cause: `uart_protocol_rx_task`
read UART bytes 32 at a time, so a ~157-253 byte CONFIG_PAGE needed 5-8
scheduler round trips against a ~145 ms budget while small STATUS/DIAG/POWER
frames needed 1-2. Fixed by reading a whole frame in one go and giving the
reply 300 ms of margin (the 2000 ms multi-page ceiling untouched, since
growing that had caused an earlier panic-reboot regression). Two more bugs
surfaced in the same instrumentation pass: `link_task` polled its RX ring
once per 100 ms (now 10), and the pre-send drain discarded a CONFIG_PAGE for
the wrong index instead of stashing it.

**Net result:** `abs_max_temp_c = 80` is committed and confirmed on the
bench — S1's absolute ceiling was armed for the first time in the project.
**The constraint this uncovered, which shaped M12's whole design:** config
writes are refused whenever the relay owner is `ARMED` (steady state ~60 s
after boot), so the only write window is the boot GRACE period — nothing in
the UI, API or error text said so before this was found.

## M7 — repo reorganisation, CLOSED 2026-09-05

Moved from `ROADMAP.md` M7, which is now removed from that file entirely —
all three items are done: the two moves below, plus `hardware/UnitTestFixture`
which the owner decided to KEEP (2026-09-05, no code/hardware action taken).

- [x] `pdfMcp/` moved under `tools/` — 2026-08-28. Its own running `pdf-mcp.exe`
      process blocked a plain rename the same way `mykicadMcp`'s would, so
      `tools/pdfMcp/` is a copy, not a move; `.mcp.json` updated to the new
      path. The stale root-level copy cleans up on the next session restart,
      once nothing holds it open
- [x] `mykicadMcp/` moved under `tools/` — 2026-08-28, as its own dedicated pass
      per the plan above: stopped the `kicad` server (`mcp_servers.ps1 stop
      -Server kicad`), moved the submodule (a directory-rename `git mv` hit
      the same "Permission denied" this repo's original hardware/firmware
      split ran into — worked around the documented way, pre-creating the
      destination and moving children individually; one stale abandoned
      `.claude/worktrees/` leftover from an unrelated old session couldn't be
      moved and was left behind, harmless debris, not part of the submodule's
      tracked content), fixed the submodule's own `.git` gitdir pointer and
      `core.worktree` for its new depth (the actual cause of a first attempt
      silently re-adding it as 43 individual file blobs instead of one
      gitlink — caught by `git ls-files -s` showing `100644` entries instead
      of a single `160000`, not assumed away), updated `.gitmodules`,
      `mcp_servers.ps1`, and all 8 of the 9 `.claude/settings.json` allowlist
      entries with an unambiguous path (the 9th, `../mykicadMcp/...`, has no
      recoverable original working directory to translate against and was
      left to simply stop matching — the safe direction, a future prompt
      rather than a silently wrong grant). Verified: `git submodule status`
      resolves all three submodules, the `kicad` server restarted clean from
      the new path and answered a real `kicad_call`, and the submodule's own
      110-test suite passed unchanged from its new location

## 2026-09-04 LCD/display session — six items landed

Moved from `ROADMAP.md`'s M1 and "Genuinely still software" sections during
the fifth roadmap-upkeep sweep, same day. All from direct owner feedback on
the real ST7796 panel unless noted.

- **LCD backlight control (M1 HW-change item) — `be02d34`.** The GPIO15
  flying wire to the panel's backlight input is fitted, owner-confirmed by
  meter after it read flat with `KILNCTL_BACKLIGHT_PWM_ENABLE` off (the flag
  being off meant `backlight_pwm_init()`/`_start()` never configured the
  GPIO at all — the flat reading was consistent with either a missing wire
  or a disabled driver, and it was the driver). Flag now defaults on, and ON
  duty comes from `display_power_cfg_brightness_percent()` instead of the
  `CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT` constant it used to read — that
  setting had persisted and round-tripped through the settings API since the
  display-power feature landed but drove nothing (`brightness_inert` in
  `GET /api/settings/display`). IDLE stays a Kconfig constant, since it's
  the blanked state, not something the brightness slider addresses. The
  poll loop's skip condition now also gates on the computed duty, not just
  `screen_on`, so a brightness change is never invisible until the next
  blank/wake edge. **Not yet confirmed by meter or eye that the panel
  actually dims** — the owner is flashing this; that's the next check.
- **Touch was mirrored top-to-bottom on the ST7796 glass — `f028e2f`.**
  Pressing the bottom-right corner activated the top-right menu button: X
  landed correctly, Y was mirrored. `touch_dev_map_uncalibrated()` derives
  `out_py` from `ay` (the post-swap axis) through `invert_y`, so the
  mirror was that knob, not `invert_x`. `KILNCTL_TOUCH_CAP_INVERT_Y` now
  defaults on. The capacitive orientation knobs (`KILNCTL_TOUCH_CAP_*`) are
  now written `default <v> if KILNCTL_DISPLAY_PANEL_ST7796` instead of as
  bare defaults, since film orientation under the glass is a property of
  the display module, not the board — the same reasoning that already
  splits the capacitive knobs from the resistive ones. A future second
  panel gets its own conditional line instead of silently inheriting the
  MSP4031's. Same commit also flipped `keep_on_while_firing`/
  `display_on_error` to default **true** (previously false, argued only
  from internal consistency with `DISPLAY_TIMEOUT_NEVER` — an argument
  about the code, not what the operator wants).
- **LVGL wake-edge invalidate reentered its own flush callback — `51e1ef5`,
  killed a live firing on the bench before the fix.** `ili9488_flush_cb()`
  ran `lv_obj_invalidate()` on the off→on wake edge from inside LVGL's own
  active-refresh flush callback, reentering its invalid-area walk and
  starving the idle task until the task watchdog fired on the LVGL task.
  Reproduced twice on an idle board. Fixed by moving the wake-edge
  invalidate into `lvgl_port_service_idle_wake()`, run from
  `lvgl_port_task`'s loop before `lv_timer_handler()`, outside any refresh.
  Verified clean on hardware; regression test added and negative-tested by
  hand. This is the root cause behind the `safety_poll`
  `IllegalInstruction`/`configASSERT` panic described in `CLAUDE.md`
  "Firmware gotchas" — `exc_addr 0x0` there was a red herring, and this fix
  is what actually closed the underlying stack-corruption path.
- **LCD diagnostics consolidated 9 pages → 6 — `1cf200f`.** Three
  per-channel Thermocouple Faults pages folded into one (per-row remedy
  text trimmed to a fixed short phrase, per-channel title folded into the
  fault line). Safety Processor and Board Health merged into one page,
  using Board Health's accented left-border row style throughout per the
  owner's stated preference; the duplicate ESP32 die-temp row was dropped
  (already shown on the PSRAM & storage page) and the live State row
  dropped its "last trip" tail (owned by the Trip Detail page). Worst-case
  content fits the 320x480 no-scroll budget: ~212–248px for the merged
  fault page, ~237–257px for the merged safety/board-health page, both
  under the ~267px ceiling.
- **Profile-detail page: chart flex-grows, buttons shrink to 40px,
  bottom-anchored — `445a78e`.** Owner feedback on the real 480x320 panel:
  action buttons dominated the screen, the planned-curve chart was a fixed
  90px, and the button row sat under the chart rather than the bottom edge.
  Chart given `flex_grow(1)`; drawn buttons shrunk from
  `UI_THEME_MIN_TOUCH_TARGET_PX` (72px) to 40px, with the action row's gap
  widened 4px → 48px so the three buttons' `ui_theme_apply_touch_area()`-
  extended (88px effective) hit boxes don't overlap — LVGL 9 has no
  per-axis `ext_click_area`, so the extension is symmetric on all sides.
  `ui_page_profile_segments.c` checked and found unaffected (no chart, no
  action-button row).
- **Built-in schedule catalogue browsed by firing type → cone, not
  publisher family — `0470185`.** The 28 built-in schedules were browsed by
  publisher family (Bartlett/Plainsman/Crystalline/General), not the axis a
  potter actually picks on. `ui_page_profiles_family.c` is now a firing-type
  picker (Bisque/Glaze/Other); `ui_page_profiles_builtin_list.c` lists that
  type's schedules sorted by cone ascending, cone shown per row.
  `builtin_profile_t` gained `.firing_type` (hand-classified — no universal
  derivation exists, so a decal firing and a quartz-inversion
  crack-avoidance schedule both land in "Other") and `.cone` (derived from
  each entry's peak segment + ramp rate off a two-speed Orton chart,
  falling back to a title-stated cone). No `target_c`/`ramp_c_per_hr`/
  `dwell_min` value was touched. `family` is kept (still shown on the
  detail page) — every consumer was checked before anything changed.
  `profiles_builtin_table.inc`'s header comment, which claimed a
  regeneration path via a `gen_builtin_profiles.py` that no longer exists in
  the repo, was corrected to say the table is hand-maintained.
- **Display settings web page restyled to house idiom — `70ef683`.**
  `settings_display_page.html` had hand-rolled `.kc-dp-*` CSS instead of
  reusing `theme.css`'s shared `.card`/block-label/`.hint`/inline-flex
  idiom already used by `zones_page.html`/`main_page.html`. Swapped the
  markup and page-local CSS; no new classes invented, no other page
  touched. Same commit fixed `test_display_power_cfg.c`'s empty-NVS
  defaults check, left stale by `f028e2f` — it still asserted `false` for
  both switches after their default flipped to `true`, and the test's
  "deliberately wrong" seed values no longer proved an override once `true`
  became the real default.
- **Builtin-catalogue cone/firing-type metadata checked against its cited
  source — `9d73c8f`.** `0470185`'s `.cone`/`.firing_type` values came from
  an Orton chart reconstructed from memory and a guessed bisque/glaze/other
  split, not from the `digitalfire.com/schedule/<slug>` page each entry
  cites. Checked all 28 against their source pages: `04DSDH` states "cone
  04" explicitly (was `-5`, now `-4`); `BQ1000` says "about cone 05" (was
  `-6`, now `-5`); `C6PLST` states its bars are unbisqued/unglazed test bars
  (was `Glaze`, now `Other`). 15 entries already matched and are unchanged.
  10 — `FSCG1`/`FSCGB1`/`FSCGCL`/`FSCGWM`/`FSCRGL`/`FSHP1`/`FSHP3`/`FSNM5`/
  `MDDCL`/`QICA` — have source pages stating no cone at all (crystalline/
  specialty schedules, or a related-but-different page's cone was being
  conflated with this one); their prior guessed value was left in place
  marked `UNRESOLVED` with an inline comment naming the page checked and
  why it doesn't resolve, rather than invented silently. No segment
  (`target_c`/`ramp_c_per_hr`/`dwell_min`/`segment_count`) was touched.
  Owner decision needed on the 10 unresolved entries — see ROADMAP.md
  "Blocked on you". Not yet flashed (firing in progress).
