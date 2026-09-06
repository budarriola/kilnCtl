# Thin ESP-IDF component wrapper for firmware/hwAbstraction/esp/.
#
# ESP-IDF derives a component's name from its directory name, and that name
# lives in ESP-IDF's flat global namespace (all components, from every
# project and every managed dependency, share one component-name space).
# The real sources stay at firmware/hwAbstraction/esp/ (shared, non-ESP-IDF
# tree used elsewhere too -- see docs/HW_ABSTRACTION_PLAN.md and
# firmware/hwAbstraction/README.md); this directory exists only so the
# component registers under the name "hwabstraction_esp" instead of the bare,
# collision-prone "esp" (asymmetric with SaftyFW's "hwabstraction_pico"
# static library target). Do not add sources here -- add them under
# ../../esp/ and list them below with a "../../esp/" prefix.
#
# See firmware/hwAbstraction/esp/CMakeLists.txt for the history of this SRCS
# list (HAL Phase 1a/1b) -- kept here verbatim, paths adjusted for this
# component's location one directory deeper.
idf_component_register(SRCS "../../esp/spi/esp_spi_owner.c"
                             "../../esp/spi/owner_slot_pool.c"
                             "../../esp/spi/hal_spi_esp.c"
                             "../../esp/i2c/i2c_owner.c"
                             "../../esp/i2c/hal_i2c_esp.c"
                             "../../esp/uart/uart_owner.c"
                             "../../esp/uart/uart_protocol.c"
                             "../../esp/uart/hal_uart_esp.c"
                             "../../esp/common/hal_esp_common.c"
                             "../../common/hal_status.c"
                       INCLUDE_DIRS "../../esp/spi" "../../esp/i2c" "../../esp/uart"
                                    "../../esp/common" "../../interface"
                       REQUIRES esp_driver_spi esp_driver_i2c esp_driver_uart esp_driver_gpio
                                freertos esp_timer kilnlink
                       PRIV_REQUIRES drivers)
