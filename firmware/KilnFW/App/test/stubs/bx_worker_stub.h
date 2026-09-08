#ifndef KILNCTL_TEST_STUBS_BX_WORKER_STUB_H
#define KILNCTL_TEST_STUBS_BX_WORKER_STUB_H

// Shared busy-modeling stub for uart_bridge_ext_run_on_flash_worker() /
// uart_bridge_ext_is_on_flash_worker(). Originally lived only in
// test_adaptive_tune.c (R1/R2, opus review of commit 7c47683) after the OLD
// stub shape -- bare `fn(arg); return ESP_OK;` with no lock/queue modeling
// at all -- let a re-entrant call (a job already running through the stub
// calling back into it) silently run fn() again with no complaint. That is
// NOT what the real bx_run_on_internal_stack() does: it takes a non-
// recursive mutex and feeds a depth-1 queue that only the worker task
// itself drains, so a re-entrant call from within an already-dispatched job
// deadlocks the real board permanently (see uart_bridge_ext.c's own comment
// on that function, and adaptive_tune.c's adaptive_tune_clear_ki_baseline()/
// adaptive_tune_run_end() call-site comments, for the deadlock shapes this
// represents).
//
// S2 (2026-09-01 audit, second re-entrant path on the halt path): the bare
// `fn(arg); return ESP_OK;` shape this replaces was ALSO still sitting,
// undetected, in test_profile_executor_prestart.c -- which links the real
// adaptive_tune.c and exercises profile_executor_halt() (itself calling
// adaptive_tune_run_end()) at several call sites. A busy-modeling stub
// added to only one file does not protect the other, so this header makes
// it shared: both test files now include it and get identical re-entrancy
// modeling instead of each hand-rolling (or forgetting to hand-roll) their
// own.
//
// S3 (self-consistency): a caller-supplied fn() used to have to set
// s_stub_on_flash_worker = true BY HAND before calling back into
// adaptive_tune.c from inside a dispatched job, decoupled from s_stub_bx_
// busy -- easy to forget when writing a NEW re-entrant-path test, and a
// forgotten flag silently fails to catch the bug it was meant to catch.
// This stub now sets s_stub_on_flash_worker = true itself for the duration
// of fn(arg) (restoring whatever it was before on the way out, so nested/
// sequential dispatches from a non-worker context still see `false`
// afterward), so "we are now running on the flash worker's own task" is
// modeled automatically by the one thing that is actually true whenever
// fn() is executing: it always runs via this dispatch path, or inline (see
// s_stub_on_flash_worker's own use below) via the is_on_flash_worker()
// check the code under test performs.
//
// Include AFTER "test_common.h" (TEST_CHECK()/g_test_failures) and
// "esp_err.h" (esp_err_t/ESP_OK/ESP_FAIL) are already visible -- this
// header does not include either itself, to avoid dragging a header-guard
// ordering dependency into files that already curate their own include
// list carefully (see test_adaptive_tune.c's and test_profile_executor_
// prestart.c's own #include blocks).
//
// The two globals are `static` (this header is included into exactly one
// translation unit per test executable -- each test file here builds its
// own separate .exe, see build_host_tests.ps1 -- so there is no multiple-
// definition hazard even though several executables include it). The two
// functions themselves are NOT `static`: uart_bridge.h declares both with
// external linkage (uart_bridge_ext_run_on_flash_worker()/_is_on_flash_
// worker()), and a `static` definition here would conflict with that
// earlier non-static declaration once these drivers' own headers are
// #included later in the same translation unit.

static bool s_stub_bx_busy = false;
static bool s_stub_on_flash_worker = false;

esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    if (s_stub_bx_busy) {
        // Models the real deadlock: a caller re-entering the worker while a
        // job is already in flight. On hardware this blocks forever; here
        // it must fail loudly instead, or this stub is exactly as blind as
        // the one it replaces.
        TEST_CHECK(false,
                   "uart_bridge_ext_run_on_flash_worker() called re-entrantly -- "
                   "this deadlocks the real flash worker permanently (see R1, commit 7c47683)");
        return ESP_FAIL;
    }
    s_stub_bx_busy = true;
    bool prev_on_worker = s_stub_on_flash_worker;
    s_stub_on_flash_worker = true; // S3: self-consistent -- fn() really is now "on the worker"
    fn(arg);
    s_stub_on_flash_worker = prev_on_worker;
    s_stub_bx_busy = false;
    return ESP_OK;
}

// Stands in for "is the calling task bx_flash_worker" -- see uart_bridge_
// ext.c's real uart_bridge_ext_is_on_flash_worker(), which this host build
// does not link (it reads a FreeRTOS task handle). Driven automatically by
// the dispatch stub above while fn() is running (S3); a test that wants to
// simulate "already on the worker" from OUTSIDE a dispatched job (e.g. to
// call a function under test directly, without going through the stub
// above first) may still set s_stub_on_flash_worker = true by hand around
// that call.
bool uart_bridge_ext_is_on_flash_worker(void)
{
    return s_stub_on_flash_worker;
}

// Stands in for "the flash-safe worker task has been created" -- see
// uart_bridge_ext.c's real uart_bridge_ext_flash_worker_started(), added
// alongside cfg_fs_mount.c's wait_for_flash_worker() (the fix for the
// worker-not-started-yet race that made every cold-boot deferred cfg auto-
// format fail in ~16 ms). Always true here: every test file that includes
// this stub dispatches through uart_bridge_ext_run_on_flash_worker()
// directly and never exercises the boot-ordering race itself, so there is
// never a reason for a wait loop built on this stub to actually wait.
bool uart_bridge_ext_flash_worker_started(void)
{
    return true;
}

#endif // KILNCTL_TEST_STUBS_BX_WORKER_STUB_H
