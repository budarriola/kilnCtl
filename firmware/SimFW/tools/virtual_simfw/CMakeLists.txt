# virtual_simfw -- host-side program that runs SimFW's REAL simulation logic
# (firmware/SimFW/src/sim/*.c, compiled unmodified) and speaks the REAL
# benchproto wire protocol (firmware/CommonFW/src/benchproto_*.c, compiled
# unmodified) over a plain TCP socket, so kilnsim can drive full scenarios
# end-to-end on a PC with no hardware attached. See ./README.md.
#
# DELIBERATELY SEPARATE from ../../CMakeLists.txt (SimFW itself) and from
# firmware/CommonFW's own build, the same way ../spi_test_master/CMakeLists.txt
# is kept apart from SimFW's build: this is a plain host (Win32) console
# program, not RP2040 firmware -- no pico-sdk, no FreeRTOS. Its own
# standalone CMakeLists.txt/executable target means SimFW's real build can
# never accidentally pull this in.
#
# Build (PowerShell, MSVC):
#   $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
#   cmd /c "call `"$vcvars`" x64 && cmake -G Ninja -B build && cmake --build build"
# ...or just run build_host.ps1, which drives `cl` directly the same way
# firmware/SimFW/test/build_host_tests.ps1 does (this repo's environment does
# not reliably have cmake/ninja on PATH -- see that script's own header).
cmake_minimum_required(VERSION 3.16)
project(virtual_simfw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SIMFW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(COMMONFW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../CommonFW)

add_executable(virtual_simfw
    src/virtual_simfw.c
    # SimFW's real, pure sim logic (READ-ONLY: compiled here, never modified
    # -- see this directory's README.md).
    ${SIMFW_ROOT}/src/sim/thermal_model.c
    ${SIMFW_ROOT}/src/sim/max31856_regs.c
    ${SIMFW_ROOT}/src/sim/fault_engine.c
    ${SIMFW_ROOT}/src/sim/tc_fault_state.c
    ${SIMFW_ROOT}/src/sim/sine_synth.c
    # CommonFW's real benchproto wire codec + reliability layer (READ-ONLY).
    ${COMMONFW_ROOT}/src/benchproto_crc.c
    ${COMMONFW_ROOT}/src/benchproto_frame.c
    ${COMMONFW_ROOT}/src/benchproto_link.c
)

target_include_directories(virtual_simfw PRIVATE
    src
    ${SIMFW_ROOT}/src/sim
    ${SIMFW_ROOT}/src/tasks   # cmd_ids.h only -- the numeric wire source of truth
    ${SIMFW_ROOT}/src         # version.h
    ${COMMONFW_ROOT}/include
)

if (MSVC)
    target_compile_options(virtual_simfw PRIVATE /W4)
else()
    target_compile_options(virtual_simfw PRIVATE -Wall -Wextra)
endif()

if (WIN32)
    target_link_libraries(virtual_simfw PRIVATE ws2_32)
endif()
