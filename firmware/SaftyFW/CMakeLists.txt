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

# SAFTYFW_APP_SOURCES -- the one source list shared by the default SaftyFW
# target below AND the two slot-linked targets further down (SaftyFW_slotA /
# SaftyFW_slotB, docs/BOOTLOADER.md section 2 / TODO.md Phase 10 item 10.6).
# Factored out so all three executables are provably built from the exact
# same application sources -- no hand-copied file list to let drift.
set(SAFTYFW_APP_SOURCES
    src/main.c
    src/boot_reason.c
    src/safety_guards.c
    src/spi_owner.c
    src/max31856.c
    src/current_sense.c
    src/tasks/relay_owner.c
    src/tasks/relay_grace.c
    src/tasks/watchdog_task.c
    src/tasks/safety_core.c
    src/tasks/discrete_task.c
    src/tasks/thermo_task.c
    src/tasks/current_task.c
    src/tasks/uart_owner.c
    src/tasks/link_frame.c
    src/tasks/link_task.c
    src/tasks/log_task.c
    src/tasks/tx_watermark.c
    src/tasks/update_task.c
    # Phase 10 -- update_task.c's own frame-receive/decision logic. These
    # four are the FROZEN, host-tested (272/272, test/build_host_tests.ps1)
    # pure modules from src/update/; bootloader/crc32.c and
    # bootloader/metadata.c are the SAME frozen files bootloader/main.c
    # itself builds against (a separate executable, its own CMakeLists.txt)
    # -- compiled a second time here into the application, not modified, so
    # the application can persist/read metadata records with the exact same
    # pack/unpack/CRC logic the bootloader uses to read them back.
    src/update/image_header.c
    src/update/received_ranges.c
    src/update/update_receiver.c
    src/update/confirm.c
    bootloader/crc32.c
    bootloader/metadata.c
)

add_executable(SaftyFW ${SAFTYFW_APP_SOURCES})

target_include_directories(SaftyFW PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    src
    src/update
    bootloader
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
    hardware_flash # Phase 10, added this pass -- update_task.c's flash_range_erase()/_program(), same library bootloader/CMakeLists.txt already links
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

# =============================================================================
# SaftyFW_slotA / SaftyFW_slotB -- the SAME application, linked to run from
# inside the bootloader's slot A / slot B (docs/BOOTLOADER.md section 2,
# bootloader/flash_layout.h's BOOTLOADER_SLOT_A_FLASH_OFFSET /
# BOOTLOADER_SLOT_B_FLASH_OFFSET). Originally (TODO.md item 10.6, first pass)
# this was link-time-only -- nothing in the application called
# flash_range_erase()/flash_range_program(). That is no longer true: Phase
# 10's later pass added src/tasks/update_task.c, which does real,
# flash_safe_execute()-wrapped erase/program calls to stage an incoming
# image into the INACTIVE slot (never the one this executable itself is
# running from) -- see that file's own header comment for the RP2040/
# FreeRTOS-SMP flash-write rules it follows.
#
# The default SaftyFW target above is UNCHANGED and UNAFFECTED by any of
# this -- it keeps linking at pico-sdk's stock application address (which
# happens to collide with the bootloader's own address, 0x10000100; see the
# task history in TODO.md/BOOTLOADER.md) and remains what ordinary
# development/debugging builds against.
#
# bootloader/main.c's jump_to_app() requires a valid Cortex-M vector table
# (word 0 = initial SP, word 1 = reset handler) as the very first thing at
# the slot's start address, with no boot2 stub -- see
# bootloader/app_slot.ld.in's header comment for the full contract and how
# this custom linker script (derived from pico-sdk's own
# pico_crt0/rp2040/memmap_default.ld) satisfies it.
function(saftyfw_add_slot_executable TARGET_NAME FLASH_ORIGIN)
    add_executable(${TARGET_NAME} ${SAFTYFW_APP_SOURCES})

    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        src
        src/update
        bootloader
    )

    target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Werror)

    # Same SAFTYFW_ENABLE_USB_STDIO gating as the default SaftyFW target
    # above -- keeps all three executables' compiled behavior consistent for
    # whatever value the option was configured with.
    if(SAFTYFW_ENABLE_USB_STDIO)
        target_compile_definitions(${TARGET_NAME} PRIVATE SAFTYFW_ENABLE_USB_STDIO=1)
    endif()

    target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_gpio
        hardware_irq
        hardware_watchdog
        hardware_adc
        hardware_uart
        hardware_spi
        hardware_i2c
        hardware_flash
        pico_flash
        pico_multicore
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        kilnlink
    )

    pico_enable_stdio_usb(${TARGET_NAME} ${SAFTYFW_ENABLE_USB_STDIO})
    pico_enable_stdio_uart(${TARGET_NAME} 0)

    # SAFTYFW_APP_FLASH_ORIGIN is read by configure_file() below via
    # bootloader/app_slot.ld.in's @SAFTYFW_APP_FLASH_ORIGIN@ placeholder.
    # This is a per-target, per-call local value (function scope), not a
    # cache/global -- each call to this function configures its own copy of
    # the .ld file with its own origin.
    set(SAFTYFW_APP_FLASH_ORIGIN ${FLASH_ORIGIN})
    set(_saftyfw_slot_ld ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_app_slot.ld)
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/bootloader/app_slot.ld.in
        ${_saftyfw_slot_ld}
        @ONLY
    )

    # pico_set_linker_script() is pico-sdk's own documented mechanism for
    # overriding a target's linker script (pico_standard_link/CMakeLists.txt)
    # -- used here instead of hand-rolling target_link_options(), so this
    # keeps working the same way the SDK's own no_flash/copy_to_ram/etc.
    # binary-type mechanism does.
    pico_set_linker_script(${TARGET_NAME} ${_saftyfw_slot_ld})
endfunction()

# BOOTLOADER_SLOT_A_FLASH_OFFSET (0x00011000) / BOOTLOADER_SLOT_B_FLASH_OFFSET
# (0x000E1000) from bootloader/flash_layout.h, plus XIP_BASE (0x10000000) --
# see that file's own header comment for why the offsets there are
# flash-relative rather than XIP-mapped. *** MANUAL SYNC HAZARD: *** these two
# literals, app_slot.ld.in's hardcoded FLASH LENGTH (0xD0000), and
# flash_layout.h's macros must be kept in sync by hand; nothing enforces this
# today (see app_slot.ld.in's header comment for the same note -- a CI check
# that parses flash_layout.h and asserts these three sources of truth agree
# would close this gap, not built this pass).
saftyfw_add_slot_executable(SaftyFW_slotA 0x10011000)
saftyfw_add_slot_executable(SaftyFW_slotB 0x100E1000)
