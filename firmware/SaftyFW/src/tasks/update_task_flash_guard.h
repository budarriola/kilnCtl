// update_task_flash_guard.h -- pure overlap arithmetic behind a hard refusal
// to erase or program any flash region that overlaps the RUNNING image's own
// extent.
//
// Triage finding (2026-09-21, opus): the bench Pico runs a flat, bootloader-
// less image loaded directly at XIP_BASE (flash offset 0), size 0x1D204. That
// extent overlaps BOOTLOADER_METADATA_FLASH_OFFSET (0x10000..0x11000) and the
// first 0xC204 bytes of slot A (0x11000..0x1D204). update_task_process_begin()
// (update_task.c) falls back to a bootstrap default metadata record whenever
// the metadata sector holds no valid record (update_task_read_latest_
// metadata_or_default()) -- active_slot = A, target = B -- erases slot B (no
// overlap, harmless), then calls update_task_persist_metadata(), which erases
// and programs BOOTLOADER_METADATA_FLASH_OFFSET. On that flat image, this
// program call overwrites part of the code and data the chip is CURRENTLY
// EXECUTING FROM, mid-flight. The board answered ERASING and then went
// silent for 120 s -- consistent with corrupting its own running image
// rather than a benign timeout.
//
// This module is the pure decision half: given the running image's own flash
// extent (as reported by the linker -- see update_task.c's
// update_task_running_image_flash_range() wrapper, which reads
// __flash_binary_start/__flash_binary_end, the same symbols
// bootloader/app_slot.ld.in defines for every slot-linked build and the
// stock pico-sdk linker script defines for a flat build alike, since both
// describe "where this exact running binary's code and data actually sit in
// flash" -- a fact XIP execution makes true by construction, not a guess),
// answers one question: would touching [region_offset, region_offset+
// region_size) hit any byte of that extent? If so, the caller must refuse --
// unconditionally, regardless of which flash operation (erase or program) or
// which region (metadata sector, either slot) is about to run.
//
// Pure, no RTOS/SDK/HAL dependency -- host-testable, same discipline as
// update_task_slot_linkage.h/update_task_erase_plan.h.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_FLASH_GUARD_H
#define SAFTYFW_TASKS_UPDATE_TASK_FLASH_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true iff the half-open byte range [region_offset, region_offset +
// region_size) -- flash-relative, the same convention as flash_layout.h's
// own offsets (add XIP_BASE for an execute-in-place pointer) -- overlaps the
// running image's own half-open extent [running_image_start_offset,
// running_image_end_offset), also flash-relative.
//
// region_size == 0 never overlaps anything (an empty range touches no
// bytes). A malformed running-image extent (end <= start) is treated the
// same way -- FAILS CLOSED as "no image extent is known", which is exactly
// wrong for THIS guard's purpose (a guard whose job is refusing on overlap
// must not silently report "no overlap" just because it could not determine
// its own extent) -- see update_task_flash_guard_running_image_extent_valid()
// below, which every caller must check FIRST and refuse outright if it ever
// returns false, precisely so this function is never asked to answer with a
// malformed extent in the first place.
bool update_task_flash_guard_overlaps(uint32_t region_offset, uint32_t region_size,
                                       uint32_t running_image_start_offset,
                                       uint32_t running_image_end_offset);

// A running-image extent is only usable as evidence if it is well-formed:
// end strictly greater than start (a non-empty extent), and both flash-
// relative offsets (i.e. already had XIP_BASE subtracted, so they compare
// directly against flash_layout.h's own offsets). A caller whose linker-
// symbol read produces anything else (e.g. a build where the two symbols
// somehow came back equal or reversed) cannot trust update_task_flash_guard_
// overlaps() and must refuse the operation outright rather than let a
// malformed extent silently mean "nothing overlaps".
bool update_task_flash_guard_running_image_extent_valid(uint32_t running_image_start_offset,
                                                          uint32_t running_image_end_offset);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UPDATE_TASK_FLASH_GUARD_H
