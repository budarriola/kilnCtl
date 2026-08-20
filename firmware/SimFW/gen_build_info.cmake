# Regenerates build_info.h with the current git commit/dirty state and the
# wall-clock build time. Invoked via a custom target with no tracked
# dependencies (see CMakeLists.txt), so CMake/Ninja always reruns it on every
# build invocation rather than only when something it depends on changed -- a
# plain add_custom_command keyed on file timestamps would go stale between
# builds that don't touch this project's sources.
#
# Adapted verbatim in shape from ../KilnFW/App/drivers/gen_build_info.cmake
# (that file's own header comment explains the design; this is the same
# script, just scoped to SimFW's own directory for the dirty check instead of
# UnitTestFw's). cmd_task's SYS GET_VERSION/GET_CAPS handlers
# (src/tasks/cmd_task.c) are what consume the generated header.
#
# PROJECT_ROOT is expected to be passed in via -DPROJECT_ROOT=... (the git
# working tree root) and OUT via -DOUT=... (the header path to write).
# THIS_PROJECT_DIR scopes the dirty check to this project's own directory,
# not the whole kilnCtl monorepo -- unrelated in-progress changes elsewhere
# in the repo (PCB files, other boards) must not mark this firmware's build
# dirty.

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

file(WRITE ${OUT} "// Auto-generated on every build by gen_build_info.cmake -- do not edit, do not commit.\n")
file(APPEND ${OUT} "#ifndef SIMFW_BUILD_INFO_H\n#define SIMFW_BUILD_INFO_H\n\n")
file(APPEND ${OUT} "#define SIMFW_FW_GIT_COMMIT \"${commit}\"\n")
file(APPEND ${OUT} "#define SIMFW_FW_GIT_DIRTY ${dirty}\n")
file(APPEND ${OUT} "#define SIMFW_FW_BUILD_DATE \"${build_date}\"\n")
file(APPEND ${OUT} "#define SIMFW_FW_BUILD_TIME \"${build_time}\"\n")
file(APPEND ${OUT} "\n#endif // SIMFW_BUILD_INFO_H\n")
