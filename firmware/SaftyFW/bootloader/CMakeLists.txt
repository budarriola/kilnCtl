# SaftyFW bootloader -- RP2040, pico-sdk, bare-metal (NO FreeRTOS-Kernel).
# docs/BOOTLOADER.md is the design authority; if this file and that doc
# disagree, the doc wins.
#
# This is a SEPARATE build from ../CMakeLists.txt (the FreeRTOS application):
# two different add_executable() targets that must never be combined into
# one CMake invocation without careful thought. The bootloader must never
# link FreeRTOS or anything the application needs -- docs/BOOTLOADER.md
# section 1's "written once over SWD and never updates itself" is only
# defensible if this stays minimal and auditable on its own. A top-level
# script that builds both this and ../CMakeLists.txt is a separate, later
# piece of work -- not built here.
#
# Uses pico-sdk's DEFAULT linker script, deliberately -- see main.c's header
# comment / the coordinator's task brief: pico-sdk's standard app linker
# script places boot2 at 0x10000000 (256B) and the vector table immediately
# after at 0x10000100, which is exactly BOOTLOADER_FLASH_OFFSET
# (flash_layout.h). So this is just an ordinary pico-sdk executable, flashed
# at the very start of flash like any other -- no custom linker script for
# the bootloader itself. (The APPLICATION images in slot A/B DO need a
# custom linker script to relocate their vector table to a non-zero offset --
# that is out of scope here, a separate follow-on on ../CMakeLists.txt.)
#
# The only real size constraint: the resulting binary must fit within
# BOOTLOADER_FLASH_SIZE (~64K, flash_layout.h) -- keep this lean (no
# FreeRTOS, minimal pico-sdk libraries linked).
#
# Requires, on the machine building this (same toolchain as ../CMakeLists.txt,
# minus FREERTOS_KERNEL_PATH -- this target does not use FreeRTOS):
#   PICO_SDK_PATH        -- github.com/raspberrypi/pico-sdk, 2.x
#   arm-none-eabi-gcc on PATH, or PICO_TOOLCHAIN_PATH pointing at its bin dir
#
# Example (PowerShell), same toolchain layout as ../CMakeLists.txt's own
# header comment:
#   $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
#   cmake -G Ninja -B build -DPICO_BOARD=pico ^
#     "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
#   cmake --build build
cmake_minimum_required(VERSION 3.16)

# Must be included before project() -- same pico-sdk convention as
# ../CMakeLists.txt. Reuses the copy one directory up rather than
# duplicating it.
include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(saftyfw_bootloader C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

add_executable(saftyfw_bootloader
    main.c
    crc32.c
    metadata.c
)

target_include_directories(saftyfw_bootloader PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_options(saftyfw_bootloader PRIVATE -Wall -Wextra -Werror)

target_link_libraries(saftyfw_bootloader PRIVATE
    pico_stdlib
    hardware_flash
    hardware_sync
)

# SWD/probe flashing only, same as the application (docs/ARCHITECTURE.md
# section 1: "Not USB") -- no stdio_usb, no stdio_uart. The bootloader
# has no console output of its own; recovery mode's beacon is a raw UART1
# write, not stdio.
pico_enable_stdio_usb(saftyfw_bootloader 0)
pico_enable_stdio_uart(saftyfw_bootloader 0)

# pico_add_extra_outputs(saftyfw_bootloader) would generate .uf2/.bin/.hex/
# .dis and embed the RP2040 boot-stage2 checksum via picotool -- left
# disabled on this machine for the exact same reason ../CMakeLists.txt
# documents at its own call site: picotool has no prebuilt binary for this
# pico-sdk version, and building it from source needs a HOST C/C++ compiler
# (MSVC or a native gcc), which is not available/configured in this build
# environment (only arm-none-eabi-gcc is set up, deliberately, since
# flashing here is SWD-via-OpenOCD, not a UF2 drag-and-drop). The plain
# .elf this produces is what OpenOCD flashes anyway. Re-enable once
# picotool is available, mirroring ../CMakeLists.txt's own note.
