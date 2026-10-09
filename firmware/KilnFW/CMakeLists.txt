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

# --- Hard-error the missing-declaration class, TREE-WIDE -------------------
#
# 3e0b2fa4 added -Werror=implicit-function-declaration / -implicit-int /
# -int-conversion to the App and App/drivers components via
# target_compile_options(${COMPONENT_LIB} PRIVATE ...). That guard is not
# theoretical: an adversarial review proved end to end that without it, a call
# to an unprototyped function with a float argument compiles with only a
# warning and silently writes the Pico's abs_max_temp_c ceiling as 0.0.
#
# But per-component opt-in means a component is unguarded by OMISSION, and
# silently so. Two first-party components were already outside it: kilnlink
# (the ESP<->Pico link-protocol code, firmware/CommonFW/src) and
# hwabstraction_esp (firmware/hwAbstraction/esp) -- 42 translation units
# compiling with no guard at all, including the very link code that carries
# the safety ceiling.
#
# 3e0b2fa4's rationale for NOT using this tree-wide lever was that it would
# also hit vendored/managed components (lvgl, espressif__mdns,
# joltwallet__littlefs) "which are not ours to patch and would fail the
# build". MEASURED 2026-09-16 -- that is not what happens:
#
#   * Applied UNGUARDED, the build does fail (exit 2) -- but on none of those.
#     It fails on ESP-IDF's own nvs_flash, on all 14 of its C++ translation
#     units, with:
#         cc1plus.exe: error: '-Werror=' argument
#         '-Werror=implicit-function-declaration' is not valid for C++ [-Werror]
#     All three diagnostics are C-only, so ANY C++ translation unit anywhere
#     in the build rejects them outright. lvgl/mdns/littlefs are C and were
#     never the problem; the language mismatch was.
#
#   * Guarded to C with $<COMPILE_LANGUAGE:C>, the full target build is clean:
#     exit 0, zero failed targets, zero implicit-declaration diagnostics
#     anywhere -- vendored and managed components included. Nothing needed
#     patching and nothing needed excluding.
#
# So the guard lives here, once, and covers every component that exists today
# or is added later: there is no list to maintain and nothing to remember.
# That is the whole point -- it closes the omission hole rather than moving it.
#
# The per-component target_compile_options() blocks in App/CMakeLists.txt and
# App/drivers/CMakeLists.txt are now redundant. They are left in place
# deliberately, as local in-file documentation of the rule for anyone reading
# those components; specifying the same -Werror= twice is a no-op, so they
# cannot conflict with this.
#
# Placement is load-bearing: this MUST precede project(), because the
# COMPILE_OPTIONS build property is consumed as each component registers and
# project() is what triggers that registration.
idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>" APPEND)
idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-Werror=implicit-int>" APPEND)
idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-Werror=int-conversion>" APPEND)
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
# elf_archive/ (a sibling of build/, NOT inside it -- see 2026-09-15 note
# below) keyed by content hash AND registers it in manifest.json
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
# 2026-09-15: OUTDIR moved from ${CMAKE_BINARY_DIR}/elf_archive (i.e.
# build/elf_archive) to ${CMAKE_CURRENT_LIST_DIR}/elf_archive (a sibling of
# build/), matching elf_archive.py's kiln_archive_dir()/safty_archive_dir().
# The board's 2026-09-14 23:55:17Z flash (commit c8f7506b) was archived
# correctly at flash time (manifest entry KilnCtrl-0965ca5755c1.elf, per
# docs/audits/profile_executor_panic_recurrence_2026-09-14.md) but was gone
# by the next day's symbolization attempt
# (docs/audits/profile_executor_coredump_2026-09-15.md): manifest.json,
# flash_provenance.json and the archived ELF were ALL missing at once, and
# build/'s own CMakeCache.txt/config.env timestamps show the directory was
# reconfigured from scratch shortly before that flash -- i.e. a clean/
# fresh-configure of build/ (idf.py fullclean, or an equivalent manual wipe)
# silently took the "durable" archive out with it, because it lived inside
# the one directory a clean is specifically supposed to empty. Moving it
# outside build/ (still gitignored, still never committed) is the actual
# fix; nothing else -- not _prune, not a kiln_fw_root override -- was
# responsible this time, and the lost ELF could not be recovered from any
# worktree on disk (searched, no match on content or embedded build string).
#
# POST_BUILD on the app target rather than a separate target: it must run
# every time the ELF is relinked, including when nothing else changed.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
        -DOUTDIR=${CMAKE_CURRENT_LIST_DIR}/elf_archive
        -P ${CMAKE_CURRENT_LIST_DIR}/archive_elf.cmake
    VERBATIM
)
