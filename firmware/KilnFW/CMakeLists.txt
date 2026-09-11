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

# --- Keep the ELF a board was actually flashed with, so a coredump from it
# --- can always be symbolized ----------------------------------------------
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
# The real archiving happens on the FLASH side, not here: flash_firmware()/
# debug_program() call tools/PcTools/src/kilnctrl/elf_archive.py's
# archive_kiln_elf() after a confirmed flash, which copies the ELF into
# build/elf_archive/ keyed by content hash AND registers it in manifest.json
# under the identity the board's own /api/status will later report
# (`fw_build`) -- that registration is what makes retention (see
# elf_archive.py's MAX_ARCHIVED_ELFS policy) and lookup (find_crash_elf)
# possible at all. This POST_BUILD step only refreshes a `KilnCtrl-latest.elf`
# convenience pointer at the most recently LINKED (not necessarily flashed)
# ELF -- see archive_elf.cmake's own comment for why it used to also deposit
# a permanent, unregistered copy on every ordinary build (~1.3 GB of builds
# that were never on any board, found and removed 2026-09-10) and no longer
# does.
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
