# boot_probe -- DIAGNOSTIC TOOL, not part of SimFW proper. See README.md in
# this directory for the question this answers.
#
# Two minimal, standalone pico-sdk programs (no FreeRTOS, no TinyUSB, no
# SimFW code at all) built with the exact same toolchain/PICO_BOARD as
# ../../CMakeLists.txt (SimFW itself), to isolate "is the bare board /
# toolchain / UF2 pipeline good?" from "is SimFW itself broken?".
#
#   blink_gpio -- toggles GPIO25 (onboard LED on a bare Pico) twice a second.
#                 Nothing else. No stdio, no USB.
#   blink_uart -- same blink, plus prints a line over UART0 on GP16(TX)/
#                 GP17(RX) at 115200 8N1 each toggle, via
#                 pico_enable_stdio_uart (same pins/baud SimFW's
#                 CMakeLists.txt configures for its own console).
#
# Build (PowerShell), same invocation shape as ../../CMakeLists.txt's own
# header comment:
#   $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
#   & "C:\Program Files\CMake\bin\cmake.exe" -G Ninja -B build -DPICO_BOARD=pico `
#     "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
#   & "C:\Program Files\CMake\bin\cmake.exe" --build build
#
# Then convert each .elf with ../elf2uf2.py (the SAME converter SimFW's own
# build uses -- no picotool involved either place):
#   python ../elf2uf2.py build blink_gpio.elf --verify
#   python ../elf2uf2.py build blink_uart.elf --verify

cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(boot_probe C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Same UART pin/baud override SimFW's CMakeLists.txt uses, set before
# pico_sdk_init() for the same reason documented there: pico_stdio_uart's
# own sources are compiled against whatever PICO_DEFAULT_UART* the
# preprocessor sees at pico_sdk_init() time.
add_compile_definitions(
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=16
    PICO_DEFAULT_UART_RX_PIN=17
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

pico_sdk_init()

# Deliberately NOT calling pico_add_extra_outputs(): that needs picotool,
# which is not available on this machine (see ../elf2uf2.py's own header
# comment -- SimFW's own CMakeLists.txt hit the same issue and skips it for
# the same reason). The plain .elf from add_executable() below is all
# ../elf2uf2.py needs to produce a .uf2/.bin.

# --- blink_gpio: bare GPIO toggle, nothing else. ---------------------------
add_executable(blink_gpio blink_gpio/main.c)
target_link_libraries(blink_gpio pico_stdlib)

# --- blink_uart: same blink + UART0 stdout line each toggle. ---------------
add_executable(blink_uart blink_uart/main.c)
target_link_libraries(blink_uart pico_stdlib)
pico_enable_stdio_uart(blink_uart 1)
pico_enable_stdio_usb(blink_uart 0)
