#ifndef PROFILES_SLOT_BITMAP_H
#define PROFILES_SLOT_BITMAP_H

/* profiles_slot_bitmap -- a widened, id-indexed "is this slot in use" bitmap,
 * shared by profiles_http.c's s_profiles.used_bitmap and
 * profiles_favorites.c's user-slot favorite mask.
 *
 * docs/PROFILE_SLOTS_100.md section 7 task 1: both of those were a
 * single scalar (uint8_t / uint32_t) tested with `mask & (1u << id)`, which
 * is undefined behavior the moment `id` reaches the scalar's bit width (32
 * for the favorites mask; the plan's own Status section names
 * `favorites_get_handler()`'s `user_mask & (1u << i)` at `i == 32` as the
 * concrete instance) and simply cannot address the 100 user slots +
 * live-edit id the plan eventually raises `PROFILES_MAX_COUNT` to. Four
 * 32-bit words cover ids 0..127, which is exactly `PROFILE_BUILTIN_ID_BASE`
 * (profiles_builtin.h) -- the hard ceiling section 2 of that plan documents.
 *
 * This header widens the TYPE and the per-id test/set/clear only. It does
 * NOT change `PROFILES_MAX_COUNT` (still 8, task 6's job) and does not
 * change either module's on-flash key shape: at 8 slots today, only word[0]
 * bits 0..7 are ever nonzero, and both callers keep persisting exactly that
 * one word/byte so the persisted bytes stay byte-identical to before this
 * change (see profiles_http.c's nvs_save_slot()/nvs_load_all_from() and
 * profiles_favorites.c's favorites_save()/profiles_favorites_start()). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILES_SLOT_BITMAP_WORDS 4
#define PROFILES_SLOT_BITMAP_MAX_ID (PROFILES_SLOT_BITMAP_WORDS * 32) /* 128, exclusive */

typedef struct {
    uint32_t words[PROFILES_SLOT_BITMAP_WORDS];
} profiles_slot_bitmap_t;

static inline bool profiles_slot_bitmap_test(const profiles_slot_bitmap_t *bm, unsigned id)
{
    if (bm == NULL || id >= PROFILES_SLOT_BITMAP_MAX_ID) {
        return false;
    }
    return (bm->words[id / 32] & (1u << (id % 32))) != 0;
}

static inline void profiles_slot_bitmap_set(profiles_slot_bitmap_t *bm, unsigned id)
{
    if (bm == NULL || id >= PROFILES_SLOT_BITMAP_MAX_ID) {
        return;
    }
    bm->words[id / 32] |= (1u << (id % 32));
}

static inline void profiles_slot_bitmap_clear(profiles_slot_bitmap_t *bm, unsigned id)
{
    if (bm == NULL || id >= PROFILES_SLOT_BITMAP_MAX_ID) {
        return;
    }
    bm->words[id / 32] &= ~(1u << (id % 32));
}

/* word[0] is the low 32 ids -- exactly what the pre-widening uint8_t/uint32_t
 * scalar covered. These two helpers are the persistence seam: they let a
 * caller keep writing/reading a single scalar NVS key (byte-identical to
 * before) while everything in RAM already operates on the wider type. */
static inline void profiles_slot_bitmap_from_u32(profiles_slot_bitmap_t *bm, uint32_t low_word)
{
    if (bm == NULL) {
        return;
    }
    bm->words[0] = low_word;
    for (int i = 1; i < PROFILES_SLOT_BITMAP_WORDS; i++) {
        bm->words[i] = 0;
    }
}

static inline uint32_t profiles_slot_bitmap_to_u32(const profiles_slot_bitmap_t *bm)
{
    return bm ? bm->words[0] : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* PROFILES_SLOT_BITMAP_H */
