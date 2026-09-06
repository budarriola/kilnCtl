// owner_slot_pool.c -- see owner_slot_pool.h.
#include "owner_slot_pool.h"

int owner_slot_pool_alloc(uint8_t *refcounts, size_t count)
{
    if (!refcounts) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (refcounts[i] == 0u) {
            refcounts[i] = OWNER_SLOT_POOL_HELD_BY_BOTH;
            return (int)i;
        }
    }
    return -1;
}

bool owner_slot_pool_release(uint8_t *refcounts, size_t count, int index)
{
    if (!refcounts || index < 0 || (size_t)index >= count) {
        return false;
    }
    if (refcounts[index] == 0u) {
        /* Double release -- a caller bug. Refuse to underflow rather than
         * wrap a uint8_t 0 -> 255, which would make an already-free slot
         * look held again and could wedge the pool. */
        return false;
    }
    refcounts[index]--;
    return refcounts[index] == 0u;
}
