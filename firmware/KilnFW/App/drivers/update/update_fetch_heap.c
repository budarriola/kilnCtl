// update_fetch_heap.c -- see update_fetch_heap.h.
#include "update_fetch_heap.h"

static fetch_heap_verdict_t verdict(uint32_t free_internal, uint32_t largest, uint32_t need, uint32_t blk)
{
    if (free_internal < need) {
        return FETCH_HEAP_LOW_FREE;
    }
    if (largest < blk) {
        return FETCH_HEAP_LOW_BLOCK;
    }
    return FETCH_HEAP_OK;
}

fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block, bool is_download)
{
    return verdict(free_internal, largest_internal_block, FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH,
                   is_download ? FETCH_LARGEST_BLOCK_MIN : FETCH_CHECK_BLOCK_MIN);
}

fetch_heap_verdict_t update_fetch_heap_admit_hop(uint32_t free_internal, uint32_t largest_internal_block,
                                                 bool is_download)
{
    return verdict(free_internal, largest_internal_block,
                   is_download ? FETCH_HOP_DOWNLOAD_HEAP_MIN : FETCH_HOP_CHECK_HEAP_MIN,
                   is_download ? FETCH_LARGEST_BLOCK_MIN : FETCH_CHECK_BLOCK_MIN);
}

bool update_fetch_heap_dma_ok(uint32_t dma_free)
{
    return dma_free >= FETCH_DMA_FREE_MIN;
}

bool update_fetch_heap_dma_abort(uint32_t dma_free, unsigned *low_run)
{
    if (dma_free < FETCH_DMA_HARD_MIN) {
        return true;
    }
    if (dma_free < FETCH_DMA_FREE_MIN) {
        (*low_run)++;
        return *low_run >= FETCH_DMA_LOW_READS;
    }
    *low_run = 0;
    return false;
}

bool update_fetch_heap_abort(uint32_t free_internal)
{
    return free_internal < FETCH_HEAP_ABORT_BELOW;
}
