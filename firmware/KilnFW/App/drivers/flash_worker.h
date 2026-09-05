#ifndef KILNFW_FLASH_WORKER_H
#define KILNFW_FLASH_WORKER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Narrow header for the shared flash-safe executor dispatch function, split
 * out of uart_bridge.h so a consumer that only needs this one call (e.g.
 * log_store_mount.c) does not have to pull in the whole UART bridge API.
 *
 * Runs fn(arg) on the SAME internal-SRAM-stack worker task
 * uart_bridge_ext_start_flash_worker() creates, and blocks the calling task
 * until it returns -- for ANY caller elsewhere in the firmware that needs to
 * touch NVS/flash from a task whose own stack is not safely internal (see
 * uart_bridge_ext.c:104-127's HAZARD block: a flash operation disables the
 * cache, which makes PSRAM unreachable, and ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() asserts -- aborts the whole board
 * -- if the calling task's stack lives there). `arg` may point at the
 * caller's stack, since the caller is blocked for the whole call and that
 * storage stays live. Returns ESP_ERR_INVALID_ARG if fn is NULL, ESP_FAIL if
 * the worker is not started or its job queue/lock could not be used (caller
 * must not fall through to running fn() itself in that case -- that is
 * precisely the bug this exists to prevent).
 *
 * RE-ENTRANCY HAZARD: dispatching onto this worker from a handler that is
 * already running ON the flash worker deadlocks the board (see
 * "project_flash_worker_reentrancy" -- the host stub models no lock so
 * tests cannot see this). Callers that may already be on the worker must
 * check uart_bridge_ext_is_on_flash_worker() (declared in uart_bridge.h)
 * first and run their work inline instead of dispatching again. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* KILNFW_FLASH_WORKER_H */
