// update_task_flash_guard.c -- see update_task_flash_guard.h.
#include "update_task_flash_guard.h"

bool update_task_flash_guard_running_image_extent_valid(uint32_t running_image_start_offset,
                                                          uint32_t running_image_end_offset)
{
    return running_image_end_offset > running_image_start_offset;
}

bool update_task_flash_guard_overlaps(uint32_t region_offset, uint32_t region_size,
                                       uint32_t running_image_start_offset,
                                       uint32_t running_image_end_offset)
{
    if (region_size == 0u) {
        return false;
    }
    if (!update_task_flash_guard_running_image_extent_valid(running_image_start_offset,
                                                              running_image_end_offset)) {
        // See header comment: an unusable extent must never be read as "no
        // overlap" by this function. Callers are required to check
        // update_task_flash_guard_running_image_extent_valid() themselves
        // before relying on this return value; this branch exists only as a
        // second, defence-in-depth line -- it still answers "overlap" (fail
        // closed / refuse) rather than "no overlap" (fail open / proceed).
        return true;
    }

    uint32_t region_end = region_offset + region_size; // no overflow: both operands are flash offsets, well under 2^32

    // Two half-open ranges [a, a_end) and [b, b_end) overlap iff a < b_end
    // AND b < a_end.
    return (region_offset < running_image_end_offset) && (running_image_start_offset < region_end);
}
