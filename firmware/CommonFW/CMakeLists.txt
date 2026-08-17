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
endif()
