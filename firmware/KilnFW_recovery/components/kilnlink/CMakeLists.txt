# Thin ESP-IDF component over firmware/CommonFW's kilnlink framing -- only the
# two files the recovery image's Pico relay needs (CRC16 + frame encode/
# decode/stuffing). It holds no source of its own; the code is linked from
# CommonFW so there is exactly one framing implementation in the tree
# (CommonFW/README.md rule 7). Deliberately NOT the full command-codec list
# that firmware/KilnFW/components/kilnlink carries: the recovery image hand-
# builds its handful of update frames (recovery_pico_proto.c).
set(KILNLINK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../CommonFW")

idf_component_register(
    SRCS "${KILNLINK_DIR}/src/kilnlink_crc.c"
         "${KILNLINK_DIR}/src/kilnlink_frame.c"
    INCLUDE_DIRS "${KILNLINK_DIR}/include"
)
