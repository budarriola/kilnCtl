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

# Debug UART pin/instance override -- MUST be set before pico_sdk_init()
# below, which is what pulls in pico-sdk's board header (boards/pico.h) and
# compiles pico_stdio_uart's own sources (src/rp2_common/pico_stdio_uart/
# stdio_uart.c) against whatever PICO_DEFAULT_UART* the preprocessor sees at
# THAT compile, not just SimFW's own translation units -- a
# target_compile_definitions(SimFW ...) after pico_sdk_init() would not
# reach that library. docs/HARDWARE.md section 1's footnote is the
# authority: the bench debug UART is physically wired to GP16/GP17 (UART0),
# same convention as ../SaftyFW/src/tasks/console_uart.c, discovered AFTER
# this document's own earlier GPIO0/GPIO1 guess -- "if the physical harness
# disagrees with this file, the harness wins" (that doc's own header). For
# board=pico, GPIO0/1 would otherwise be the SDK's silent default, which is
# exactly why this is set explicitly here rather than left implicit: a
# future PICO_BOARD change must not silently move the UART off the wire
# that is actually populated. 115200 8N1, no flow control, no inversion --
# matches console_uart.c's own uart_set_hw_flow(false, false)/
# uart_set_format(8, 1, UART_PARITY_NONE) defaults (pico-sdk's uart_init()
# already defaults to 8N1/no-flow, so pico_stdio_uart's own init needs no
# extra calls to match).
add_compile_definitions(
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=16
    PICO_DEFAULT_UART_RX_PIN=17
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

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
# sections 4.4/5, docs/DESIGN_NOTES.md section 12, firmware/CommonFW/docs/BENCHPROTO.md): framing, CRC, reliability/
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
# hardware_dma/hardware_pio ARE linked below too: wave_owner now drives CT
# synthesis through src/drivers/ct_wave_i2s.{c,h} (PIO+DMA I2S MASTER
# transport for 2x UDA1334A stereo DAC modules) instead of the retired
# PWM+RC path (src/drivers/ct_wave_pwm.{c,h}, deleted -- docs/DESIGN_NOTES.md
# section 3.3's 2026-08-23 decision). src/sim/ct_i2s_gen.{c,h} is the pure,
# host-testable sample-block generator wave_owner.c drives it with (see that
# header for the explicit zero-crossing gate the 16 kHz path needs, unlike
# the old PWM path's "one DMA table = one cycle" trick); src/sim/sine_synth.c
# is linked alongside it since ct_i2s_gen.c calls into it (256-entry table,
# distortion, zero-crossing detection) rather than reimplementing any of that
# pure math, per docs/DESIGN_NOTES.md section 3.3 and docs/PLAN.md section
# 13's "pure, host-testable" sim/ modules being the single source of truth.
# src/sim/ct_calibration.c is linked for the same reason: ct_i2s_gen.c's
# per-sample amplitude computation delegates the per-channel
# clamp(gain*amps + offset, 0, 1) mapping to it rather than doing that
# arithmetic inline, so the code the firmware runs is the code the MSVC host
# tests exercise. Its constants live in the generated
# src/sim/ct_calibration_defaults.h (tools/gen_ct_cal_table.py) and are
# all-uncalibrated/identity today -- no CT hardware, no calibration run yet.
# ct_wave_i2s.c/.pio cannot actually run yet: PIO1's program memory is
# nearly full (bus B's RX+TX_b programs already occupy 29 of 32 words) and
# the 8-word ct_wave_i2s_out program cannot fit in the 3 that remain -- see
# ct_wave_i2s.h's header and docs/HARDWARE.md section 1b.7. wave_owner_start()
# calls ct_wave_i2s_init() unconditionally and lets that failure reach
# simfw_fatal() rather than hiding it behind a fallback, so the fixture halts
# loudly instead of booting with a CT path that looks alive but is silent.
# tinyusb_device/pico_unique_id (below) back usb_owner's real body: native
# USB CDC framing/dispatch over `benchproto` (docs/DESIGN_NOTES.md section 4.1's
# usb_owner/cmd_task rows) -- src/tusb_config.h and
# src/tasks/usb_descriptors.c are usb_owner's own descriptor/config half, see
# usb_owner.h's header comment. Add each remaining library in the same
# change that gives its owning task a real body, so an unused link never
# masks a missing one.
# src/drivers/console_sink.{c,h} (2026-08-23, "give SimFW a second USB CDC
# interface, dedicated to console/log output"): a small pico-sdk
# stdio_driver_t that mirrors printf()/log output to usb_owner's new CDC1
# console sink alongside the existing UART0 output -- see that file's header
# comment and usb_owner.h's "Dual CDC" section. Links no new library (only
# pico_stdio, already pulled in transitively via pico_stdlib below).
# src/tasks/safe_reboot.c (gap-closure pass, "flash over USB without
# BOOTSEL"): the one place allowed to call pico-sdk's reset_usb_boot()
# (pico/bootrom.h) -- both cmd_task.c's SIMFW_CMD_SYS_REBOOT_BOOTLOADER
# handler and usb_owner.c's 1200-baud tud_cdc_line_coding_cb()/
# tud_cdc_line_state_cb() touch-convention callbacks delegate to it, so the
# safe-state-then-reboot sequence exists exactly once. See that file's
# header comment for why this is more than an ordinary reboot (the
# MCP23017 I2C expanders do not reset with the RP2040).
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
    src/tasks/safe_reboot.c
    src/tasks/telemetry.c
    src/tasks/log_task.c
    src/sim/tc_fault_state.c
    src/sim/thermal_model.c
    src/sim/fault_engine.c
    src/drivers/mcp23017.c
    src/drivers/ct_wave_i2s.c
    src/drivers/simfw_fatal.c
    src/drivers/console_sink.c
    src/sim/sine_synth.c
    src/sim/ct_calibration.c
    src/sim/ct_i2s_gen.c
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

# ct_wave_i2s.pio: PIO I2S master transport for 2x UDA1334A DAC modules
# (src/drivers/ct_wave_i2s.{c,h}), wave_owner's CT synthesis backend now that
# ct_wave_pwm.c's PWM+RC path is retired -- see that header for the
# PIO1-program-memory conflict this driver has with bus B (max31856_pio_engine.c):
# the 8-word ct_wave_i2s_out program cannot fit in PIO1's 3 remaining free
# words, so ct_wave_i2s_init() (called unconditionally from wave_owner_start())
# is expected to reach simfw_fatal() on the current build. See
# docs/HARDWARE.md section 1b.7 and docs/PLAN.md's open items.
pico_generate_pio_header(SimFW ${CMAKE_CURRENT_LIST_DIR}/src/drivers/ct_wave_i2s.pio)

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
    hardware_dma
    hardware_pio
    hardware_irq
    pico_multicore
    pico_unique_id
    # pico_bootrom's actual implementation (bootrom.c, the ROM function
    # lookup reset_usb_boot()/rom_reset_usb_boot() needs) is a separate
    # INTERFACE target from pico_bootrom_headers -- pico_crt0 (pulled in via
    # pico_stdlib) links only the headers, not this, so src/tasks/
    # safe_reboot.c's call needs it linked explicitly here.
    pico_bootrom
    tinyusb_device
    benchproto
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

# stdio_usb stays OFF: native USB is usb_owner's own TinyUSB CDC link
# (src/tasks/usb_owner.c, src/tasks/usb_descriptors.c) -- pico-sdk's separate
# stdio_usb wrapper would fight it for the same peripheral, so it stays off,
# matching the single-owner-per-peripheral doctrine every other task in this
# project follows. USB is also the very thing broken on the bench right now
# (no enumeration at all), so a UART-independent trace path matters more
# than ever here.
#
# stdio_uart is ON, 2026-08-23 (bench boot-debug pass): the fixture is
# showing up dead on the bench (dark LED, no USB) and the boot-stage LED
# beacon (src/main.c's simfw_boot_beacon()) shows nothing either, so a trace
# path that does not depend on the LED or on TinyUSB is needed to see how
# far boot gets. This is the pico-sdk stdio backend, not a hand-rolled
# owner file, because it needs nothing this project doesn't already get for
# free from pico_stdlib: no ring buffer, no IRQ, no task -- printf() blocks
# on uart_write_blocking() under the hood, same as SaftyFW's
# console_uart.c, which is fine and even desirable for boot tracing (nothing
# is dropped, unlike a non-blocking log queue that could itself hide a
# hang). See the PICO_DEFAULT_UART* compile definitions above this project's
# project() call for the pin/instance/baud choice.
pico_enable_stdio_usb(SimFW 0)
pico_enable_stdio_uart(SimFW 1)

# pico_add_extra_outputs(SimFW) would generate .uf2/.bin/.hex/.dis and embed
# the RP2040 boot-stage2 checksum via picotool -- left disabled on this
# machine for the same reason SaftyFW's CMakeLists.txt gives: picotool has
# no prebuilt binary for this pico-sdk version and building it from source
# needs a host C/C++ compiler that is not set up here (only arm-none-eabi-gcc
# is configured). Instead, tools/elf2uf2.py (Python stdlib + objcopy only,
# no picotool, no host C/C++ compiler) builds build/SimFW.uf2 as a POST_BUILD
# step below, so BOOTSEL drag-and-drop (DESIGN_NOTES.md section 3.1/9) has a
# real .uf2 to use -- see that script's module docstring for the UF2 format
# and boot2-CRC verification it performs. Re-enable pico_add_extra_outputs()
# instead of this custom command only if picotool ever becomes available and
# its extra outputs (.hex/.dis) are actually wanted.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET SimFW POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/elf2uf2.py build
            $<TARGET_FILE:SimFW>
            --bin $<TARGET_FILE_DIR:SimFW>/SimFW.bin
            --uf2 $<TARGET_FILE_DIR:SimFW>/SimFW.uf2
            --objcopy ${CMAKE_OBJCOPY}
            --verify
    COMMENT "Generating and verifying SimFW.uf2 (tools/elf2uf2.py, no picotool needed)"
    VERBATIM
)
