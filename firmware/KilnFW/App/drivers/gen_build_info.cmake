# Regenerates build_info.h with the current git commit/dirty state and the
# wall-clock build time. Invoked via a custom target with no tracked
# dependencies (see CMakeLists.txt), so CMake/Ninja always reruns it on
# every build invocation rather than only when something it depends on
# changed -- a plain add_custom_command keyed on file timestamps would go
# stale between builds that don't touch this component's sources.
#
# PROJECT_ROOT is expected to be passed in via -DPROJECT_ROOT=... (the git
# working tree root, several directories above this component) and OUT via
# -DOUT=... (the header path to write).

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

    # Scoped to this project's own directory (CMAKE_CURRENT_SOURCE_DIR's
    # parent, i.e. the UnitTest project root), not the whole kilnCtl repo --
    # this project lives in a much larger monorepo with unrelated
    # subprojects (PCB files, other boards, etc.) that get their own
    # in-progress changes; those shouldn't mark *this* firmware's build as
    # dirty.
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

# WP8 (GITHUB_RELEASE_UPDATE_PLAN.md): identity the board compares a fetched release against.
# FW_RELEASE_VERSION is the release tag, exported as KILNCTL_RELEASE_VERSION by
# tools/make_release.ps1 (empty for any other build, so a dev build never claims a version).
# FW_PARTITIONS_SHA256 is sha256 of partitions.csv with CRLF normalised, exactly as
# tools/release_manifest.py computes compat.partitions_sha256.
set(release_version "")
if(DEFINED ENV{KILNCTL_RELEASE_VERSION} AND NOT "$ENV{KILNCTL_RELEASE_VERSION}" STREQUAL "")
    set(release_version "$ENV{KILNCTL_RELEASE_VERSION}")
    # Must accept exactly what update_tag_valid() (update_url.c, via update_semver.c) accepts:
    # core numbers 0 or 1-9 digits without a leading zero, a prerelease of non-empty dot-separated
    # [0-9A-Za-z-] identifiers (so '-' is allowed inside: "v1.2.3-rc-1") whose all-digit identifiers
    # have no leading zero, no build metadata, 6..32 characters in all. CMake regexes have no {n,m},
    # hence the spelled-out digit limit. tools/check_release_version_regex.ps1 reads the two
    # _RV_* variables below and keeps them in agreement with the host-test tag table.
    set(_RV_TAG_REGEX "^v(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?[0-9]?)(-[0-9A-Za-z.-]+)?$")
    set(_RV_REJECT_REGEXES "\\.\\.;^v[0-9.]+-\\.;\\.$;^v[0-9.]+-$;(^v[0-9.]+-|\\.)0[0-9]+(\\.|$)")
    string(LENGTH "${release_version}" _rv_len)
    set(_rv_ok TRUE)
    if(NOT release_version MATCHES "${_RV_TAG_REGEX}" OR _rv_len GREATER 32)
        set(_rv_ok FALSE)
    endif()
    foreach(_rv_rx IN LISTS _RV_REJECT_REGEXES)
        if(release_version MATCHES "${_rv_rx}")
            set(_rv_ok FALSE)
        endif()
    endforeach()
    if(NOT _rv_ok)
        # A release build that asked for a version must get it or fail: silently baking an empty
        # running version would make the board unable to tell an upgrade from a downgrade.
        message(FATAL_ERROR "KILNCTL_RELEASE_VERSION='${release_version}' is not a valid release tag "
                            "(vMAJOR.MINOR.PREREL as update_tag_valid() accepts, at most 32 characters).")
    endif()
endif()
set(partitions_sha "")
if(EXISTS "${THIS_PROJECT_DIR}/partitions.csv")
    file(READ "${THIS_PROJECT_DIR}/partitions.csv" _praw)
    string(REPLACE "\r\n" "\n" _praw "${_praw}")
    string(SHA256 partitions_sha "${_praw}")
endif()

string(TIMESTAMP build_date "%Y-%m-%d" UTC)
string(TIMESTAMP build_time "%H:%M:%SZ" UTC)

# Built up in one variable and written ONCE, not via a WRITE + several
# APPENDs against ${OUT} directly. Two overlapping build invocations (e.g.
# two concurrent build_kilnfw MCP calls, or a stray reconfigure racing a
# build) each running this target can interleave their WRITE/APPEND calls,
# which is exactly what produced a build_info.h containing a truncated
# `#define FW_GIT_DIRTY` with the tail of another line spliced in --
# "unknown type name 'by'" from the header comment. Writing to a temp file
# in the same directory and then file(RENAME)-ing it over ${OUT} makes the
# header atomic: MoveFileEx-on-Windows / rename(2)-on-POSIX replace the
# destination in one filesystem operation, so a concurrent reader/compiler
# always sees either the old complete header or the new complete header,
# never a partial one. The temp name includes the CMake process id so two
# overlapping runs don't also collide on the temp file itself.
set(_content "// Auto-generated on every build by gen_build_info.cmake -- do not edit, do not commit.\n")
string(APPEND _content "#ifndef BUILD_INFO_H\n#define BUILD_INFO_H\n\n")
string(APPEND _content "#define FW_GIT_COMMIT \"${commit}\"\n")
string(APPEND _content "#define FW_GIT_DIRTY ${dirty}\n")
string(APPEND _content "#define FW_RELEASE_VERSION \"${release_version}\"
")
string(APPEND _content "#define FW_PARTITIONS_SHA256 \"${partitions_sha}\"
")
string(APPEND _content "#define FW_BUILD_DATE \"${build_date}\"\n")
string(APPEND _content "#define FW_BUILD_TIME \"${build_time}\"\n")
string(APPEND _content "\n#endif // BUILD_INFO_H\n")

string(RANDOM LENGTH 8 _nonce)
set(_tmp "${OUT}.tmp.${_nonce}")
file(WRITE ${_tmp} "${_content}")
file(RENAME ${_tmp} ${OUT})
