# ESP-IDF component wrapper around firmware/CommonFW's kilnlink -- the shared
# wire-protocol library between KilnFW and SaftyFW. This directory holds no
# source of its own; it only points idf_component_register at CommonFW so the
# same code compiles under ESP-IDF's xtensa-gcc, matching what pico-sdk's
# add_subdirectory(../CommonFW kilnlink) will do on the SaftyFW side. See
# firmware/CommonFW/README.md for the freestanding-C11 rules that make this
# possible and firmware/CommonFW/README.md's "Integration" section for why
# the wrapper exists rather than pointing ESP-IDF at CommonFW's own
# CMakeLists.txt directly (idf_component_register has its own registration
# protocol that a plain add_library-based CMakeLists.txt doesn't speak).
#
# Not yet consumed by anything in App/drivers -- KilnFW/App/drivers/CMakeLists.txt
# lists "kilnlink" in REQUIRES purely so `idf.py build` compiles this
# component under the real xtensa-gcc toolchain and catches anything MSVC's
# host build didn't. App/drivers/espInterfaces/uart_protocol.c has not been
# migrated to call kilnlink_frame_* yet -- CommonFW/README.md is explicit
# that migration needs proven byte-identical output against the
# pre-refactor implementation before the old code is deleted, which is a
# dedicated verification pass of its own, not bundled with wiring the build.
set(KILNLINK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../CommonFW")

idf_component_register(
    SRCS "${KILNLINK_DIR}/src/kilnlink_crc.c"
         "${KILNLINK_DIR}/src/kilnlink_frame.c"
         "${KILNLINK_DIR}/src/kilnlink_context.c"
         "${KILNLINK_DIR}/src/kilnlink_announce.c"
         "${KILNLINK_DIR}/src/kilnlink_clear_trip.c"
         "${KILNLINK_DIR}/src/kilnlink_set_config.c"
         "${KILNLINK_DIR}/src/kilnlink_rollback.c"
         "${KILNLINK_DIR}/src/kilnlink_announce_reboot.c"
    INCLUDE_DIRS "${KILNLINK_DIR}/include"
)
