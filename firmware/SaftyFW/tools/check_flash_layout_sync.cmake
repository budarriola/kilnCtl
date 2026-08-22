# check_flash_layout_sync.cmake -- TODO.md Phase 10's "*** MANUAL SYNC HAZARD
# ***" check: bootloader/flash_layout.h's slot offsets/size are hand-copied
# into (a) bootloader/app_slot.ld.in's FLASH LENGTH literal and (b) the two
# saftyfw_add_slot_executable() FLASH_ORIGIN literals in CMakeLists.txt.
# Nothing enforced they stayed in sync until this check.
#
# Invoked via add_custom_target ALL (CMakeLists.txt), same "runs every build,
# not just on reconfigure" reasoning as gen_build_info.cmake in this
# directory -- a hand-edit to any of the three files must be caught on the
# very next build, not only after someone happens to touch CMakeLists.txt
# and trigger a reconfigure.
#
# Expected -D arguments:
#   FLASH_LAYOUT_HEADER -- path to bootloader/flash_layout.h
#   LD_TEMPLATE         -- path to bootloader/app_slot.ld.in
#   SLOT_A_ORIGIN       -- the FLASH_ORIGIN literal passed to
#                           saftyfw_add_slot_executable(SaftyFW_slotA ...)
#                           in CMakeLists.txt, e.g. 0x10011000
#   SLOT_B_ORIGIN       -- same, for SaftyFW_slotB, e.g. 0x100E1000
#   XIP_BASE            -- pico-sdk's XIP_BASE, 0x10000000 (flash_layout.h's
#                           offsets are flash-relative; CMakeLists.txt's
#                           origins are XIP-mapped -- see that header's own
#                           comment for why)
#
# Exits (FATAL_ERROR, non-zero) with the specific mismatched numbers if any
# of the three sources of truth disagree.

if(NOT FLASH_LAYOUT_HEADER OR NOT EXISTS ${FLASH_LAYOUT_HEADER})
    message(FATAL_ERROR "check_flash_layout_sync: FLASH_LAYOUT_HEADER not found: ${FLASH_LAYOUT_HEADER}")
endif()
if(NOT LD_TEMPLATE OR NOT EXISTS ${LD_TEMPLATE})
    message(FATAL_ERROR "check_flash_layout_sync: LD_TEMPLATE not found: ${LD_TEMPLATE}")
endif()

file(STRINGS ${FLASH_LAYOUT_HEADER} header_lines)

function(extract_hex_macro out_var macro_name)
    foreach(line ${header_lines})
        if(line MATCHES "^#define[ \t]+${macro_name}[ \t]+(0x[0-9A-Fa-f]+)u?")
            set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "check_flash_layout_sync: could not find #define ${macro_name} in ${FLASH_LAYOUT_HEADER}")
endfunction()

extract_hex_macro(header_slot_a_offset "BOOTLOADER_SLOT_A_FLASH_OFFSET")
extract_hex_macro(header_slot_b_offset "BOOTLOADER_SLOT_B_FLASH_OFFSET")
extract_hex_macro(header_slot_size     "BOOTLOADER_SLOT_FLASH_SIZE")

# app_slot.ld.in's FLASH LENGTH literal, e.g. "LENGTH = 0xD0000"
file(STRINGS ${LD_TEMPLATE} ld_lines)
set(ld_length "")
foreach(line ${ld_lines})
    if(line MATCHES "FLASH\\(rx\\)[ \t]*:.*LENGTH[ \t]*=[ \t]*(0x[0-9A-Fa-f]+)")
        set(ld_length "${CMAKE_MATCH_1}")
        break()
    endif()
endforeach()
if(NOT ld_length)
    message(FATAL_ERROR "check_flash_layout_sync: could not find FLASH(rx) ... LENGTH = 0x... in ${LD_TEMPLATE}")
endif()

set(mismatches "")

# --- Slot size: flash_layout.h's BOOTLOADER_SLOT_FLASH_SIZE vs app_slot.ld.in's LENGTH literal ---
math(EXPR header_size_dec "${header_slot_size}")
math(EXPR ld_length_dec "${ld_length}")
if(NOT header_size_dec EQUAL ld_length_dec)
    list(APPEND mismatches
        "slot size: flash_layout.h BOOTLOADER_SLOT_FLASH_SIZE=${header_slot_size} (${header_size_dec}) "
        "!= app_slot.ld.in FLASH LENGTH=${ld_length} (${ld_length_dec})")
endif()

# --- Slot A/B origins: flash_layout.h's offset + XIP_BASE vs CMakeLists.txt's literal ---
if(NOT XIP_BASE)
    set(XIP_BASE "0x10000000")
endif()
math(EXPR xip_base_dec "${XIP_BASE}")

math(EXPR header_a_offset_dec "${header_slot_a_offset}")
math(EXPR expected_a_origin_dec "${header_a_offset_dec} + ${xip_base_dec}")
if(SLOT_A_ORIGIN)
    math(EXPR cmake_a_origin_dec "${SLOT_A_ORIGIN}")
    if(NOT expected_a_origin_dec EQUAL cmake_a_origin_dec)
        math(EXPR expected_a_origin_hex "${expected_a_origin_dec}" OUTPUT_FORMAT HEXADECIMAL)
        list(APPEND mismatches
            "slot A origin: flash_layout.h BOOTLOADER_SLOT_A_FLASH_OFFSET=${header_slot_a_offset} + XIP_BASE=${XIP_BASE} "
            "= ${expected_a_origin_hex} (${expected_a_origin_dec}) != CMakeLists.txt SaftyFW_slotA origin=${SLOT_A_ORIGIN} (${cmake_a_origin_dec})")
    endif()
else()
    message(FATAL_ERROR "check_flash_layout_sync: SLOT_A_ORIGIN not passed")
endif()

math(EXPR header_b_offset_dec "${header_slot_b_offset}")
math(EXPR expected_b_origin_dec "${header_b_offset_dec} + ${xip_base_dec}")
if(SLOT_B_ORIGIN)
    math(EXPR cmake_b_origin_dec "${SLOT_B_ORIGIN}")
    if(NOT expected_b_origin_dec EQUAL cmake_b_origin_dec)
        math(EXPR expected_b_origin_hex "${expected_b_origin_dec}" OUTPUT_FORMAT HEXADECIMAL)
        list(APPEND mismatches
            "slot B origin: flash_layout.h BOOTLOADER_SLOT_B_FLASH_OFFSET=${header_slot_b_offset} + XIP_BASE=${XIP_BASE} "
            "= ${expected_b_origin_hex} (${expected_b_origin_dec}) != CMakeLists.txt SaftyFW_slotB origin=${SLOT_B_ORIGIN} (${cmake_b_origin_dec})")
    endif()
else()
    message(FATAL_ERROR "check_flash_layout_sync: SLOT_B_ORIGIN not passed")
endif()

list(LENGTH mismatches n_mismatches)
if(n_mismatches GREATER 0)
    string(REPLACE ";" "" mismatch_text "${mismatches}")
    message(FATAL_ERROR "check_flash_layout_sync: FLASH LAYOUT OUT OF SYNC -- ${mismatch_text}")
endif()

message(STATUS "check_flash_layout_sync: flash_layout.h, app_slot.ld.in and CMakeLists.txt slot literals agree.")
