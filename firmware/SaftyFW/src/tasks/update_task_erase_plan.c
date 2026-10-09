// update_task_erase_plan.c -- see update_task_erase_plan.h.
#include "update_task_erase_plan.h"

#include <stddef.h>

uint32_t update_erase_plan_chunk_count(uint32_t total_len, uint32_t chunk_size)
{
    if (total_len == 0u || chunk_size == 0u) {
        return 0u;
    }

    // Round up without overflowing: total_len + chunk_size - 1 can wrap for a
    // total_len near UINT32_MAX, and this walks a real flash slot, so the
    // division-based form is used instead of the usual idiom.
    uint32_t whole = total_len / chunk_size;
    return ((total_len % chunk_size) != 0u) ? (whole + 1u) : whole;
}

bool update_erase_plan_chunk(uint32_t slot_offset, uint32_t total_len, uint32_t chunk_size,
                              uint32_t index, uint32_t *out_offset, uint32_t *out_len)
{
    if (out_offset == NULL || out_len == NULL) {
        return false;
    }
    if (total_len == 0u || chunk_size == 0u) {
        return false;
    }
    if (index >= update_erase_plan_chunk_count(total_len, chunk_size)) {
        return false;
    }

    uint32_t consumed = index * chunk_size; // < total_len by the index check above
    uint32_t remaining = total_len - consumed;

    *out_offset = slot_offset + consumed;
    *out_len = (remaining < chunk_size) ? remaining : chunk_size;
    return true;
}
