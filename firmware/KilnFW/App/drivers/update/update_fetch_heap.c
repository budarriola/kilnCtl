// update_fetch_heap.c -- see update_fetch_heap.h.
#include "update_fetch_heap.h"

fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block)
{
    if (free_internal < FETCH_HEAP_PRECHECK_MIN) {
        return FETCH_HEAP_LOW_FREE;
    }
    if (largest_internal_block < FETCH_LARGEST_BLOCK_MIN) {
        return FETCH_HEAP_LOW_BLOCK;
    }
    return FETCH_HEAP_OK;
}

bool update_fetch_heap_abort(uint32_t free_internal)
{
    return free_internal < FETCH_HEAP_ABORT_BELOW;
}
