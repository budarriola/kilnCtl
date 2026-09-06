/* fake_scratch.c -- host fake backend for hal_scratch.h. See fake_scratch.h. */
#include "fake_scratch.h"

#include <string.h>

#define FAKE_SCRATCH_MAX_CLAIMS 12u

static uint32_t            s_slots[HAL_SCRATCH_SLOT_COUNT];
static hal_scratch_claim_t s_claims[FAKE_SCRATCH_MAX_CLAIMS];
static uint8_t             s_claim_count = 0;

static bool slot_in_range(uint8_t slot) {
    return slot < HAL_SCRATCH_SLOT_COUNT;
}

/* True if (slot, tag) is already registered -- the real collision
 * hal_scratch_claim() must refuse. A second claim on the same slot with a
 * DIFFERENT tag (slot 5's two legitimate co-owners) is not a collision. */
static bool already_claimed(uint8_t slot, uint8_t tag) {
    for (uint8_t i = 0; i < s_claim_count; i++) {
        if (s_claims[i].slot == slot && s_claims[i].tag == tag) {
            return true;
        }
    }
    return false;
}

void fake_scratch_reset_all(void) {
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_claims, 0, sizeof(s_claims));
    s_claim_count = 0;
}

void fake_scratch_simulate_reset(void) {
    /* Watchdog reset: claim table is software/module state that real
     * firmware rebuilds by re-running every claiming module's boot-time
     * hal_scratch_claim() call -- but the 8 hardware scratch words survive
     * a watchdog reset untouched, which is the entire reason they exist. */
    memset(s_claims, 0, sizeof(s_claims));
    s_claim_count = 0;
}

void fake_scratch_simulate_power_loss(void) {
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_claims, 0, sizeof(s_claims));
    s_claim_count = 0;
}

uint8_t fake_scratch_get_claim_count(void) {
    return s_claim_count;
}

bool fake_scratch_get_claim(uint8_t index, hal_scratch_claim_t *out) {
    if (out == NULL || index >= s_claim_count) {
        return false;
    }
    *out = s_claims[index];
    return true;
}

bool fake_scratch_is_claimed(uint8_t slot, uint8_t tag) {
    return already_claimed(slot, tag);
}

/* --- hal_scratch.h implementation --- */

hal_status_t hal_scratch_claim(uint8_t slot, const char *owner, uint8_t tag) {
    if (owner == NULL || !slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    /* Slot 4 is pico-sdk's own -- see hal_scratch.h's "SLOT 4 HARD
     * RESERVATION". */
    if (slot == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) {
        return HAL_INVALID_ARG;
    }
    if (already_claimed(slot, tag)) {
        return HAL_INVALID_ARG;
    }
    if (s_claim_count >= FAKE_SCRATCH_MAX_CLAIMS) {
        return HAL_NO_MEM;
    }
    s_claims[s_claim_count].slot = slot;
    s_claims[s_claim_count].owner = owner;
    s_claims[s_claim_count].tag = tag;
    s_claim_count++;
    return HAL_OK;
}

hal_status_t hal_scratch_write_u32(uint8_t slot, uint32_t value) {
    if (!slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    if (slot == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) {
        return HAL_INVALID_ARG;
    }
    s_slots[slot] = value;
    return HAL_OK;
}

hal_status_t hal_scratch_read_u32(uint8_t slot, uint32_t *out_value,
                                   uint8_t magic_slot, uint32_t magic,
                                   bool *magic_ok) {
    if (out_value == NULL || !slot_in_range(slot) || !slot_in_range(magic_slot)) {
        return HAL_INVALID_ARG;
    }
    *out_value = s_slots[slot];
    if (magic_ok != NULL) {
        *magic_ok = (s_slots[magic_slot] == magic);
    }
    return HAL_OK;
}

hal_status_t hal_scratch_clear(uint8_t slot) {
    if (!slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    if (slot == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) {
        return HAL_INVALID_ARG;
    }
    s_slots[slot] = 0u;
    return HAL_OK;
}
