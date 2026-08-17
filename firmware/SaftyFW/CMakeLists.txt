# SaftyFW -- kiln safety processor firmware, RP2040, pico-sdk + FreeRTOS-Kernel
# (SMP port). docs/ARCHITECTURE.md section 1 is the authority for every choice
# below (toolchain, RTOS, warnings level); if this file and that section
# disagree, the doc wins.
#
# Requires, on the machine building this (none of these are vendored into the
# repo -- see docs/ARCHITECTURE.md section 1, "vendored as a submodule or via
# PICO_SDK_PATH"):
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

project(SaftyFW C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

# FreeRTOS-Kernel's RP2040 SMP port. May be included after pico_sdk_init()
# (pico-sdk > 1.3.1, which 2.x satisfies) -- see
# FreeRTOS_Kernel_import.cmake's own comment.
include(${CMAKE_CURRENT_LIST_DIR}/FreeRTOS_Kernel_import.cmake)

# kilnlink -- the shared wire-protocol library between KilnFW and SaftyFW.
# Not yet actually used by any SaftyFW source (link_task.c's framing is a
# Phase 7 TODO), but linked now so the target exists and pulls kilnlink
# through the real arm-none-eabi-gcc toolchain from day one, matching
# KilnFW's own "linked-but-unused so far" note in
# components/kilnlink/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../CommonFW kilnlink)

add_executable(SaftyFW
    src/main.c
    src/boot_reason.c
    src/safety_guards.c
    src/spi_owner.c
    src/max31856.c
    src/current_sense.c
    src/tasks/relay_owner.c
    src/tasks/watchdog_task.c
    src/tasks/safety_core.c
    src/tasks/discrete_task.c
    src/tasks/thermo_task.c
    src/tasks/current_task.c
    src/tasks/uart_owner.c
    src/tasks/link_frame.c
    src/tasks/link_task.c
    src/tasks/log_task.c
)

target_include_directories(SaftyFW PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    src
)

target_compile_options(SaftyFW PRIVATE -Wall -Wextra -Werror)

# SAFTYFW_ENABLE_USB_STDIO -- TODO.md Phase 2/8, docs/ARCHITECTURE.md section
# 1: "SAFTYFW_ENABLE_USB_STDIO, compile-time, default off, debug builds
# only... a build flag and *cannot* be a runtime GUI toggle -- TinyUSB is
# either linked in or it is not." No Kconfig on this side (that is KilnFW's
# ESP-IDF world); a plain CMake option is the idiomatic pico-sdk equivalent --
# `-DSAFTYFW_ENABLE_USB_STDIO=ON` at configure time is this project's whole
# "Kconfig". Bench-only: docs/HARDWARE.md section 7's 3V3-back-feed caveat
# still applies and is not solved by this flag, only gated by it.
option(SAFTYFW_ENABLE_USB_STDIO "Mirror log_task output to stdio_usb (bench-only, see docs/ARCHITECTURE.md section 1 and docs/HARDWARE.md section 7)" OFF)
if(SAFTYFW_ENABLE_USB_STDIO)
    target_compile_definitions(SaftyFW PRIVATE SAFTYFW_ENABLE_USB_STDIO=1)
endif()
# pico_enable_stdio_usb() itself is called once, below, after
# target_link_libraries -- see that call site's comment for why it is
# conditional on this same option rather than a second, separate call here.

target_link_libraries(SaftyFW PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_irq
    hardware_watchdog
    hardware_adc
    hardware_uart
    hardware_spi
    hardware_i2c
    pico_flash
    pico_multicore
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    kilnlink
)

# SWD/probe flashing only (docs/ARCHITECTURE.md section 1: "Not USB" -- A1's
# 3V3 is back-fed, see docs/HARDWARE.md section 7), and no UART0 stdio by
# default -- the console path is the debug probe's UART bridge on GP16/GP17,
# not stdio_usb. SAFTYFW_ENABLE_USB_STDIO (option above, default OFF) gates
# this on: log_task.c only compiles in its stdio_usb mirror
# (#ifdef SAFTYFW_ENABLE_USB_STDIO) when the option is set, and this is the
# one call site that must agree with it -- pico_enable_stdio_usb() actually
# links TinyUSB in, so calling it unconditionally here would silently
# override the option above regardless of what log_task.c does.
pico_enable_stdio_usb(SaftyFW ${SAFTYFW_ENABLE_USB_STDIO})
pico_enable_stdio_uart(SaftyFW 0)

# pico_add_extra_outputs(SaftyFW) would generate .uf2/.bin/.hex/.dis and embed
# the RP2040 boot-stage2 checksum via picotool -- left disabled on this
# machine because picotool has no prebuilt binary for this pico-sdk version
# and building it from source needs a HOST C/C++ compiler (MSVC or a native
# gcc), which was not available/configured in this build environment (only
# arm-none-eabi-gcc was set up, deliberately, since flashing here is
# SWD-via-OpenOCD, not a UF2 drag-and-drop). The plain .elf below is what
# OpenOCD flashes anyway. Re-enable this line once picotool is available
# (`winget install` a host toolchain, or build/install picotool separately
# and point PICOTOOL_FETCH_FROM_GIT_PATH / find_package(picotool) at it) if a
# .uf2 is ever wanted for bench convenience.
