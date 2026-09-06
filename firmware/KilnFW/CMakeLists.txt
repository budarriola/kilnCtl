# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# App/ holds app_main() in place of the conventional "main" component, and
# App/drivers is its own sub-component (the device drivers plus the
# i2c/spi/uart owners); both need to be pointed to explicitly since neither
# lives under main/ or components/ where the build would find them
# automatically.
# The ESP-IDF component lives at ../hwAbstraction/idf/hwabstraction_esp, a
# thin wrapper directory named "hwabstraction_esp" (ESP-IDF derives a
# component's name from its directory name, and that name lives in a flat
# global namespace) so it does not register under the bare, collision-prone
# name "esp". Its sources live one level up, under
# ../hwAbstraction/esp/ -- see that wrapper's CMakeLists.txt.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/App"
    "${CMAKE_CURRENT_LIST_DIR}/App/drivers"
    "${CMAKE_CURRENT_LIST_DIR}/../hwAbstraction/idf/hwabstraction_esp"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(KilnCtrl)

# --- Keep every built ELF, so a coredump can always be symbolized ----------
#
# A coredump records the SHA256 of the app that crashed, and espcoredump
# refuses to decode against any other ELF -- correctly, since symbolizing
# against the wrong build gives confidently wrong answers.
#
# The catch specific to this project: App/drivers/gen_build_info.cmake
# regenerates build_info.h with a BUILD TIMESTAMP on every single build (see
# the comment above `drivers_build_info`), so rebuilding the identical source
# produces a different binary and a different SHA. The ELF for a flashed image
# therefore CANNOT be reconstructed after the fact. Once it is overwritten,
# any dump from that image is unreadable.
#
# That is not hypothetical: on 2026-08-20 the first spontaneous reboot this
# project ever captured was nearly lost this way, and decoding it needed a
# throwaway script to override the SHA check -- with the caveat that its
# output could only be trusted because the source happened to differ by two
# blank lines.
#
# So: archive each ELF next to the build, keyed by content hash. Cheap (a file
# copy of an already-built artifact), automatic, and it means the answer to
# "which ELF matches this dump?" is always "one of these". Keyed by hash
# rather than timestamp so identical rebuilds collapse to one file instead of
# accumulating.
#
# POST_BUILD on the app target rather than a separate target: it must run
# every time the ELF is relinked, including when nothing else changed.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
        -DOUTDIR=${CMAKE_BINARY_DIR}/elf_archive
        -P ${CMAKE_CURRENT_LIST_DIR}/archive_elf.cmake
    VERBATIM
)
