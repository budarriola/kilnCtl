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
# scope the dirty check -- see below), and OUT is the header path to write
# (-DOUT=...).

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
file(WRITE ${OUT} "// Auto-generated on every build by gen_build_info.cmake -- do not edit, do not commit.\n")
file(APPEND ${OUT} "// Identity for SaftyFW itself (the RP2040 safety-processor firmware), not KilnFW.\n")
file(APPEND ${OUT} "#ifndef SAFTYFW_BUILD_INFO_H\n#define SAFTYFW_BUILD_INFO_H\n\n")
file(APPEND ${OUT} "// Short git commit hash (\"unknown\" if git or the repo was unavailable at build time).\n")
file(APPEND ${OUT} "#define SAFTYFW_GIT_COMMIT \"${commit}\"\n")
file(APPEND ${OUT} "// 1 if firmware/SaftyFW had uncommitted changes at build time (or git/the repo was\n")
file(APPEND ${OUT} "// unavailable -- unknown safely maps to dirty), 0 if the working tree was clean.\n")
file(APPEND ${OUT} "#define SAFTYFW_GIT_DIRTY ${dirty}\n")
file(APPEND ${OUT} "// UTC build date/time, wall-clock at the moment this header was generated.\n")
file(APPEND ${OUT} "#define SAFTYFW_BUILD_DATE \"${build_date}\"\n")
file(APPEND ${OUT} "#define SAFTYFW_BUILD_TIME \"${build_time}\"\n")
file(APPEND ${OUT} "\n#endif // SAFTYFW_BUILD_INFO_H\n")
