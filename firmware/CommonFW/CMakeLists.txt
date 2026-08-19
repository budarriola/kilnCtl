# kilnlink -- the shared wire-protocol library between KilnFW (ESP-IDF) and
# SaftyFW (pico-sdk). See README.md for the freestanding-C11 rules every file
# under src/ and include/ must follow: no allocation, no I/O, no globals,
# every decoder bounds-checked. Consumed three ways:
#
#   SaftyFW (pico-sdk):  add_subdirectory(../CommonFW kilnlink)
#                         target_link_libraries(SaftyFW PRIVATE kilnlink)
#   KilnFW (ESP-IDF):     a thin components/kilnlink/CMakeLists.txt wrapper
#                         pointing idf_component_register at this directory
#   host tests:           this file, built standalone (the default when this
#                         is the top-level CMakeLists, i.e. not pulled in via
#                         add_subdirectory from an embedded build)
cmake_minimum_required(VERSION 3.16)
project(kilnlink C)

add_library(kilnlink STATIC
    src/kilnlink_crc.c
    src/kilnlink_frame.c
    src/kilnlink_context.c
    src/kilnlink_status.c
    src/kilnlink_power.c
    src/kilnlink_announce.c
    src/kilnlink_diag.c
    src/kilnlink_trip.c
    src/kilnlink_ceiling.c
    src/kilnlink_clear_trip.c
    src/kilnlink_get_fw_version.c
    src/kilnlink_set_clock.c
)
target_include_directories(kilnlink PUBLIC include)
set_target_properties(kilnlink PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(MSVC)
    target_compile_options(kilnlink PRIVATE /W4 /WX)
else()
    target_compile_options(kilnlink PRIVATE -Wall -Wextra -Werror)
endif()

# Only build the host test binary when this is the top-level CMake project --
# an embedded build (pico-sdk/ESP-IDF) pulling kilnlink in via
# add_subdirectory() gets just the library, not a host-only test executable
# it has no way to run.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_frame test/test_frame.c)
    target_link_libraries(test_frame PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_frame PRIVATE /W4 /WX)
    else()
        target_compile_options(test_frame PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_frame COMMAND test_frame)

    add_executable(test_fuzz test/test_fuzz.c)
    target_link_libraries(test_fuzz PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_fuzz PRIVATE /W4 /WX)
    else()
        target_compile_options(test_fuzz PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_fuzz COMMAND test_fuzz)

    add_executable(test_context test/test_context.c)
    target_link_libraries(test_context PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_context PRIVATE /W4 /WX)
    else()
        target_compile_options(test_context PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_context COMMAND test_context)

    add_executable(test_status test/test_status.c)
    target_link_libraries(test_status PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_status PRIVATE /W4 /WX)
    else()
        target_compile_options(test_status PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_status COMMAND test_status)

    add_executable(test_power test/test_power.c)
    target_link_libraries(test_power PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_power PRIVATE /W4 /WX)
    else()
        target_compile_options(test_power PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_power COMMAND test_power)

    add_executable(test_announce test/test_announce.c)
    target_link_libraries(test_announce PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_announce PRIVATE /W4 /WX)
    else()
        target_compile_options(test_announce PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_announce COMMAND test_announce)

    add_executable(test_uart_protocol_delegate test/test_uart_protocol_delegate.c)
    target_link_libraries(test_uart_protocol_delegate PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_uart_protocol_delegate PRIVATE /W4 /WX)
    else()
        target_compile_options(test_uart_protocol_delegate PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_uart_protocol_delegate COMMAND test_uart_protocol_delegate)

    add_executable(test_diag test/test_diag.c)
    target_link_libraries(test_diag PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_diag PRIVATE /W4 /WX)
    else()
        target_compile_options(test_diag PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_diag COMMAND test_diag)

    add_executable(test_trip test/test_trip.c)
    target_link_libraries(test_trip PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_trip PRIVATE /W4 /WX)
    else()
        target_compile_options(test_trip PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_trip COMMAND test_trip)

    add_executable(test_ceiling test/test_ceiling.c)
    target_link_libraries(test_ceiling PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_ceiling PRIVATE /W4 /WX)
    else()
        target_compile_options(test_ceiling PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_ceiling COMMAND test_ceiling)

    add_executable(test_clear_trip test/test_clear_trip.c)
    target_link_libraries(test_clear_trip PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_clear_trip PRIVATE /W4 /WX)
    else()
        target_compile_options(test_clear_trip PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_clear_trip COMMAND test_clear_trip)

    add_executable(test_get_fw_version test/test_get_fw_version.c)
    target_link_libraries(test_get_fw_version PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_get_fw_version PRIVATE /W4 /WX)
    else()
        target_compile_options(test_get_fw_version PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_get_fw_version COMMAND test_get_fw_version)

    add_executable(test_set_clock test/test_set_clock.c)
    target_link_libraries(test_set_clock PRIVATE kilnlink)
    if(MSVC)
        target_compile_options(test_set_clock PRIVATE /W4 /WX)
    else()
        target_compile_options(test_set_clock PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_set_clock COMMAND test_set_clock)
endif()
