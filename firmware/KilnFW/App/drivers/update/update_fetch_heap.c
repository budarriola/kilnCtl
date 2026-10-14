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

static uint32_t sub_sat(uint32_t a, uint32_t b)
{
    return a > b ? a - b : 0u;
}

uint32_t update_fetch_heap_need(bool is_download, bool hop)
{
    if (!hop) {
        return FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH;
    }
    return is_download ? FETCH_HOP_DOWNLOAD_HEAP_MIN : FETCH_HOP_CHECK_HEAP_MIN;
}

fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block, bool is_download)
{
    return verdict(free_internal, largest_internal_block, update_fetch_heap_need(is_download, false),
                   is_download ? FETCH_LARGEST_BLOCK_MIN : FETCH_CHECK_BLOCK_MIN);
}

fetch_heap_verdict_t update_fetch_heap_admit_hop(uint32_t free_internal, uint32_t largest_internal_block,
                                                 bool is_download)
{
    return verdict(free_internal, largest_internal_block, update_fetch_heap_need(is_download, true),
                   is_download ? FETCH_LARGEST_BLOCK_MIN : FETCH_CHECK_BLOCK_MIN);
}

uint32_t update_fetch_heap_dma_offset(uint32_t free_internal, uint32_t dma_free)
{
    return sub_sat(free_internal, dma_free);
}

uint32_t update_fetch_heap_dma_need(bool is_download, bool hop, uint32_t rtc_free)
{
    // A check's first admission uses the download need (see the header); a hop uses the kind's hop need.
    return sub_sat(update_fetch_heap_need(is_download, hop), rtc_free);
}

uint32_t update_fetch_heap_dma_floor(uint32_t rtc_free)
{
    return sub_sat(FETCH_HEAP_ABORT_BELOW, rtc_free);
}

uint32_t update_fetch_heap_dma_hard_min(uint32_t rtc_free)
{
    return sub_sat(FETCH_HEAP_FLOOR_BYTES, rtc_free);
}

bool update_fetch_heap_dma_admit(uint32_t dma_free, uint32_t rtc_free, bool is_download, bool hop)
{
    return dma_free >= update_fetch_heap_dma_need(is_download, hop, rtc_free);
}

unsigned update_fetch_heap_dma_pick_pair(const uint32_t *f, const uint32_t *dma, unsigned n)
{
    unsigned best = 0;
    uint32_t best_off = sub_sat(f[0], dma[0]);
    for (unsigned i = 1; i < n; i++) {
        const uint32_t off = sub_sat(f[i], dma[i]);
        if (off < best_off || (off == best_off && dma[i] < dma[best])) {
            best = i;
            best_off = off;
        }
    }
    return best;
}

bool update_fetch_heap_dma_abort(uint32_t dma_free, uint32_t rtc_free, unsigned *hist)
{
    if (dma_free < update_fetch_heap_dma_hard_min(rtc_free)) {
        return true;
    }
    const unsigned mask = (1u << FETCH_DMA_WINDOW) - 1u;
    *hist = ((*hist << 1) | (dma_free < update_fetch_heap_dma_floor(rtc_free) ? 1u : 0u)) & mask;
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
