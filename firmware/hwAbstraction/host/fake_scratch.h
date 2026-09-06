/* fake_scratch.h -- host fake backend for hal_scratch.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION.md "hal_scratch -- pico watchdog-scratch
 * registry" and the pico backend (pico/scratch/hal_scratch_pico.c) this
 * fake mirrors: a static claim table keyed on (slot, tag), slot 4 hard-
 * reserved (HAL_SCRATCH_SLOT_WATCHDOG_ENABLE, refused outright on claim/
 * write/clear), and three raw uint32_t accessors with a magic-word
 * validation convention on read.
 *
 * Storage model: 8 uint32_t slots, matching HAL_SCRATCH_SLOT_COUNT. The
 * point of real watchdog scratch registers is that they are NOT cleared by
 * a watchdog reset (that is the entire reason the four real pokers use them
 * for trip-reason/startup-diag/crash-checkpoint state) but ARE cleared by a
 * genuine power-on-reset (uninitialized silicon on first power, per boot_
 * reason.c's own magic-word-validation discipline). This fake therefore
 * offers two distinct simulated events:
 *   fake_scratch_simulate_reset()      -- models a watchdog/software reset:
 *       clears the claim table (real firmware re-runs every module's boot-
 *       time hal_scratch_claim() call after any reboot) but LEAVES the 8
 *       slot values untouched -- scratch survives a watchdog reset, that is
 *       its entire point.
 *   fake_scratch_simulate_power_loss() -- models a real power-on-reset:
 *       clears both the claim table AND all 8 slot values (to 0, matching
 *       real silicon's power-on state -- callers distinguish "really
 *       written this cycle" from "reads zero because power-on left it
 *       uninitialised" via the magic-word convention hal_scratch_read_u32()
 *       implements, not via any special sentinel value here).
 *
 * Claim table: mirrors the pico backend's (slot, tag) uniqueness rule
 * exactly -- a second claim naming the same (slot, tag) pair is refused
 * (HAL_INVALID_ARG), but slot 5's two legitimate co-owners (distinct tags)
 * are accepted. Slot 4 (HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) is refused
 * outright for claim, write, and clear -- see hal_scratch.h's own "SLOT 4
 * HARD RESERVATION" comment.
 *
 * Inspection: fake_scratch_get_claim_count()/fake_scratch_get_claim() let a
 * test walk the live claim table (e.g. to assert a module claimed the slot
 * it was supposed to, or that two modules that both claim the same slot at
 * runtime are actually refused). The registry is runtime-only and advisory
 * -- see hal_scratch.h's "REGISTRY / UNIQUENESS" note: there is no
 * compile-time claim table or macro on any backend, and write/clear do not
 * consult this table at all beyond the slot-4 special case.
 */
#ifndef KILNCTL_FAKE_SCRATCH_H
#define KILNCTL_FAKE_SCRATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_scratch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resets everything to power-on-first-boot state: all 8 slots zeroed, claim
 * table empty. Call between test cases. */
void fake_scratch_reset_all(void);

/* Watchdog/software reset: clears the claim table only. Slot VALUES are
 * left exactly as they were -- see this header's top comment on why. */
void fake_scratch_simulate_reset(void);

/* Power-on reset: clears the claim table AND zeroes all 8 slot values. */
void fake_scratch_simulate_power_loss(void);

/* Number of live entries in the claim table. */
uint8_t fake_scratch_get_claim_count(void);

/* Copies claim table entry `index` (0-based, in claim order) into *out.
 * Returns false (out untouched) for an out-of-range index. */
bool fake_scratch_get_claim(uint8_t index, hal_scratch_claim_t *out);

/* True if (slot, tag) is currently present in the claim table. Convenience
 * over walking fake_scratch_get_claim() by hand. */
bool fake_scratch_is_claimed(uint8_t slot, uint8_t tag);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_SCRATCH_H */
