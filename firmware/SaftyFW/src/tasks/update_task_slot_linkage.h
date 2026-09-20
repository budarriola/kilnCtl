// update_task_slot_linkage.h -- pure slot-linkage plausibility check for an
// incoming update image's Cortex-M vector table (word 0 = initial SP, word 1
// = reset vector), ONE image, at UPDATE_END time, before the target slot's
// metadata is flipped active.
//
// Split out of update_task.c for the same reason update_task_erase_plan.c
// was: update_task.c pulls in FreeRTOS.h, pico/time.h and the pico hal_flash
// backend, none of which exist off target, so this check could only ever be
// regression-tested by a source-text scan there. The arithmetic that decides
// whether a vector table is plausible for a given slot has no such
// dependency and is worth asserting directly -- see update_task.c's own
// update_task_slot_linkage_plausible() wrapper (which reads the two words out
// of the mapped flash region and calls straight through to this function) for
// the full rationale: an image linked for slot A but written into slot B (a
// build/tooling mixup, not a transmission error) CRCs correctly -- the bytes
// received are exactly the bytes sent -- and would otherwise sail straight
// into bootloader/main.c's unconditional jump_to_app().
//
// Owner decision 2026-09-20.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_SLOT_LINKAGE_H
#define SAFTYFW_TASKS_UPDATE_TASK_SLOT_LINKAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true iff BOTH:
//   1. `sp` lies inside [sram_base, sram_end] (inclusive of sram_end: a
//      stack pointer initialised to the very top of RAM, one past the last
//      usable byte, is the normal/expected value, not an overrun).
//   2. `reset_vector` has the Thumb bit set (bit 0 -- required for every
//      Cortex-M instruction address; the RP2040 has no ARM mode) AND lies
//      inside [xip_base + target_slot_offset, xip_base + target_slot_offset
//      + slot_size) -- the TARGET slot's own flash window, not merely some
//      valid-looking address.
//
// Pure arithmetic, no I/O -- callers pass in the two constants
// (SRAM_BASE/SRAM_END, XIP_BASE) rather than this module including
// pico-sdk's addressmap.h itself, so it stays host-testable with no
// on-target dependency at all, same discipline as every other decision
// module under src/update/ and this directory.
bool update_task_slot_linkage_check(uint32_t sp, uint32_t reset_vector,
                                     uint32_t target_slot_offset, uint32_t slot_size,
                                     uint32_t sram_base, uint32_t sram_end, uint32_t xip_base);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UPDATE_TASK_SLOT_LINKAGE_H
