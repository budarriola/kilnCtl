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
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_ROOT}
        OUTPUT_VARIABLE commit_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE commit_result
        ERROR_QUIET
    )
    if(commit_result EQUAL 0 AND commit_out)
        set(commit ${commit_out})
    endif()

    # Scoped to firmware/SaftyFW only (THIS_PROJECT_DIR), not the whole
    # kilnCtl monorepo -- this project shares a repo with KiCad hardware
    # files, KilnFW, CommonFW, docs, etc. that get their own in-progress
    # changes; those must not mark SaftyFW's own build as dirty. Matches
    # KilnFW's gen_build_info.cmake, same rationale.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain -- "${THIS_PROJECT_DIR}"
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
