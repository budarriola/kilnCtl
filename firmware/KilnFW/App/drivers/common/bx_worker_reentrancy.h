#ifndef BX_WORKER_REENTRANCY_H
#define BX_WORKER_REENTRANCY_H

#include <stdbool.h>

// S4 (2026-09-01 audit of ae5905f): uart_bridge_ext.c was compiled by NO
// host test at all (no reference in build_host_tests.ps1 -- it pulls in
// kiln_io.h/profiles_http.h/wifi_prov.h/MAX31856.h/autotune_engine.h and a
// dozen more hardware-driving headers, well past what a host stub set can
// realistically cover), so removing bx_run_on_internal_stack()'s task-
// identity re-entrancy check reddened nothing -- the backstop that saved
// the accept path (and, before S1's fix, the halt path) had zero test
// coverage of its own.
//
// bx_run_on_internal_stack() and uart_bridge_ext_is_on_flash_worker() both
// answer the exact same question -- "is the calling task bx_flash_worker
// itself?" -- with the exact same two-pointer comparison, duplicated in
// both places. Factored out here as a tiny, dependency-free predicate (raw
// pointers rather than TaskHandle_t, so this header needs neither
// freertos/task.h nor any other ESP-IDF header) so:
//   1. uart_bridge_ext.c has one comparison instead of two to keep in sync,
//   2. that comparison is host-testable on its own, with no FreeRTOS/
//      hardware stub surface required at all -- see test_bx_worker_
//      reentrancy.c, which pins exactly the shape a careless refactor of
//      either call site could get wrong (NULL handle, matching handle,
//      non-matching handle) that the backstop and the is_on_flash_worker()
//      check both depend on.
//
// `worker_handle` is NULL before bx_worker_ensure_started()'s task-create
// has run (or after a worker that was never started) -- NULL never matches
// ANY current handle, including another NULL, so a not-yet-started worker
// is correctly never mistaken for "the caller is already on it".
static inline bool bx_caller_is_worker_task(const void *worker_handle, const void *current_handle)
{
    return worker_handle != NULL && current_handle == worker_handle;
}

#endif // BX_WORKER_REENTRANCY_H
