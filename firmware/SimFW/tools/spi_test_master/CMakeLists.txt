# spi_test_master -- SimFW bench utility, M-A's "scripted test master."
# See ../../docs/PLAN.md section 10 (milestone M-A) and ./README.md.
#
# DELIBERATELY SEPARATE from ../../CMakeLists.txt (SimFW itself): this is a
# tiny reference firmware for a *second*, spare Pico that clocks SimFW's PIO
# SPI slave emulator during bring-up, not a component of SimFW. It has its
# own CMakeLists.txt, its own executable target, and is configured/built
# independently -- SimFW's own build never references this directory, so
# this tool can never be accidentally linked into the fixture firmware.
# Plain pico-sdk, no FreeRTOS: this firmware's whole job is a blocking
# superloop reading USB CDC lines and driving hardware SPI, which needs no
# scheduler.
#
# Requires the same toolchain as ../../CMakeLists.txt / ../../../SaftyFW/
# CMakeLists.txt (PICO_SDK_PATH, arm-none-eabi-gcc). Build (PowerShell):
#   $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
#   cmake -G Ninja -B build -DPICO_BOARD=pico `
#     "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
#   cmake --build build
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(spi_test_master C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

add_executable(spi_test_master
    src/main.c
    src/spi_master.c
    src/seq.c
)

target_include_directories(spi_test_master PRIVATE
    src
    # Read-only reference to SimFW's own MAX31856 register-address map
    # (src/sim/max31856_regs.h is a pure header, no .c dependency, no
    # pico-sdk assumptions) so this tool's expectations can never drift
    # from the slave model it is testing against. Nothing else from SimFW
    # is included or linked, and this file is never written to from here.
    #
    # Deliberately scoped to src/sim/ (NOT the whole SimFW src/ tree):
    # SimFW's src/ root also holds its own src/tusb_config.h (written for
    # SimFW's FreeRTOS/tinyusb_device setup), which would silently shadow
    # this project's own pico_stdio_usb-supplied tusb_config.h if src/ were
    # on the include path -- CFG_TUSB_RHPORT0_MODE then goes undefined and
    # TinyUSB fails a static_assert. Scoping to src/sim/ avoids the clash
    # entirely.
    ${CMAKE_CURRENT_LIST_DIR}/../../src/sim
)

target_compile_options(spi_test_master PRIVATE -Wall -Wextra -Werror)

target_link_libraries(spi_test_master PRIVATE
    pico_stdlib
    hardware_spi
    hardware_gpio
)

# USB CDC only (this tool's plain-text line protocol, see README.md) --
# no UART backend needed on a bench tool that is always plugged into USB
# anyway.
pico_enable_stdio_usb(spi_test_master 1)
pico_enable_stdio_uart(spi_test_master 0)

# pico-sdk's stock pico_stdio_usb descriptors default to Raspberry Pi's own
# VID:PID (2E8A:000A -- src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c
# in the pico-sdk checkout) and a generic "Pico" product string. That is the
# same generic ID any other stock pico-sdk CDC example on the bench would
# also present, and it happens to be the exact ID `kilnsim`'s USB auto-detect
# used to treat as SimFW's own placeholder (../../docs/HARDWARE.md's "USB
# identity" section) -- so on a bench with this tool, the SimFW fixture, and
# other stock Pico boards all plugged in, a generic "which one is which"
# ambiguity is easy to hit. pico_stdio_usb's descriptors honor USBD_PID/
# USBD_PRODUCT overrides via #ifndef guards, so this is a compile-definition
# override, not a hand-written descriptor file: keep this tool's own VID
# (still Raspberry Pi's, same informal-reuse basis as SimFW's) but move off
# the shared generic PID/string. 0xF00B mirrors SimFW's own 0xF00A choice
# (docs/HARDWARE.md) -- same "far outside RPi's documented low PID range"
# reasoning, one digit apart so the two bench tools are easy to tell apart
# by eye in a USB descriptor dump.
target_compile_definitions(spi_test_master PRIVATE
    USBD_PID=0xF00B
    "USBD_PRODUCT=\"spi_test_master (kilnCtl bench)\""
)

# picotool is not set up in this environment (same gap ../../CMakeLists.txt
# and ../../../SaftyFW/CMakeLists.txt document) -- pico_add_extra_outputs()
# (which would generate the .uf2 for BOOTSEL drag-and-drop) is left disabled.
# The plain .elf below flashes fine over SWD/OpenOCD.
# pico_add_extra_outputs(spi_test_master)
