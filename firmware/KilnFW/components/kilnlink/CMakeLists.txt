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
# host build didn't. App/drivers/owners/uart_protocol.c (formerly
# App/drivers/espInterfaces/uart_protocol.c before the drivers/ layering
# reorg flattened espInterfaces/ into owners/) has not been migrated to
# call kilnlink_frame_* yet -- CommonFW/README.md is explicit
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
         "${KILNLINK_DIR}/src/kilnlink_set_ct_cal.c"
         "${KILNLINK_DIR}/src/kilnlink_get_ct_cal.c"
         "${KILNLINK_DIR}/src/kilnlink_ct_cal.c"
         # docs/COMMISSIONING.md sec 2/3 (2026-08-21 follow-up): the ESP-side
         # commissioning cache/HTTP surface (App/drivers/safety/safety_cfg_store.c,
         # App/drivers/http/safety_cfg_http.c) and safety_link.c's SET_PARAM/COMMIT_CONFIG/
         # GET_CONFIG_PAGE wire calls need these five -- added here for the
         # identical reason every entry above this line was added: this
         # wrapper's SRCS list does not track CommonFW/CMakeLists.txt's own
         # add_library(kilnlink ...) automatically, so a new codec is invisible
         # to the ESP-IDF build until it is also listed here. (Several older
         # kilnlink_*.c files -- status/power/diag/trip/ceiling/set_clock/
         # get_fw_version/set_log_level -- are ALSO missing from this list,
         # pre-existing gaps this pass did not touch since safety_link.c
         # hand-parses those frames rather than linking their codecs; see that
         # file's own comments, e.g. safety_apply_diag()'s header, for why.)
         "${KILNLINK_DIR}/src/kilnlink_param_value.c"
         "${KILNLINK_DIR}/src/kilnlink_set_param.c"
         "${KILNLINK_DIR}/src/kilnlink_commit_config.c"
         "${KILNLINK_DIR}/src/kilnlink_get_config_page.c"
         "${KILNLINK_DIR}/src/kilnlink_config_page.c"
         # SET_LOG_LEVEL now has a real ESP caller (safety_link.c's
         # safety_link_send_set_log_level(), ROADMAP.md "no ESP caller" loose
         # end) so it moves out of this comment's "hand-parsed, not linked"
         # list above and into the actually-linked set below. COMMIT_CONFIG_REJECTED
         # is the new Pico -> ESP reply safety_link.c decodes for the
         # per-field commissioning rejection reason (COMMISSIONING.md sec 3.1).
         "${KILNLINK_DIR}/src/kilnlink_set_log_level.c"
         "${KILNLINK_DIR}/src/kilnlink_commit_config_rejected.c"
         # SAFETY_CMD_ROLLBACK_RESULT (0x25) -- the missing Pico->ESP reply
         # for a refused SAFETY_CMD_ROLLBACK, safety_link.c decodes this the
         # same way it decodes COMMIT_CONFIG_REJECTED above.
         "${KILNLINK_DIR}/src/kilnlink_rollback_result.c"
         # SAFETY_CMD_REBOOT (0x29) / SAFETY_CMD_REBOOT_RESULT (0x2A) -- the
         # reboot-in-place command behind POST /api/sw_reset
         # (App/drivers/http/sw_reset_http.c via safety_link_send_reboot()).
         "${KILNLINK_DIR}/src/kilnlink_reboot.c"
         "${KILNLINK_DIR}/src/kilnlink_reboot_result.c"
         # CT_COMMISSIONING_PLAN.md step 2 -- SAFETY_CMD_CT_AUTO_ZERO_BEGIN
         # (0x26) / SAFETY_CMD_GET_CT_AUTO_ZERO (0x27) / SAFETY_CMD_CT_AUTO_
         # ZERO_STATUS (0x28 reply), safety_link_commands.c's safety_link_
         # send_ct_auto_zero_begin()/safety_link_get_ct_auto_zero_status().
         "${KILNLINK_DIR}/src/kilnlink_ct_auto_zero_begin.c"
         "${KILNLINK_DIR}/src/kilnlink_get_ct_auto_zero.c"
         "${KILNLINK_DIR}/src/kilnlink_ct_auto_zero_status.c"
    INCLUDE_DIRS "${KILNLINK_DIR}/include"
)
