// flash_worker_wait -- ONE shared bounded-poll helper for "block this boot
// call site until the flash-safe worker task exists, or give up after a
// ceiling". Extracted 2026-09-08 from cfg_fs_mount.c's cfg_fs_auto_format_
// task-only wait_for_flash_worker() (added by 1136c0a9 to fix the auto-
// format path's boot race) so every OTHER migrate-on-load call site that
// dispatches to the flash worker before main_control_bringup() has started
// it (relay_cycles_init(), adaptive_tune_init()'s kibase resolve) shares
// ONE implementation instead of each copying its own 20ms/5s constants --
// see docs/FILESYSTEM_PLAN.md's "reset one side of a pair" entry for why a
// copied wait is itself a hazard.
//
// Host-testable: backed only by a caller-supplied "is it started yet"
// predicate function pointer plus FreeRTOS's vTaskDelay/pdMS_TO_TICKS, both
// already stubbed for every host test that links relay_cycles.c/
// adaptive_tune.c. No ESP-IDF-only header is pulled in here, unlike
// cfg_fs_mount.c itself (esp_littlefs.h, esp_partition.h) which is why THAT
// file needs its own separate 32-bit test executable.
#ifndef KILNFW_FLASH_WORKER_WAIT_H
#define KILNFW_FLASH_WORKER_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_WORKER_WAIT_POLL_MS_DEFAULT 20
#define FLASH_WORKER_WAIT_CEILING_MS_DEFAULT 5000

/* Polls started_fn() every poll_ms until it returns true, or until
 * ceiling_ms total has elapsed. Returns true iff started_fn() returned true
 * before the ceiling. A NULL started_fn returns false immediately (treated
 * as "never started", the safe assumption). Never blocks past ceiling_ms --
 * boot must never hang here indefinitely. */
bool flash_worker_wait_until_started(bool (*started_fn)(void), uint32_t poll_ms, uint32_t ceiling_ms);

/* Convenience wrapper using the two defaults above and
 * uart_bridge_ext_flash_worker_started() as the predicate -- what every
 * boot-time call site actually wants. */
bool flash_worker_wait_default(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNFW_FLASH_WORKER_WAIT_H */
