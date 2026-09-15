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
    src/clear_trip_diag.c
    src/clear_trip_diag_codec.c
    src/watchdog_overdue_diag.c
    src/watchdog_overdue_diag_codec.c
    src/safety_guards.c
    src/max31856.c
    # Pure CR1.TC TYPE[3:0] range predicate, split out of max31856.c so it is
    # host-testable (test/test_max31856_tc_type_policy.c) with no SPI/GPIO
    # stubs -- see max31856_tc_type_policy.h.
    src/max31856_tc_type_policy.c
    # Pure ~FAULT-pin raw-GPIO-level-to-asserted-bool predicate, split out of
    # max31856.c for the same host-testing reason -- see max31856_fault_pin_
    # policy.h. Feeds S5 (thermocouple fault).
    src/max31856_fault_pin_policy.c
    # Pure CJ/TC fixed-point decode math, split out of max31856.c for the same
    # host-testing reason -- see max31856_decode.h.
    src/max31856_decode.c
    # Pure per-tc_type plausibility range (datasheet Table 1), consumed by
    # thermo_task.c to feed S5's sensor-invalid path -- see
    # max31856_tc_range_policy.h.
    src/max31856_tc_range_policy.c
    # Pure retry-cadence policy for re-attempting max31856_configure() while
    # tc_type has never verified (bring-up race: IC not yet powered when
    # main.c's one-shot configure() runs) -- consumed by thermo_task.c, see
    # max31856_reconfig_retry.h.
    src/max31856_reconfig_retry.c
    src/current_sense.c
    # NOTE: src/spi_owner.c, src/tasks/uart_owner.c and
    # src/tasks/uart_owner_tx_policy.c moved to the hwabstraction_pico
    # library below (HAL Phase 1a, docs/HW_ABSTRACTION.md) and are no
    # longer part of this source list.
    # Pure counts-domain presence decision, decoupled from k_ct_v_per_a, split
    # out of current_sense.c so it is host-testable (test/test_current_
    # presence_policy.c) with no hardware/adc.h stub -- see
    # current_presence_policy.h.
    src/current_presence_policy.c
    # ct_amps_cal.c -- pure, host-tested (test/test_ct_amps_cal.c) per-channel
    # CT amps correction current_sense.c applies on top of its own physics-
    # based conversion; see that file's header comment.
    src/ct_amps_cal.c
    # Phase 9 -- src/config_store.c is the pure, host-tested record format
    # (test/test_config_store.c, mirrors bootloader/metadata.c); config_store_flash.c
    # is the real XIP-read / flash_safe_execute()-write / ARMED-check glue, NOT
    # host-tested, same discipline as update_task.c (see that file's header
    # comment for why).
    src/config_store.c
    src/config_store_flash.c
    # docs/COMMISSIONING.md section 2's SET_PARAM/COMMIT_CONFIG/GET_PARAM/
    # GET_CONFIG_PAGE wire family: the param_id <-> config_store_record_t
    # field table plus cross-field COMMIT_CONFIG validation. Pure, host-tested
    # (test/test_config_store.c) same as config_store.c itself.
    src/config_params.c
    src/commissioning_gate.c
    src/tasks/relay_owner.c
    src/tasks/relay_grace.c
    src/tasks/watchdog_task.c
    src/tasks/watchdog_gate.c
    src/tasks/clock_health.c
    src/tasks/tick_timing.c
    src/tasks/safety_core.c
    # Pure GPIO-level-to-logical-meaning mapping for E-stop/mainFault, split
    # out of discrete_task.c so it is host-testable (test/test_discrete_pin_
    # policy.c) with no hardware/gpio.h stub -- see discrete_pin_policy.h.
    src/discrete_pin_policy.c
    # Pure consecutive-sample debounce, split out of discrete_task.c's
    # static debounce_update() so it is host-testable (test/test_debounce_
    # policy.c, test/test_debounce_nuisance.c) -- see debounce_policy.h.
    src/debounce_policy.c
    src/tasks/discrete_task.c
    # Pure missed-~DRDY-edge recovery decision, split out of thermo_task.c's
    # loop so it is host-testable (test/test_thermo_task_drdy_recovery.c) --
    # see thermo_task_drdy_recovery.h.
    src/tasks/thermo_task_drdy_recovery.c
    src/tasks/thermo_task.c
    # Pure tc_type-change reapply decision, split out of thermo_task.c/
    # link_task.c so it is host-testable (test/test_tc_type_reapply_
    # policy.c) -- see tc_type_reapply_policy.h.
    src/tasks/tc_type_reapply_policy.c
    src/tasks/current_task.c
    src/tasks/console_uart.c
    src/tasks/link_frame.c
    src/tasks/link_task.c
    # Pure Frame B (DIAG) `flags` byte assembly, split out of link_task.c's
    # link_task_send_diag() so it is host-testable (test/test_link_diag_
    # flags.c) -- see link_diag_flags.h.
    src/link_diag_flags.c
    src/tasks/reboot_announce.c
    # Live per-task stack high-water-mark round-robin sampler, KILNLINK_
    # PROTOCOL_VERSION 13 (docs/audits/saftyfw_live_stack_reporting_impl_
    # 2026-09-14.md) -- see stack_margin_poller.h for the design rationale.
    src/tasks/stack_margin_poller.c
    src/tasks/log_task.c
    src/tasks/tx_watermark.c
    src/tasks/update_task.c
    # 2026-09-09 -- the pure ARMED/TRANSFER_ACTIVE decision table behind
    # update_task_reboot_allowed(), split out so it can be host-compiled and
    # behaviourally tested (see update_task_reboot_policy.h for why).
    src/tasks/update_task_reboot_policy.c
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

# hwabstraction_pico -- HAL Phase 1a (docs/HW_ABSTRACTION.md move set):
# src/spi_owner.c/h, src/tasks/uart_owner.c/h (renamed to
# hal_uart_pico_internal.h to avoid colliding with the ESP-side
# uart_owner.h moving into firmware/hwAbstraction/esp/uart/ under the same
# effort) and src/tasks/uart_owner_tx_policy.c/h relocated here, byte-
# identical apart from include paths, colocated in per-interface dirs
# (pico/spi/, pico/uart/). One STATIC library, linked by all three
# executables below (SaftyFW, SaftyFW_slotA, SaftyFW_slotB) -- their link
# lists and FreeRTOSConfig.h are identical (see the plan's "Verified shape"
# note), and the only runtime registration inside these sources is
# irq_set_exclusive_handler (uart_owner.c), called from init rather than a
# constructor, so a STATIC lib shared by all three is safe.
#
# This was move-only in Phase 1a: the interface/ + pico/*/hal_*_pico.c HAL
# bodies (hal_uart_pico.c included) had no consumers and were deliberately
# NOT added to this library or wired into these targets -- only the moved
# owner sources, which SaftyFW's real code already calls directly
# (spi_owner_transfer, uart_owner_send, etc.), unchanged. Phase 1b (below)
# is the first exception: relay_owner.c is now a real hal_gpio consumer, so
# hal_gpio_pico.c + hal_status.c are wired in for real.
# hal_gpio (HAL Phase 1b, docs/HW_ABSTRACTION.md "hal_gpio" section):
# relay_owner.c is now a hal_gpio client (interface/hal_gpio.h), backed on
# target by pico/gpio/hal_gpio_pico.c. common/hal_status.c is the shared,
# vendor-neutral hal_status_t name table every backend links against.
# hal_wdt (HAL Phase 4, docs/HW_ABSTRACTION.md "hal_time / hal_wdt /
# hal_pwm / hal_sysinfo" section): pico/wdt/hal_wdt_pico.c existed since the
# Phase 1b syntax-only pass (compile_pico_backends.ps1) but was never linked
# into a real target -- watchdog_task.c (feed) and update_task.c (reboot) are
# now real hal_wdt clients, wired in for real by this pass.
set(HWABSTRACTION_DIR ${CMAKE_CURRENT_LIST_DIR}/../hwAbstraction)
set(HWABSTRACTION_PICO_DIR ${HWABSTRACTION_DIR}/pico)
add_library(hwabstraction_pico STATIC
    ${HWABSTRACTION_PICO_DIR}/spi/spi_owner.c
    ${HWABSTRACTION_PICO_DIR}/spi/hal_spi_pico.c
    ${HWABSTRACTION_PICO_DIR}/uart/uart_owner.c
    ${HWABSTRACTION_PICO_DIR}/uart/uart_owner_tx_policy.c
    ${HWABSTRACTION_PICO_DIR}/gpio/hal_gpio_pico.c
    ${HWABSTRACTION_PICO_DIR}/adc/hal_adc_pico.c
    ${HWABSTRACTION_PICO_DIR}/scratch/hal_scratch_pico.c
    ${HWABSTRACTION_PICO_DIR}/flash/hal_flash_pico.c
    ${HWABSTRACTION_PICO_DIR}/wdt/hal_wdt_pico.c
    ${HWABSTRACTION_DIR}/common/hal_status.c
)
target_include_directories(hwabstraction_pico PUBLIC
    ${HWABSTRACTION_PICO_DIR}/spi
    ${HWABSTRACTION_PICO_DIR}/uart
    ${HWABSTRACTION_PICO_DIR}/gpio
    ${HWABSTRACTION_PICO_DIR}/adc
    ${HWABSTRACTION_PICO_DIR}/scratch
    ${HWABSTRACTION_PICO_DIR}/flash
    ${HWABSTRACTION_PICO_DIR}/wdt
    ${HWABSTRACTION_DIR}/interface
)
# CLOSED 2026-09-06 (HAL Phase 1b, docs/HW_ABSTRACTION.md): this used to
# give hwabstraction_pico a PRIVATE include dir on src/board so spi_owner.c
# and uart_owner.c could reach SaftyFW's own board_pins.h across the
# hwAbstraction/SaftyFW boundary. Both files now take their pin values as
# init-time parameters instead (spi_owner_pins_t/uart_owner_pins_t) --
# neither includes board_pins.h any more, so this library needs no
# visibility into SaftyFW's private src/ tree at all.
# FreeRTOSConfig.h lives at the SaftyFW project root (same file SaftyFW's own
# target_include_directories(SaftyFW PRIVATE ${CMAKE_CURRENT_LIST_DIR} ...)
# exposes). Linking FreeRTOS-Kernel PUBLIC into this library means every
# pico-sdk source it pulls in gets compiled with
# PICO_CONFIG_RTOS_ADAPTER_HEADER active (freertos_sdk_config.h -> "FreeRTOS
# Config.h"), so this library needs the same include, not just SaftyFW's own
# target.
target_include_directories(hwabstraction_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(hwabstraction_pico PUBLIC
    hardware_gpio
    hardware_irq
    hardware_uart
    hardware_spi
    hardware_adc
    hardware_flash
    hardware_watchdog
    pico_flash
    pico_multicore
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
target_compile_options(hwabstraction_pico PRIVATE -Wall -Wextra -Werror)

add_executable(SaftyFW ${SAFTYFW_APP_SOURCES})

target_include_directories(SaftyFW PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    src
    src/board
    src/update
    bootloader
    ${CMAKE_CURRENT_BINARY_DIR}
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

# Same "plain CMake option is this project's Kconfig" pattern as the flag
# above, and bench-only for a much sharper reason: this one DISABLES a safety
# detector. See src/tasks/thermo_task.c's SAFTYFW_THERMO_ASSUME_DRDY comment
# for the datasheet arithmetic and for exactly what is given up -- in short,
# with it on, a MAX31856 that has died mid-run is read on a timer and its
# stale registers are published as though fresh, because ~DRDY silence is no
# longer treated as evidence of anything. It exists only because the bench
# fixture's ~DRDY wire (fixture GP26 to this board's GP12) is not fitted, and
# it must go off again the moment that wire is in. The translation unit also
# raises a #warning on every build when this is on, so an enabled CMake cache
# cannot quietly outlive the missing jumper.
option(SAFTYFW_THERMO_ASSUME_DRDY "Read the MAX31856 on a conversion-time timer when no ~DRDY edge arrives (BENCH-ONLY: disables ~DRDY staleness detection)" OFF)
if(SAFTYFW_THERMO_ASSUME_DRDY)
    target_compile_definitions(SaftyFW PRIVATE SAFTYFW_THERMO_ASSUME_DRDY=1)
    message(WARNING
        "SAFTYFW_THERMO_ASSUME_DRDY=ON: the MAX31856 will be read on a "
        "conversion-time timer and ~DRDY staleness detection is DISABLED. "
        "A part that dies mid-run will have its stale registers published as "
        "though fresh. Bench-only -- turn this off once the fixture's ~DRDY "
        "wire (fixture GP26 to this board's GP12) is fitted.")
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
    hwabstraction_pico
)

# SaftyFW's own build-identity header (TODO.md Phase 8: "build_info.h-
# equivalent identity for SaftyFW itself... is not generated on every build
# yet"). Copies KilnFW's App/drivers/CMakeLists.txt + gen_build_info.cmake
# model exactly, including the add_custom_target-not-add_custom_command
# reasoning: add_custom_target has no dependency tracking, so Ninja/Make
# reruns it on *every* build invocation rather than only when CMake
# reconfigures or a tracked file changes -- necessary here because the git
# commit/dirty state and the build timestamp can both change between two
# builds that touch none of this target's sources.
#
# Header lands in the build directory (${CMAKE_CURRENT_BINARY_DIR}/
# saftyfw_build_info.h) and is include-able as "saftyfw_build_info.h" from
# any SaftyFW source, since that directory is on this target's (and the two
# slot targets' -- see saftyfw_add_slot_executable() below)
# target_include_directories(). Macros: SAFTYFW_GIT_COMMIT (string),
# SAFTYFW_GIT_DIRTY (0/1), SAFTYFW_BUILD_DATE (string, UTC), SAFTYFW_BUILD_TIME
# (string, UTC). NOT wired into link_task_send_fw_version() by this change --
# that file is owned by other work in progress; this only makes the header
# exist, build, and be include-able. See gen_build_info.cmake's own comment
# for the full per-field rationale (identical to KilnFW's copy).
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE SAFTYFW_GIT_TOPLEVEL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT SAFTYFW_GIT_TOPLEVEL)
    set(SAFTYFW_GIT_TOPLEVEL ${CMAKE_CURRENT_SOURCE_DIR})
endif()

set(SAFTYFW_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/saftyfw_build_info.h")
add_custom_target(saftyfw_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${SAFTYFW_BUILD_INFO_HEADER}
        -DPROJECT_ROOT=${SAFTYFW_GIT_TOPLEVEL}
        -DTHIS_PROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/gen_build_info.cmake
    BYPRODUCTS ${SAFTYFW_BUILD_INFO_HEADER}
    COMMENT "Regenerating saftyfw_build_info.h (git commit/dirty/build time)"
    VERBATIM
)
add_dependencies(SaftyFW saftyfw_build_info)

# TODO.md Phase 10 "*** MANUAL SYNC HAZARD ***": bootloader/flash_layout.h's
# slot offsets/size are hand-copied into app_slot.ld.in's FLASH LENGTH and
# the two saftyfw_add_slot_executable() origin literals just below. This
# target parses the header and asserts all three agree, failing loudly with
# the specific mismatched numbers if not -- see
# tools/check_flash_layout_sync.cmake for the full parsing/comparison logic.
# Wired into the build (add_custom_target ALL, same "reruns every build"
# reasoning as saftyfw_build_info above) rather than left as a standalone
# script, so a hand-edit to any of the three files is caught on the very
# next build without anyone having to remember to run a separate command.
add_custom_target(saftyfw_check_flash_layout_sync ALL
    COMMAND ${CMAKE_COMMAND}
        -DFLASH_LAYOUT_HEADER=${CMAKE_CURRENT_LIST_DIR}/bootloader/flash_layout.h
        -DLD_TEMPLATE=${CMAKE_CURRENT_LIST_DIR}/bootloader/app_slot.ld.in
        -DSLOT_A_ORIGIN=0x10011000
        -DSLOT_B_ORIGIN=0x100E1000
        -DXIP_BASE=0x10000000
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/check_flash_layout_sync.cmake
    COMMENT "Checking bootloader/flash_layout.h agrees with app_slot.ld.in and CMakeLists.txt slot literals"
    VERBATIM
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
        src/board
        src/update
        bootloader
        ${CMAKE_CURRENT_BINARY_DIR}
    )

    target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Werror)

    # Same generated build-identity header as the default SaftyFW target
    # above -- these slot executables are the SAME application sources, so
    # they get the SAME saftyfw_build_info.h (one generation, shared by all
    # three targets via ${CMAKE_CURRENT_BINARY_DIR} on each target's include
    # path).
    add_dependencies(${TARGET_NAME} saftyfw_build_info)

    # Same SAFTYFW_ENABLE_USB_STDIO gating as the default SaftyFW target
    # above -- keeps all three executables' compiled behavior consistent for
    # whatever value the option was configured with.
    if(SAFTYFW_ENABLE_USB_STDIO)
        target_compile_definitions(${TARGET_NAME} PRIVATE SAFTYFW_ENABLE_USB_STDIO=1)
    endif()
    if(SAFTYFW_THERMO_ASSUME_DRDY)
        target_compile_definitions(${TARGET_NAME} PRIVATE SAFTYFW_THERMO_ASSUME_DRDY=1)
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
        hwabstraction_pico
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
