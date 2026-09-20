// update_task_slot_linkage.c -- see update_task_slot_linkage.h.
#include "update_task_slot_linkage.h"

bool update_task_slot_linkage_check(uint32_t sp, uint32_t reset_vector,
                                     uint32_t target_slot_offset, uint32_t slot_size,
                                     uint32_t sram_base, uint32_t sram_end, uint32_t xip_base)
{
    if (sp < sram_base || sp > sram_end) {
        return false;
    }
    if ((reset_vector & 0x1u) == 0u) {
        return false; // Thumb bit not set -- not a valid Cortex-M code address
    }

    uint32_t slot_window_start = xip_base + target_slot_offset;
    uint32_t slot_window_end = slot_window_start + slot_size;
    if (reset_vector < slot_window_start || reset_vector >= slot_window_end) {
        return false;
    }
    return true;
}
