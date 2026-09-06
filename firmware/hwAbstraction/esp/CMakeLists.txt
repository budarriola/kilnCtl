# This directory is NOT an ESP-IDF component (no idf_component_register
# here). The real component registration lives at
# firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt -- a thin
# wrapper directory that lists these same sources with a "../../esp/"
# prefix.
#
# Why the split: ESP-IDF derives a component's name from its directory name,
# and that name lives in ESP-IDF's flat global namespace (shared across
# every component from every project and managed dependency). Registering
# straight out of this directory named it the bare, collision-prone "esp"
# (asymmetric with SaftyFW's "hwabstraction_pico" static-library target).
# Renaming this directory itself was rejected because firmware/hwAbstraction/
# is a tree shared with other consumers (docs/HW_ABSTRACTION_PLAN.md, the
# pico/, host/, and test/ siblings) that key off this exact path -- so the
# name fix is confined to one new wrapper directory instead of moving files.
#
# See firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt for the
# SRCS/INCLUDE_DIRS/REQUIRES history (HAL Phase 1a/1b) and
# firmware/hwAbstraction/README.md for the tree layout.
