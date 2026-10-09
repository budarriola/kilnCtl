// received_ranges.c -- see received_ranges.h.
#include "received_ranges.h"

#include <string.h>

static bool bit_get(const uint8_t *bits, uint32_t index)
{
    return (bits[index / 8u] & (1u << (index % 8u))) != 0u;
}

static void bit_set(uint8_t *bits, uint32_t index)
{
    bits[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

void update_received_ranges_reset(update_received_ranges_t *r, uint32_t image_length)
{
    memset(r->bits, 0, sizeof(r->bits));

    uint32_t chunks = (image_length + UPDATE_CHUNK_LEN - 1u) / UPDATE_CHUNK_LEN;
    if (chunks > UPDATE_MAX_CHUNKS) {
        chunks = UPDATE_MAX_CHUNKS; // defensive clamp -- see header comment
    }
    r->total_chunks = chunks;
}

bool update_received_ranges_mark(update_received_ranges_t *r, uint32_t offset)
{
    if (offset % UPDATE_CHUNK_LEN != 0u) {
        return false;
    }
    uint32_t index = offset / UPDATE_CHUNK_LEN;
    if (index >= r->total_chunks) {
        return false;
    }
    bit_set(r->bits, index);
    return true;
}

bool update_received_ranges_is_complete(const update_received_ranges_t *r)
{
    for (uint32_t i = 0; i < r->total_chunks; i++) {
        if (!bit_get(r->bits, i)) {
            return false;
        }
    }
    return true;
}

uint32_t update_received_ranges_count(const update_received_ranges_t *r)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < r->total_chunks; i++) {
        if (bit_get(r->bits, i)) {
            count++;
        }
    }
    return count;
}

size_t update_received_ranges_find_gaps(const update_received_ranges_t *r, uint32_t start_index,
                                         uint32_t *out_chunk_indices, size_t max_out)
{
    if (r->total_chunks == 0u || max_out == 0u) {
        return 0;
    }

    size_t found = 0;
    uint32_t start = start_index % r->total_chunks;
    uint32_t i = start;
    uint32_t scanned = 0;

    while (scanned < r->total_chunks && found < max_out) {
        if (!bit_get(r->bits, i)) {
            out_chunk_indices[found++] = i;
        }
        i = (i + 1u) % r->total_chunks;
        scanned++;
    }

    return found;
}
