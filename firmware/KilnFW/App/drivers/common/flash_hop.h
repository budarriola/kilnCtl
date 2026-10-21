#ifndef KILNFW_FLASH_HOP_H
#define KILNFW_FLASH_HOP_H

#include <stdbool.h>

#include "esp_err.h"
#include "flash_worker.h"
#include "hal_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hand-declared (same prototype as bridge/uart_bridge.h) so a persist module
 * does not have to pull in the whole UART bridge API, as relay_cycles.c does. */
bool uart_bridge_ext_is_on_flash_worker(void);

/* flash_hop_run(): the one sanctioned way for a guarded NVS/cfg writer to be
 * reachable from a task whose stack is in PSRAM (executor, executor watchdog,
 * kiln_cfg_swap worker, LVGL, safety_poll, ...). Owner decision 2026-10-16:
 * those stacks stay in PSRAM and the writes hop to bx_flash_worker, whose stack
 * is internal; the hal_kv_write_safe_here() guards stay strict and are NOT
 * relaxed -- the write body still runs its own guard, which now passes because
 * the body executes on the worker.
 *
 *   - hal_kv_write_safe_here() true  -> fn(arg) runs inline on the caller.
 *   - caller is bx_flash_worker      -> fn(arg) runs inline (dispatching from the
 *                                       worker to itself would deadlock the board).
 *   - otherwise                      -> fn(arg) is dispatched onto the worker and
 *                                       this call BLOCKS until it has returned, so a
 *                                       halt-path save completes before the state that
 *                                       depends on it. `arg` may point at the caller's
 *                                       stack (the caller is blocked for the call).
 *
 * Returns ESP_OK iff fn ran; fn reports its own result through `arg`. Any other
 * value means fn did NOT run (worker not started / queue unusable): the caller
 * must treat that as a failed write, never fall through to running the body
 * itself. Do NOT call while holding a lock that a job running on the worker can
 * take (see cfg_save_lock.h's reservation rule). */
static inline esp_err_t flash_hop_run(void (*fn)(void *arg), void *arg)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hal_kv_write_safe_here() || uart_bridge_ext_is_on_flash_worker()) {
        fn(arg);
        return ESP_OK;
    }
    return uart_bridge_ext_run_on_flash_worker(fn, arg);
}

#ifdef __cplusplus
}
#endif

#endif /* KILNFW_FLASH_HOP_H */
