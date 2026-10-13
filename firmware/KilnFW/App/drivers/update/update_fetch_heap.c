// update_fetch_heap.c -- see update_fetch_heap.h.
#include "update_fetch_heap.h"

fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block, bool is_download)
{
    const uint32_t need = is_download ? FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH : FETCH_CHECK_HEAP_MIN;
    const uint32_t blk = is_download ? FETCH_LARGEST_BLOCK_MIN : FETCH_CHECK_BLOCK_MIN;
    if (free_internal < need) {
        return FETCH_HEAP_LOW_FREE;
    }
    if (largest_internal_block < blk) {
        return FETCH_HEAP_LOW_BLOCK;
    }
    return FETCH_HEAP_OK;
}

bool update_fetch_heap_abort(uint32_t free_internal)
{
    return free_internal < FETCH_HEAP_ABORT_BELOW;
}
