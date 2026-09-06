# ESP-IDF component for the ESP32-S3 (KilnFW) hardware-abstraction backends.
#
# HAL Phase 1a (move-only, docs/HW_ABSTRACTION_PLAN.md): this component's
# SRCS list is the espInterfaces move set ONLY -- esp_spi_owner.c/h +
# owner_slot_pool.c/h (spi/), i2c_owner.c/h (i2c/), uart_owner.c/h +
# uart_protocol.c/h (uart/) -- moved byte-identical out of
# firmware/KilnFW/App/drivers/owners/. The existing hal_*_esp.c backend
# bodies under spi/, i2c/, uart/ (landed ahead of Phase 1a per each
# directory's own file) have no consumers yet and are deliberately NOT
# added to SRCS here; that is Phase 1b.
#
# HAL Phase 1b addition (2026-09-05): uart/hal_uart_esp.c is now a real
# consumer of uart_protocol.c's frame_and_send() (routed through
# hal_uart_attach()/hal_uart_send_blocking() instead of uart_owner_transfer,
# see docs/HW_ABSTRACTION_PLAN.md "hal_uart -- two primitives, ESP backend
# unchanged" and uart_protocol.c's own comment at that call site), so it -- and
# the common/ hal_status.c + esp/common/hal_esp_common.c it depends on for
# hal_status_to_name()/hal_esp_err_to_status() -- move from "no consumers yet"
# to SRCS here. spi/ and i2c/'s hal_*_esp.c backends remain unwired (still no
# consumer) and are deliberately left out.
#
# Component-cycle note: owner_slot_pool.h is also #include'd (bare) by two
# files that stay behind in firmware/KilnFW/App/drivers/owners/ --
# kiln_io_owner.c and thermo_owner.c -- so this component's INCLUDE_DIRS
# must reach the drivers component (hence drivers/CMakeLists.txt now
# REQUIRES esp), while esp_spi_owner.c itself still needs
# drivers/common/stack_margin.h (hence PRIV_REQUIRES drivers below). This is
# the mutual drivers<->esp dependency the plan calls out
# ("owner_slot_pool.c/h ... must move with esp_spi_owner or the component
# gains a dependency cycle on drivers", HW_ABSTRACTION_PLAN.md's migration
# inventories section). Moving owner_slot_pool.c/h does not eliminate the
# cycle by itself, since kiln_io_owner.c/thermo_owner.c still need the
# header; it is kept private-only on both sides (PRIV_REQUIRES here,
# ordinary REQUIRES on the drivers side only because drivers' *headers*
# forward the include to its own consumers, e.g. main_internal.h) so ESP-IDF's
# component graph resolves it as two private edges into a strongly connected
# pair rather than a public dependency cycle. Verified 2026-09-05 by a clean
# `build_kilnfw` -- see the plan's Phase 1a entry for confirmation this
# actually builds.
idf_component_register(SRCS "spi/esp_spi_owner.c"
                             "spi/owner_slot_pool.c"
                             "i2c/i2c_owner.c"
                             "uart/uart_owner.c"
                             "uart/uart_protocol.c"
                             "uart/hal_uart_esp.c"
                             "common/hal_esp_common.c"
                             "../common/hal_status.c"
                       INCLUDE_DIRS "spi" "i2c" "uart" "common" "../interface"
                       REQUIRES esp_driver_spi esp_driver_i2c esp_driver_uart esp_driver_gpio
                                freertos esp_timer kilnlink
                       PRIV_REQUIRES drivers)
