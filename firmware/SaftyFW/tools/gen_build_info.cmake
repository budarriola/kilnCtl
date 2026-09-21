# gen_build_info.cmake -- SaftyFW's own copy of the KilnFW build-identity
# generator (firmware/KilnFW/App/drivers/gen_build_info.cmake), same
# reasoning throughout:
#
# Invoked via add_custom_target (CMakeLists.txt), not add_custom_command --
# add_custom_target has no dependency tracking, so Ninja/Make always reruns
# it on *every* build invocation. A plain add_custom_command keyed on file
# timestamps would only rerun when one of its DEPENDS changed (e.g. a source
# file), which is wrong here: the git commit/dirty state and the build
# timestamp can both change between two otherwise-identical builds (a new
# commit landed elsewhere in the tree; time simply passed) without touching
# any file this component compiles. Regenerating unconditionally is what
# keeps this header from going stale.
#
# PROJECT_ROOT is the git working tree root (passed in via -DPROJECT_ROOT=...),
# THIS_PROJECT_DIR is firmware/SaftyFW itself (-DTHIS_PROJECT_DIR=..., used to
# scope the dirty check -- see below), OUT is the header path to write
# (-DOUT=...), and SIM_PLANT_HONORED is the resolved 0/1 value of the
# SAFTYFW_HONOR_SIM_PLANT CMake option (-DSIM_PLANT_HONORED=..., see
# CMakeLists.txt's own comment on that option for why this needs to be
# visible in the build's own identity reporting rather than only in the
# CMake cache).
if(NOT DEFINED SIM_PLANT_HONORED)
    set(SIM_PLANT_HONORED 0)
endif()

find_package(Git QUIET)

set(commit "unknown")
set(dirty "1")

if(GIT_EXECUTABLE)
    # Path-scoped to every tree this build actually compiles or includes
    # from: firmware/SaftyFW (THIS_PROJECT_DIR), firmware/CommonFW
    # (`add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../CommonFW kilnlink)`),
    # and ONLY three subdirectories of firmware/hwAbstraction --
    # hwAbstraction/pico (`add_library(hwabstraction_pico ...)`, further
    # down this same CMakeLists.txt, linked into every SaftyFW target),
    # hwAbstraction/common (hal_status.c, compiled into that same library),
    # and hwAbstraction/interface (the headers that library's
    # target_include_directories exposes) -- NOT firmware/hwAbstraction as a
    # whole: esp/, host/, idf/, test/ and README.md live under that same
    # directory but never reach the Pico image, and scoping to the whole
    # directory made an edit to any of those spuriously restamp
    # SAFTYFW_GIT_COMMIT (review round 3, docs/PICO_AUTO_UPDATE_PLAN.md sec
    # 13: 33 of the last 62 hwAbstraction-only commits touched only those
    # unreached subtrees). This is also NOT `rev-parse --short HEAD` of the
    # whole monorepo. A plain repo-wide HEAD changes on every commit
    # anywhere in the tree -- including KilnFW-only commits that touch
    # nothing SaftyFW reads -- so the boot-time identity comparison in
    # pico_auto_update.h (firmware/KilnFW/.../pico_auto_update.h) saw a
    # "mismatch" on every KilnFW flash and tried to reflash the Pico with a
    # byte-identical image every time. `git log -1` over exactly the paths
    # this build depends on only changes when one of them actually does.
    # This exact five-path list is kept in sync in two other places -- see
    # the drift test that enforces it
    # (tools/PcTools/tests/test_pico_image_freshness.py::test_scoped_paths_match_cmake_and_stale_check):
    #   tools/PcTools/src/kilnctrl/pico_image_freshness.py's SCOPED_PATHS
    #   tools/PcTools/src/kilnctrl/stale_check.py's check_saftyfw_stale() project_dirs
    set(_commonfw_dir "${PROJECT_ROOT}/firmware/CommonFW")
    set(_hwabstraction_pico_dir "${PROJECT_ROOT}/firmware/hwAbstraction/pico")
    set(_hwabstraction_common_dir "${PROJECT_ROOT}/firmware/hwAbstraction/common")
    set(_hwabstraction_interface_dir "${PROJECT_ROOT}/firmware/hwAbstraction/interface")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%h -- "${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_pico_dir}" "${_hwabstraction_common_dir}" "${_hwabstraction_interface_dir}"
        WORKING_DIRECTORY ${PROJECT_ROOT}
        OUTPUT_VARIABLE commit_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE commit_result
        ERROR_QUIET
    )
    if(commit_result EQUAL 0 AND commit_out)
        set(commit ${commit_out})
    endif()

    # Dirty flag scoped to the SAME five paths as the commit above (it used
    # to be scoped to THIS_PROJECT_DIR alone, under-covering CommonFW and
    # hwAbstraction relative to what the commit field claims as inputs -- a
    # CommonFW/hwAbstraction-only uncommitted edit would silently not mark
    # the build dirty). Still excludes the rest of the kilnCtl monorepo
    # (KiCad hardware files, KilnFW, docs, etc.) -- those must not mark
    # SaftyFW's own build as dirty. Matches KilnFW's gen_build_info.cmake,
    # same rationale.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain -- "${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_pico_dir}" "${_hwabstraction_common_dir}" "${_hwabstraction_interface_dir}"
        WORKING_DIRECTORY ${PROJECT_ROOT}
        OUTPUT_VARIABLE status_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status_result
        ERROR_QUIET
    )
    if(status_result EQUAL 0)
        if(status_out STREQUAL "")
            set(dirty "0")
        else()
            set(dirty "1")
        endif()
    endif()
endif()

string(TIMESTAMP build_date "%Y-%m-%d" UTC)
string(TIMESTAMP build_time "%H:%M:%SZ" UTC)

# Macro names are SAFTYFW_-prefixed (not FW_GIT_* as in KilnFW's header) so
# both headers can be included in the same translation unit without a clash
# -- deliberate, since nothing today needs that, but nothing rules it out
# either (e.g. a future shared diagnostics tool linking both build_info
# headers).
# Built up in one variable and written ONCE via a temp-file + file(RENAME),
# not via WRITE + several APPENDs against ${OUT} directly -- see KilnFW's
# gen_build_info.cmake (firmware/KilnFW/App/drivers/gen_build_info.cmake)
# for the interleaved-write corruption this avoids. RENAME is atomic on both
# Windows (MoveFileEx) and POSIX, so a concurrent reader/compiler always
# sees either the complete old header or the complete new one.
set(_content "// Auto-generated on every build by gen_build_info.cmake -- do not edit, do not commit.\n")
string(APPEND _content "// Identity for SaftyFW itself (the RP2040 safety-processor firmware), not KilnFW.\n")
string(APPEND _content "#ifndef SAFTYFW_BUILD_INFO_H\n#define SAFTYFW_BUILD_INFO_H\n\n")
string(APPEND _content "// Short git commit hash (\"unknown\" if git or the repo was unavailable at build time).\n")
string(APPEND _content "#define SAFTYFW_GIT_COMMIT \"${commit}\"\n")
string(APPEND _content "// 1 if firmware/SaftyFW had uncommitted changes at build time (or git/the repo was\n")
string(APPEND _content "// unavailable -- unknown safely maps to dirty), 0 if the working tree was clean.\n")
string(APPEND _content "#define SAFTYFW_GIT_DIRTY ${dirty}\n")
string(APPEND _content "// UTC build date/time, wall-clock at the moment this header was generated.\n")
string(APPEND _content "#define SAFTYFW_BUILD_DATE \"${build_date}\"\n")
string(APPEND _content "#define SAFTYFW_BUILD_TIME \"${build_time}\"\n")
string(APPEND _content "// 1 if this build was configured with -DSAFTYFW_HONOR_SIM_PLANT=ON (BENCH-ONLY:\n")
string(APPEND _content "// safety_guards.c will disable S2/S3/S4 when the ESP's context reports\n")
string(APPEND _content "// SIM_PLANT), 0 in every production/target build (the default). Never set from\n")
string(APPEND _content "// runtime state -- this is the compile-time fact, so a sim-gated image can be\n")
string(APPEND _content "// identified from this header alone and never mistaken for a production one.\n")
string(APPEND _content "// See CMakeLists.txt's SAFTYFW_HONOR_SIM_PLANT comment.\n")
string(APPEND _content "#define SAFTYFW_BUILD_SIM_PLANT_HONORED ${SIM_PLANT_HONORED}\n")
string(APPEND _content "\n#endif // SAFTYFW_BUILD_INFO_H\n")

string(RANDOM LENGTH 8 _nonce)
set(_tmp "${OUT}.tmp.${_nonce}")
file(WRITE ${_tmp} "${_content}")
file(RENAME ${_tmp} ${OUT})
