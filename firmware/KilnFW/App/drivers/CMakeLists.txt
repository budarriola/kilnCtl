# The simulated-plant backend and the thermal model it drives are only
# compiled into a KILNCTL_SIM_PLANT build. sim_plant.c deliberately lives in
# App/test/ and is shared with the host test binary rather than copied here --
# one model, so a guard trip on the host means the same thing on the board.
set(SIM_SRCS "")
if(CONFIG_KILNCTL_SIM_PLANT)
    set(SIM_SRCS "sim_backend.c" "../test/sim_plant.c")
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
                       "SX1509.c"
                       "kiln_io.c"
                       "ILI9488.c"
                       "lvgl_port.c"
                       "kiln_ui.c"
                       "ui_page_home.c"
                       "ui_page_config.c"
                       "ui_page_temperature.c"
                       "ui_theme.c"
                       "NS2009.c"
                       "screen_idle.c"
                       "safety_link.c"
                       "i2c_scan.c"
                       "uart_log_bridge.c"
                       "uart_bridge.c"
                       "uart_bridge_ext.c"
                       "relay_authority.c"
                       "relay_cycles.c"
                       "run_state.c"
                       "nvs_report.c"
                       "factory_reset.c"
                       "dashboard_http.c"
                       "ota_auth.c"
                       "ota_interlock.c"
                       "ota_record.c"
                       "ota_http.c"
                       "ota_pico_relay.c"
                       "wifi_prov.c"
                       "wifi_provision_http.c"
                       "zones_http.c"
                       "rules_http.c"
                       "profiles_http.c"
                       "readiness_http.c"
                       "board_temps.c"
                       "pid.c"
                       "thermal_guard.c"
                       "thermo_combine.c"
                       "heater_output.c"
                       "profile_executor.c"
                       "pid_autotune.c"
                       "autotune_engine.c"
                       "gpio_probe.c"
                       "espInterfaces/esp_spi_owner.c"
                       "espInterfaces/i2c_owner.c"
                       "espInterfaces/uart_owner.c"
                       "espInterfaces/uart_protocol.c"
                       ${SIM_SRCS}
                       INCLUDE_DIRS "." "espInterfaces"
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
                       EMBED_TXTFILES "wifi_provision_page.html" "main_page.html"
                                      "zones_page.html" "rules_page.html" "profiles_page.html"
                                      "readiness_page.html"
                       REQUIRES esp_driver_i2c esp_driver_spi esp_driver_uart esp_driver_gpio
                                esp_driver_tsens
                                esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer
                                esp_partition kilnlink mbedtls lwip app_update esp_rom lvgl)

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
