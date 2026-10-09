# KilnFW_recovery -- the OTA recovery image, docs/OTA_SINGLE_SLOT.md
# section 8 step 1. Deliberately a SEPARATE IDF project from firmware/KilnFW
# (not a component of it) so its much smaller content list can never
# accidentally drift into a copy of the main OTA application image -- see
# the plan's section 1 "Measured, not assumed" note and section 3's content
# bar: "anything beyond receive-an-image-and-write-it must justify itself".
#
# Build like any other ESP-IDF project, from a PowerShell shell with the IDF
# environment exported (idf.py silently no-ops under git-bash/MSYS -- see
# CLAUDE.md's "Firmware gotchas"):
#
#   . C:\esp\v6.0.2\esp-idf\export.ps1
#   idf.py -C firmware\KilnFW_recovery set-target esp32s3
#   idf.py -C firmware\KilnFW_recovery build
#
# Output lands at firmware/KilnFW_recovery/build/recovery.bin (this
# project's PROJECT_NAME, below), which is
# tools/check_recovery_image_size.py's default --recovery-bin path.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(recovery)
