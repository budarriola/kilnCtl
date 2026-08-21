# SimFW -- kiln simulator / unit-test fixture firmware, RP2040, pico-sdk +
# FreeRTOS-Kernel (SMP port). docs/DESIGN_NOTES.md section 4 is the authority for
# every choice below (toolchain, RTOS, warnings level, single-image); if
# this file and that doc disagree, the doc wins.
#
# Adapted from ../SaftyFW/CMakeLists.txt, which is the template DESIGN_NOTES.md
# section 4.4 names ("SaftyFW's CMake + pico_sdk + FreeRTOS import layout is
# the template"). The one deliberate structural difference: SimFW is a
# plain, SINGLE-IMAGE target. There is no A/B bootloader slot scheme here --
# SaftyFW's SaftyFW_slotA/SaftyFW_slotB machinery exists because that board
# needs field OTA updates over an isolated link with no physical access;
# SimFW is a bench tool that sits on a desk next to whoever is using it, so
# BOOTSEL drag-and-drop or SWD (docs/DESIGN_NOTES.md section 3.1/9) is the whole
# flashing story and a second linked image buys nothing.
#
# Requires, on the machine building this (none of these are vendored into
# the repo -- same as SaftyFW):
#   PICO_SDK_PATH        -- github.com/raspberrypi/pico-sdk, 2.x
#   FREERTOS_KERNEL_PATH  -- github.com/FreeRTOS/FreeRTOS-Kernel, the RP2040
#                            SMP port lives at
#                            portable/ThirdParty/GCC/RP2040 in that checkout
#   arm-none-eabi-gcc on PATH, or PICO_TOOLCHAIN_PATH pointing at its bin dir
#
# Example (PowerShell), toolchain installed via winget, SDK/kernel cloned to
# C:\pico-tools:
#   $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
#   $env:FREERTOS_KERNEL_PATH = "C:\pico-tools\FreeRTOS-Kernel"
#   cmake -G Ninja -B build -DPICO_BOARD=pico ^
#     "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
#   cmake --build build
cmake_minimum_required(VERSION 3.16)

# Must be included before project() -- locates PICO_SDK_PATH and defines
# pico_sdk_init(), per pico-sdk's own convention (see the copy's header
# comment in pico_sdk_import.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(SimFW C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Forces TinyUSB's own CFG_TUSB_OS compile definition to OPT_OS_FREERTOS
# (src/tusb_config.h's header comment explains why) instead of
# lib/tinyusb/hw/bsp/rp2040/family.cmake's own default (OPT_OS_PICO, a bare
# WFE spin that looks READY-not-BLOCKED to the FreeRTOS scheduler and would
# starve every lower-priority task on usb_owner's core). Must be set before
# pico_sdk_init() below, which is what pulls in
# src/rp2_common/tinyusb/CMakeLists.txt -> family.cmake and reads this
# variable ("if (NOT TINYUSB_OPT_OS) set(TINYUSB_OPT_OS OPT_OS_PICO)").
set(TINYUSB_OPT_OS OPT_OS_FREERTOS)

pico_sdk_init()

# FreeRTOS-Kernel's RP2040 SMP port. May be included after pico_sdk_init()
# (pico-sdk > 1.3.1, which 2.x satisfies) -- see
# FreeRTOS_Kernel_import.cmake's own comment.
include(${CMAKE_CURRENT_LIST_DIR}/FreeRTOS_Kernel_import.cmake)

# `benchproto` -- SimFW's USB control protocol core (docs/DESIGN_NOTES.md
# sections 4.4/5, docs/PLAN.md section 12, firmware/CommonFW/docs/BENCHPROTO.md): framing, CRC, reliability/
# retry/dedup, task registration, now lifted into CommonFW as its own
# library. Pulled in the same mechanism SaftyFW uses for kilnlink
# (../SaftyFW/CMakeLists.txt: `add_subdirectory(.../CommonFW kilnlink)`) --
# CommonFW/CMakeLists.txt defines both `kilnlink` and `benchproto` as
# sibling targets from one project; only `benchproto` is linked below
# (target_link_libraries), since SimFW still never speaks on the kilnlink
# wire (the safety link's compatibility floor, DESIGN_NOTES.md section 4.4 --
# unchanged from this file's previous header comment on that point).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../CommonFW benchproto)

# Regenerates build_info.h (git commit/dirty/build time) on every build
# invocation, same mechanism as ../KilnFW/App/drivers/gen_build_info.cmake
# (see this project's own gen_build_info.cmake for the adaptation notes).
# cmd_task.c's SYS GET_VERSION/GET_CAPS handlers include the generated
# header. add_custom_target (not add_custom_command) has no dependency
# tracking, so Ninja/Make always reruns it rather than only when a tracked
# input changed -- deliberate, so the header can never go stale between
# builds that don't touch this project's own sources.
set(SIMFW_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
add_custom_target(simfw_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${SIMFW_BUILD_INFO_HEADER}
        -DPROJECT_ROOT=${CMAKE_CURRENT_LIST_DIR}/../..
        -DTHIS_PROJECT_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/gen_build_info.cmake
    BYPRODUCTS ${SIMFW_BUILD_INFO_HEADER}
    COMMENT "Regenerating build_info.h (git commit/dirty/build time)"
    VERBATIM
)
#
# hardware_pio/hardware_irq ARE linked below: spi_emu_a/b now have real
# bodies (src/drivers/max31856_pio_engine.{c,h} + src/drivers/
# max31856_spi_slave.pio, PIO0/PIO1 SPI slave emulation of the MAX31856
# register file, src/sim/max31856_regs.{c,h}), docs/DESIGN_NOTES.md section 3.2.1/
# M-A's proof-of-concept milestone -- see max31856_spi_slave.pio's header
# comment for the build-verified-not-hardware-timing-verified caveat.
# hardware_i2c IS linked below too: i2c_owner now has a real body
# (src/drivers/mcp23017.{c,h} + I2C0 init), docs/DESIGN_NOTES.md section 4.1's
# relay-sense/E-stop/DUT-power milestone.
# hardware_pwm/hardware_dma ARE linked below too: wave_owner now has a real
# body (src/drivers/ct_wave_pwm.{c,h} + 3-channel PWM/DMA CT synthesis),
# docs/DESIGN_NOTES.md section 4.1/3.3's CT-waveform milestone -- see
# src/drivers/ct_wave_pwm.c's header comment for the GPIO/carrier/sample-rate
# arithmetic. src/sim/sine_synth.c is linked alongside it: wave_owner.c calls
# into it (256-entry table, distortion, zero-crossing detection) rather than
# reimplementing any of that pure math, per docs/DESIGN_NOTES.md section 3.3 and
# docs/PLAN.md section 13's "pure, host-testable" sim/ modules being the single source of truth.
# src/sim/ct_calibration.c is linked for the same reason: wave_owner.c's
# ct_wave_amps_to_pwm_scale() delegates the per-channel
# clamp(gain*amps + offset, 0, 1) mapping to it rather than doing that
# arithmetic inline, so the code the firmware runs is the code the MSVC host
# tests exercise. Its constants live in the generated
# src/sim/ct_calibration_defaults.h (tools/gen_ct_cal_table.py) and are
# all-uncalibrated/identity today -- no CT hardware, no calibration run yet.
# tinyusb_device/pico_unique_id (below) back usb_owner's real body: native
# USB CDC framing/dispatch over `benchproto` (docs/DESIGN_NOTES.md section 4.1's
# usb_owner/cmd_task rows) -- src/tusb_config.h and
# src/tasks/usb_descriptors.c are usb_owner's own descriptor/config half, see
# usb_owner.h's header comment. Add each remaining library in the same
# change that gives its owning task a real body, so an unused link never
# masks a missing one.
set(SIMFW_APP_SOURCES
    src/main.c
    src/tasks/usb_owner.c
    src/tasks/usb_descriptors.c
    src/tasks/cmd_task.c
    src/tasks/spi_emu_a.c
    src/tasks/spi_emu_b.c
    src/drivers/max31856_pio_engine.c
    src/sim/max31856_regs.c
    src/sim/max31856_resp_image.c
    src/tasks/sim_engine.c
    src/tasks/fault_sched.c
    src/tasks/wave_owner.c
    src/tasks/i2c_owner.c
    src/tasks/telemetry.c
    src/tasks/log_task.c
    src/sim/tc_fault_state.c
    src/sim/thermal_model.c
    src/sim/fault_engine.c
    src/drivers/mcp23017.c
    src/drivers/ct_wave_pwm.c
    src/drivers/simfw_fatal.c
    src/sim/sine_synth.c
    src/sim/ct_calibration.c
)

add_executable(SimFW ${SIMFW_APP_SOURCES})

add_dependencies(SimFW simfw_build_info)

# PIO SPI slave engines (docs/DESIGN_NOTES.md section 3.2.1, M-A -- the project's
# highest-risk item): src/drivers/max31856_spi_slave.pio holds the RX/TX
# state machine programs spi_emu_a/b's real bodies load via
# src/drivers/max31856_pio_engine.c. pico_generate_pio_header() runs pioasm
# (a HOST tool, built from pico-sdk's tools/pioasm against whatever host
# C++ compiler CMake finds -- NOT arm-none-eabi-gcc) as part of the build and
# emits max31856_spi_slave.pio.h into a generated-headers directory it also
# adds to SimFW's include path automatically -- no manual
# target_include_directories entry needed for that generated header.
pico_generate_pio_header(SimFW ${CMAKE_CURRENT_LIST_DIR}/src/drivers/max31856_spi_slave.pio)

target_include_directories(SimFW PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(SimFW PRIVATE -Wall -Wextra -Werror)

target_link_libraries(SimFW PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_pwm
    hardware_dma
    hardware_pio
    hardware_irq
    pico_multicore
    pico_unique_id
    tinyusb_device
    benchproto
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

# No pico-sdk stdio backend enabled: native USB is usb_owner's own TinyUSB
# CDC link (src/tasks/usb_owner.c, src/tasks/usb_descriptors.c) -- pico-sdk's
# separate stdio_usb wrapper would fight it for the same peripheral, so it
# stays off, matching the single-owner-per-peripheral doctrine every other
# task in this project follows. There is no UART0 debug-probe bridge code in
# this pass either (docs/DESIGN_NOTES.md section 3.6: "Debug UART (to Debug Probe) |
# 2 | same bench pattern as SaftyFW" -- reserved, not wired). Matches
# SaftyFW's own reasoning for disabling both by default.
pico_enable_stdio_usb(SimFW 0)
pico_enable_stdio_uart(SimFW 0)

# pico_add_extra_outputs(SimFW) would generate .uf2/.bin/.hex/.dis and embed
# the RP2040 boot-stage2 checksum via picotool -- left disabled on this
# machine for the same reason SaftyFW's CMakeLists.txt gives: picotool has
# no prebuilt binary for this pico-sdk version and building it from source
# needs a host C/C++ compiler that is not set up here (only arm-none-eabi-gcc
# is configured). The plain .elf below is what OpenOCD/SWD flashes anyway;
# BOOTSEL drag-and-drop (DESIGN_NOTES.md section 3.1/9) needs a .uf2, so re-enable
# this line once picotool is available if that flashing path is wanted.
