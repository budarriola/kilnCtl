// update_task_erase_plan.h -- pure chunk arithmetic for the slot erase walk.
//
// Split out of update_task.c for the same reason watchdog_gate.c was split out
// of watchdog_task.c: update_task.c pulls in FreeRTOS.h, pico/time.h and the
// pico hal_flash backend, none of which exist off target, so its erase loop
// could only ever be regression-tested by a source-text scan. The arithmetic
// that decides WHICH bytes get erased has no such dependency, and after
// 2026-09-18 (docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md) dropped
// UPDATE_TASK_ERASE_CHUNK_SIZE from 64 KB to 4 KB -- 13 chunks to 208 -- it is
// worth being able to assert directly that the walk still covers the whole
// slot exactly, with no gap and no overrun past the slot's last byte.
//
// An overrun here would erase into whatever follows the target slot; a gap
// would leave stale bytes inside a slot that update_task_program_chunk()
// afterwards assumes reads back as 0xFF (see its own comment on why it can
// read-modify-write a page at all). Neither failure announces itself at erase
// time, which is what makes them worth a host test rather than a comment.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_ERASE_PLAN_H
#define SAFTYFW_TASKS_UPDATE_TASK_ERASE_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How many chunks the walk over `total_len` bytes takes at `chunk_size` bytes
// each, rounding UP so a total that does not divide evenly still has its short
// tail covered. Returns 0 if either argument is 0.
uint32_t update_erase_plan_chunk_count(uint32_t total_len, uint32_t chunk_size);

// Describes chunk `index` of that walk: *out_offset is the absolute flash
// offset to erase (slot_offset + index * chunk_size) and *out_len its length,
// which is chunk_size for every chunk except a final short tail.
//
// Returns false, writing neither output, if the arguments are degenerate
// (zero chunk_size or total_len, NULL outputs) or if `index` is past the last
// chunk -- callers are expected to treat that as fail-closed rather than
// erasing a guessed range.
bool update_erase_plan_chunk(uint32_t slot_offset, uint32_t total_len, uint32_t chunk_size,
                              uint32_t index, uint32_t *out_offset, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UPDATE_TASK_ERASE_PLAN_H
