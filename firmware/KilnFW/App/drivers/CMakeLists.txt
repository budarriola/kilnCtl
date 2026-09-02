# The simulated-plant backend and the thermal model it drives are only
# compiled into a KILNCTL_SIM_PLANT build. sim_plant.c deliberately lives in
# App/test/ and is shared with the host test binary rather than copied here --
# one model, so a guard trip on the host means the same thing on the board.
set(SIM_SRCS "")
if(CONFIG_KILNCTL_SIM_PLANT)
    set(SIM_SRCS "sim_backend.c" "../test/sim_plant.c")
endif()

# TODO.md 10.6a: pre-gzip the embedded web pages (+ theme.css) so
# esp_http_server can serve the compressed body straight from flash with a
# Content-Encoding: gzip header -- no runtime compression cost, smaller flash
# usage, faster transfer for any client sending Accept-Encoding: gzip (every
# real browser does; the corresponding *_http.c handlers check the header
# defensively before relying on that -- see TODO.md 10.6a's status note).
#
# zones_page.html is included here too (2026-08-21): it was the one holdout,
# left raw because its handler lives in zones_http.c, owned by parallel work
# at the time this list was first built. That work is done; zones_http.c's
# page_get_handler now serves the gzip body via web_client_accepts_gzip()/
# web_send_gzip_not_acceptable(), same as every other *_http.c handler in
# this file's list, off the renamed _binary_zones_page_html_gz_start/_end
# symbols.
#
# execute_process (configure-time), not add_custom_command (build-time):
# idf_component_register's EMBED_TXTFILES list below is evaluated as soon as
# CMake reaches that call, so the .gz files must already exist on disk by
# then -- a build-time custom command's COMMAND wouldn't have run yet at
# that point. set_property(... CMAKE_CONFIGURE_DEPENDS ...) below makes
# editing any one of these source files force a CMake reconfigure (and so a
# re-gzip) on the next build, so this doesn't silently embed a stale
# compressed blob after a source edit the way a one-shot configure-time step
# alone would.
#
# Windows dev-machine compatibility (this project is developed on Windows,
# per README.md): find_package(Python3) + a one-line `python -c` gzip
# invocation avoids depending on a Unix `gzip` binary being on PATH, which
# this project's dev machines cannot assume.
# CMAKE_BUILD_EARLY_EXPANSION guard: ESP-IDF's build system re-includes every
# component's CMakeLists.txt in a stripped-down early pass just to harvest
# REQUIRES/PRIV_REQUIRES (component_get_requirements.cmake sets this variable
# true for that pass only -- see esp-idf/tools/cmake/scripts/
# component_get_requirements.cmake). Discovered the hard way, 2026-08-18: with
# no guard, the execute_process() gzip calls below ran during THAT early pass
# too, where CMAKE_CURRENT_SOURCE_DIR does not yet point at this directory --
# every gzip call failed with "FileNotFoundError" reading a nonexistent
# App/drivers/build/<page>.html. Top-level code in a component's CMakeLists.txt
# always runs twice for this reason; anything with a real side effect (a file
# write, a subprocess, anything not just declaring SRCS/REQUIRES) has to check
# this guard or it silently runs in a context where its own path assumptions
# are wrong.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(KILNCTL_GZIP_ASSETS
        "wifi_provision_page.html" "main_page.html"
        "profiles_page.html" "readiness_page.html" "theme.css"
        "ota_page.html" "nav.js" "app.js"
        "diagnostics_page.html" "safety_page.html"
        "settings_page.html" "backup_page.html"
        "settings_display_page.html"
        "safety_commissioning_page.html"
        "safety_config_page.html"
        "zones_page.html")
    set(KILNCTL_GZIP_ASSET_FILES "")
    foreach(asset ${KILNCTL_GZIP_ASSETS})
        set(kilnctl_gz_src "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")
        set(kilnctl_gz_out "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c
                "import gzip, shutil, sys; shutil.copyfileobj(open(sys.argv[1], 'rb'), gzip.open(sys.argv[2], 'wb', 9))"
                "${kilnctl_gz_src}" "${kilnctl_gz_out}"
            RESULT_VARIABLE KILNCTL_GZIP_RESULT
        )
        if(NOT KILNCTL_GZIP_RESULT EQUAL 0)
            message(FATAL_ERROR "App/drivers/CMakeLists.txt: failed to gzip ${asset} for EMBED_TXTFILES")
        endif()
        list(APPEND KILNCTL_GZIP_ASSET_FILES "${kilnctl_gz_out}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kilnctl_gz_src}")
    endforeach()
endif()

# esp_driver_tsens (driver/temperature_sensor.h, used by board_temps.c for
# the ESP32-S3 internal die-temperature sensor) is named on the assumption
# that this IDF version keeps the same per-peripheral esp_driver_* component
# split as esp_driver_i2c/esp_driver_spi/etc. above -- NOT confirmed against
# an installed IDF v6.0.2 tree from inside this repo (no managed_components/
# manifest or installed SDK checkout was reachable to grep). If the build
# reports this component unknown, check
# $IDF_PATH/components/esp_driver_tsens (or wherever temperature_sensor.h
# actually lives in the installed toolchain) and fix the name here.
idf_component_register(SRCS "MAX31856.c"
                       "max31856_codec.c"
                       "SX1509.c"
                       "kiln_io.c"
                       "kiln_io_owner.c"
                       "thermo_owner.c"
                       "owner_slot_pool.c"
                       "panel_spi.c"
                       "panel_codec.c"
                       "st7796_panel.c"
                       "lvgl_port.c"
                       "kiln_ui.c"
                       "ui_page_home.c"
                       "ui_page_home_graph.c"
                       "ui_page_config.c"
                       "ui_page_temperature.c"
                       "ui_page_network.c"
                       "ui_page_network_manage.c"
                       "lvgl_mem_psram.c"
                       "ui_page_diagnostics.c"
                       "ui_page_touch_cal.c"
                       "ui_page_touch_test.c"
                       "ui_page_profiles.c"
                       "ui_page_profiles_mine.c"
                       "ui_page_profiles_family.c"
                       "ui_page_profiles_builtin_list.c"
                       "ui_page_profile_detail.c"
                       "ui_page_profile_segments.c"
                       "ui_page_profile_builder_zones.c"
                       "ui_page_profile_builder_segment.c"
                       "ui_page_profile_builder_review.c"
                       "ui_confirm.c"
                       "ui_num_pad.c"
                       "touch_cal_store.c"
                       "unit_pref.c"
                       "ui_theme.c"
                       "ui_topbar.c"
                       "wifi_status_ui.c"
                       "NS2009.c"
                       "touch_dev.c"
                       # FT6336U.c: FT6336U capacitive touch controller driver
                       # for the not-yet-connected MSP4031 module -- see its
                       # own header comment. Compiled in (so it stays buildable
                       # and its unvalidated register logic stays host-testable
                       # via touch_dev.c's pure mapping split) but never
                       # instantiated anywhere: dead code at runtime on every
                       # board that exists today, which is still the NS2009.
                       "FT6336U.c"
                       "screen_idle.c"
                       "safety_link.c"
                       "safety_link_frame.c"
                       "safety_link_frames.c"
                       "safety_link_inbox.c"
                       "safety_link_poll.c"
                       "safety_link_commands.c"
                       "safety_link_payload.c"
                       "safety_trip_decision.c"
                       "i2c_scan.c"
                       "uart_log_bridge.c"
                       "uart_bridge.c"
                       "uart_bridge_thermo.c"
                       "uart_bridge_io.c"
                       "uart_bridge_touch.c"
                       "uart_bridge_ui_test.c"
                       "uart_bridge_safety.c"
                       "uart_bridge_system.c"
                       "uart_bridge_info.c"
                       "uart_bridge_ext.c"
                       "telemetry_format.c"
                       "telemetry_log.c"
                       "log_store.c"
                       "log_store_mount.c"
                       "log_http.c"
                       "relay_authority.c"
                       "relay_cycles.c"
                       "run_state.c"
                       "boot_guard.c"
                       "boot_button.c"
                       "danger_mode.c"
                       "watchdog_cfg.c"
                       "rtc_watchdog.c"
                       "crash_report.c"
                       "nvs_report.c"
                       "factory_reset.c"
                       "dashboard_http.c"
                       "dashboard_json.c"
                       "diagnostics_http.c"
                       "settings_http.c"
                       "time_sync.c"
                       "time_sync_tz.c"
                       "backup_http.c"
                       "ota_auth.c"
                       "ota_interlock.c"
                       "heat_interlock.c"
                       "ota_record.c"
                       "ota_http.c"
                       "ota_http_util.c"
                       "ota_pico_relay.c"
                       "wifi_prov.c"
                       "wifi_provision_http.c"
                       "web_encoding.c"
                       "zones_http.c"
                       # zones_http.c split 2026-09-01 (it had grown to 5628
                       # lines, the largest file in the firmware) along its
                       # natural seams -- see zones_http_internal.h's header
                       # comment for the full seam rationale. zones_http.c
                       # itself keeps hw wiring, page/URI registration, and
                       # the small sweep HTTP wrappers.
                       "zones_config_store.c"
                       "zones_config_accessors.c"
                       "zones_http_handlers.c"
                       "zones_current_sweep_engine.c"
                       "zones_current_sweep_task.c"
                       "zones_config_json.c"
                       "kiln_cfg_store.c"
                       "kiln_cfg_http.c"
                       "safety_cfg_store.c"
                       "safety_cfg_http.c"
                       # rules_http.c/rules_eval.c/rules_task.c (the "Relays &
                       # Rules" engine) were deleted 2026-08-27: relay/IO
                       # control moved into firing profiles as segments (see
                       # profile_executor.c's io_seg_* machinery) per the
                       # owner's request. See relay_authority.h's
                       # RELAY_OWNER_RULE comment for why that enumerator is
                       # still here even though nothing claims it any more.
                       "profiles_http.c"
                       # profiles_builtin.c is hand-written and #includes the
                       # machine-generated profiles_builtin_table.inc, which is
                       # NOT listed here (it is not a translation unit).
                       "profiles_builtin.c"
                       "profile_feasibility.c"
                       "readiness_http.c"
                       "board_temps.c"
                       "pid.c"
                       "pid_fuzzy.c"
                       "thermal_guard.c"
                       "thermo_combine.c"
                       "heat_enable.c"
                       "heater_output.c"
                       "zone_coupling_solve.c"
                       "profile_executor.c"
                       # profile_executor.c split 2026-09-01 ("files over
                       # 1500 lines should be broken up where it makes
                       # sense" -- it had grown to 4962 lines, the last of
                       # that day's four biggest firmware files). See
                       # profile_executor_internal.h's own doc comment for
                       # the seam rationale.
                       "profile_executor_feedforward.c"
                       "profile_executor_firing_stats.c"
                       "profile_executor_relay_io.c"
                       "profile_executor_config_reload.c"
                       "profile_executor_pid_tick.c"
                       "profile_executor_start.c"
                       "profile_executor_run.c"
                       "profile_executor_status.c"
                       "adaptive_tune.c"
                       "adaptive_tune_http.c"
                       "ramp_ident.c"
                       "pid_autotune.c"
                       "autotune_engine.c"
                       "gpio_probe.c"
                       "stack_margin.c"
                       "espInterfaces/esp_spi_owner.c"
                       "espInterfaces/i2c_owner.c"
                       "espInterfaces/uart_owner.c"
                       "espInterfaces/uart_protocol.c"
                       ${SIM_SRCS}
                       INCLUDE_DIRS "." "espInterfaces"
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
                       EMBED_TXTFILES ${KILNCTL_GZIP_ASSET_FILES}
                       REQUIRES esp_driver_i2c esp_driver_spi esp_driver_uart esp_driver_gpio
                                esp_driver_tsens
                                esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer
                                esp_partition kilnlink mbedtls lwip app_update esp_rom lvgl mdns
                                esp_hw_support esp_hal_wdt
                                espcoredump
                       PRIV_REQUIRES spi_flash spiffs)

# Firmware version info (git commit/dirty flag/build timestamp), regenerated
# on *every* build invocation -- not just when CMake reconfigures -- so it
# can't go stale. A plain add_custom_command keyed on file dependencies
# would only rerun when one of its DEPENDS changed; add_custom_target has no
# such tracking and Ninja/Make always re-executes it, which is what we want
# here. uart_bridge.c includes the generated build_info.h.
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE FW_GIT_TOPLEVEL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT FW_GIT_TOPLEVEL)
    set(FW_GIT_TOPLEVEL ${CMAKE_SOURCE_DIR})
endif()

set(BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
add_custom_target(drivers_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${BUILD_INFO_HEADER}
        -DPROJECT_ROOT=${FW_GIT_TOPLEVEL}
        -DTHIS_PROJECT_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_info.cmake
    BYPRODUCTS ${BUILD_INFO_HEADER}
    COMMENT "Regenerating build_info.h (git commit/dirty/build time)"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} drivers_build_info)

# lvgl_mem_psram.c implements LVGL's LV_STDLIB_CUSTOM allocator hooks
# (lv_malloc_core/lv_free_core/...). liblvgl.a references those symbols but is
# linked BEFORE this component's archive, so with plain archive semantics the
# linker has already passed them by and fails with "undefined reference to
# lv_malloc_core" pointing inside liblvgl.a -- which looks like an LVGL fault
# rather than a link-order one.
#
# idf_component_add_link_dependency(FROM lvgl) tells the build to link lvgl
# against this component, which is exactly the supported way to satisfy a
# library's externally-implemented hooks. Adding lvgl to this component's
# REQUIRES would not help: that is the dependency that already exists, and in
# the wrong direction for this.
idf_component_add_link_dependency(FROM lvgl)
