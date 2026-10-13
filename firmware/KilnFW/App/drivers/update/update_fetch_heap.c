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

bool update_fetch_heap_dma_abort(uint32_t dma_free, unsigned *hist)
{
    if (dma_free < FETCH_DMA_HARD_MIN) {
        return true;
    }
    const unsigned mask = (1u << FETCH_DMA_WINDOW) - 1u;
    *hist = ((*hist << 1) | (dma_free < FETCH_DMA_FREE_MIN ? 1u : 0u)) & mask;
    const unsigned run_mask = (1u << FETCH_DMA_LOW_READS) - 1u;
    if ((*hist & run_mask) == run_mask) {
        return true;  // FETCH_DMA_LOW_READS consecutive low samples
    }
    unsigned lows = 0;
    for (unsigned h = *hist; h != 0; h >>= 1) {
        lows += h & 1u;
    }
    return lows >= FETCH_DMA_WINDOW_LOW;  // low, low, ok, low, ... patterns never recover for long
}

bool update_fetch_heap_abort(uint32_t free_internal)
{
    return free_internal < FETCH_HEAP_ABORT_BELOW;
}
