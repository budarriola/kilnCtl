/* hal_scratch_pico.c -- pico-sdk backend for interface/hal_scratch.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against pico-sdk
 * 2.x's hardware/watchdog.h `watchdog_hw->scratch[]` registers, grounded in
 * the real SaftyFW consumers hal_scratch.h's own header comment names:
 * boot_reason.c (slots 0/1), startup_diag.h/main.c (slots 2/3),
 * watchdog_overdue_diag.c / main.c's stack-overflow hook (slot 5, tags
 * 0xD9/0xE3), main.c's boot-stage marker (slot 6), clear_trip_diag.c
 * (slot 7). Not wired into any CMakeLists yet; syntax-checked by
 * firmware/hwAbstraction/test/compile_pico_backends.ps1, same as
 * hal_flash_pico.c until those four direct pokers are rebased onto this
 * header (a later phase, not this one).
 *
 * One-way boundary: this file does NOT include any SaftyFW header (no
 * startup_diag.h, no clear_trip_diag_codec.h, no magic-value constants
 * pulled in from there). Everything this backend needs -- which slot,
 * whose tag, what magic word to check against -- arrives as plain
 * arguments through hal_scratch.h's own API, exactly as that header's
 * "REGISTRY / UNIQUENESS" section describes. No INTERFACE MISMATCH: every
 * real access site (raw uint32_t read/write/clear, magic-word validation)
 * is exactly the shape hal_scratch_write_u32()/hal_scratch_read_u32()/
 * hal_scratch_clear() were written for.
 */
#include "hal_scratch.h"

#include <string.h>

#include "hardware/watchdog.h"

/* Static claim table. HAL_SCRATCH_MAX_CLAIMS covers the real map one-for-one
 * (slots 0,1,2,3,5,5,6,7 = 8 real claims; slot 4 is never claimed, it is
 * refused outright) with a little headroom for a future single-tag slot
 * splitting into a genuine second co-owner without bumping this constant
 * again immediately. */
#define HAL_SCRATCH_MAX_CLAIMS 12u

static hal_scratch_claim_t s_claims[HAL_SCRATCH_MAX_CLAIMS];
static uint8_t s_claim_count = 0;

static bool slot_in_range(uint8_t slot) {
    return slot < HAL_SCRATCH_SLOT_COUNT;
}

/* True if (slot, tag) is already registered -- the real collision hal_scratch_claim()
 * must refuse (same slot, same tag). A second claim on the same slot with a
 * DIFFERENT tag (slot 5's two legitimate co-owners) is not a collision. */
static bool already_claimed(uint8_t slot, uint8_t tag) {
    for (uint8_t i = 0; i < s_claim_count; i++) {
        if (s_claims[i].slot == slot && s_claims[i].tag == tag) {
            return true;
        }
    }
    return false;
}

hal_status_t hal_scratch_claim(uint8_t slot, const char *owner, uint8_t tag) {
    if (owner == NULL || !slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    /* Slot 4 is pico-sdk's own -- watchdog_enable() owns it outright. See
     * hal_scratch.h's "SLOT 4 HARD RESERVATION". */
    if (slot == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) {
        return HAL_INVALID_ARG;
    }
    if (already_claimed(slot, tag)) {
        return HAL_INVALID_ARG;
    }
    if (s_claim_count >= HAL_SCRATCH_MAX_CLAIMS) {
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
    watchdog_hw->scratch[slot] = value;
    return HAL_OK;
}

hal_status_t hal_scratch_read_u32(uint8_t slot, uint32_t *out_value,
                                   uint8_t magic_slot, uint32_t magic,
                                   bool *magic_ok) {
    if (out_value == NULL || !slot_in_range(slot) || !slot_in_range(magic_slot)) {
        return HAL_INVALID_ARG;
    }
    *out_value = watchdog_hw->scratch[slot];
    if (magic_ok != NULL) {
        *magic_ok = (watchdog_hw->scratch[magic_slot] == magic);
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
    watchdog_hw->scratch[slot] = 0u;
    return HAL_OK;
}
