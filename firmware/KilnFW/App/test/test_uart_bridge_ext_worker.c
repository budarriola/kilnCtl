// Host tests for the flash-worker side of App/drivers/bridge/uart_bridge_ext.c:
// the save-section reservation (bx_reserve_for_save_section()/
// bx_release_save_section(), installed as pref_cfg_fs's save-section hooks),
// the recursive s_bx_lock, the bounded-acquire timeout path, and the posted-
// job slot (docs/audits/FLASH_WORKER_LOCK_INVERSION_AUDIT_2026-10-09.md,
// follow-ups to the review of 4271767d/48e1ba8a).
//
// #includes uart_bridge_ext.c directly (same convention as
// test_uart_bridge_ext_control_gate.c) and pre-empts UART_BRIDGE_H for the
// same reason that file documents (uart_bridge.h drags in panel_spi.h).
//
// The shared FreeRTOS stubs are single-threaded no-ops with no recursive
// mutex, so this file replaces the primitives uart_bridge_ext.c uses with
// small fakes (macros defined AFTER the stub headers are included and BEFORE
// the .c is, so the .c's own #includes hit the include guards and its calls
// resolve to the fakes):
//   - a recursive mutex with an owner task, depth and take/give counters; a
//     finite-tick take while another task owns it fails (the timeout path),
//     a portMAX_DELAY take while another task owns it is recorded as a
//     deadlock (the single-threaded test cannot block);
//   - a one-slot job queue;
//   - s_bx_done: when the dispatcher waits on it, the fake switches the
//     "current task" to the worker and runs one bx_worker_iteration(), the
//     same split-out loop body bx_worker_task() runs on target.
// The current task is a plain variable (g_cur_task), so a test can act as
// "task A", "task B" or the worker itself.
#define UART_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "uart_protocol.h"

int g_test_failures = 0;
int g_test_count = 0;

// test_common.h's TEST_CHECK takes a message; the condition text is message enough here.
#define CHECK(cond) TEST_CHECK((cond), #cond)

// ---------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------
#define TASK_A ((TaskHandle_t)(uintptr_t)0x10)
#define TASK_B ((TaskHandle_t)(uintptr_t)0x20)
#define TASK_WORKER ((TaskHandle_t)(uintptr_t)0x99)

static TaskHandle_t g_cur_task = TASK_A;

static TaskHandle_t fake_xTaskGetCurrentTaskHandle(void) { return g_cur_task; }

static unsigned g_task_create_calls = 0;
static BaseType_t fake_xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned long stack_depth,
                                               void *arg, UBaseType_t priority, TaskHandle_t *out_handle,
                                               BaseType_t core_id)
{
    (void)task; (void)name; (void)stack_depth; (void)arg; (void)priority; (void)core_id;
    g_task_create_calls++;
    if (out_handle) {
        *out_handle = TASK_WORKER;
    }
    return pdPASS;
}

// ---------------------------------------------------------------------
// Recursive mutex (s_bx_lock is the only recursive one in the file)
// ---------------------------------------------------------------------
static struct {
    TaskHandle_t owner;
    int depth;
    unsigned takes;
    unsigned gives;
    unsigned failed_takes;
    unsigned deadlocks;
    unsigned bad_gives;
    unsigned creates;
} g_rm;

static SemaphoreHandle_t fake_xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *storage)
{
    g_rm.creates++;
    return (SemaphoreHandle_t)storage;
}

static BaseType_t fake_xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks)
{
    CHECK(sem != NULL);
    if (g_rm.owner == NULL || g_rm.owner == g_cur_task) {
        g_rm.owner = g_cur_task;
        g_rm.depth++;
        g_rm.takes++;
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) {
        // On target this would block until the owner gives; the single-
        // threaded test cannot, so every test is written so this never
        // happens and any occurrence is a failure.
        g_rm.deadlocks++;
    }
    g_rm.failed_takes++;
    return pdFALSE;
}

static BaseType_t fake_xSemaphoreGiveRecursive(SemaphoreHandle_t sem)
{
    CHECK(sem != NULL);
    if (g_rm.owner != g_cur_task || g_rm.depth <= 0) {
        g_rm.bad_gives++;
        return pdFALSE;
    }
    g_rm.gives++;
    if (--g_rm.depth == 0) {
        g_rm.owner = NULL;
    }
    return pdTRUE;
}

// ---------------------------------------------------------------------
// One-slot queue (s_bx_jobs)
// ---------------------------------------------------------------------
static struct {
    bool created;
    bool full;
    size_t item_size;
    uint8_t item[64];
    unsigned send_while_full;
} g_q;

static QueueHandle_t fake_xQueueCreate(unsigned long len, unsigned long item_size)
{
    CHECK(len == 1);
    CHECK(item_size <= sizeof(g_q.item));
    g_q.created = true;
    g_q.full = false;
    g_q.item_size = item_size;
    return (QueueHandle_t)&g_q;
}

static BaseType_t fake_xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)q; (void)ticks;
    if (g_q.full) {
        g_q.send_while_full++;
        return pdFALSE;
    }
    memcpy(g_q.item, item, g_q.item_size);
    g_q.full = true;
    return pdTRUE;
}

static BaseType_t fake_xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks)
{
    (void)q; (void)ticks;
    if (!g_q.full) {
        return pdFALSE;
    }
    memcpy(out, g_q.item, g_q.item_size);
    g_q.full = false;
    return pdTRUE;
}

static void fake_vQueueDelete(QueueHandle_t q) { (void)q; g_q.created = false; }

// ---------------------------------------------------------------------
// Binary semaphore (s_bx_done) and plain mutex (s_bx_post_lock)
// ---------------------------------------------------------------------
static int g_done_storage;
static int g_post_lock_storage;
static int g_done_count;
static unsigned g_done_wait_without_give;

static SemaphoreHandle_t fake_xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)&g_done_storage; }
static SemaphoreHandle_t fake_xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)&g_post_lock_storage; }

static void bx_worker_iteration(void);  // defined in uart_bridge_ext.c below

static void run_worker_once(void)
{
    TaskHandle_t saved = g_cur_task;
    g_cur_task = TASK_WORKER;
    bx_worker_iteration();
    g_cur_task = saved;
}

static BaseType_t fake_xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;
    CHECK(sem != NULL);
    if (sem == (SemaphoreHandle_t)&g_done_storage) {
        // The dispatcher waits for its job: let the worker run one pass.
        if (g_done_count == 0) {
            run_worker_once();
        }
        if (g_done_count == 0) {
            g_done_wait_without_give++;
            return pdFALSE;
        }
        g_done_count--;
        return pdTRUE;
    }
    return pdTRUE;  // s_bx_post_lock: uncontended in a single-threaded test
}

static BaseType_t fake_xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (sem == (SemaphoreHandle_t)&g_done_storage) {
        g_done_count++;
    }
    return pdTRUE;
}

static void fake_vSemaphoreDelete(SemaphoreHandle_t sem) { (void)sem; }

// ---------------------------------------------------------------------
// Other symbols uart_bridge_ext.c references
// ---------------------------------------------------------------------
static size_t fake_heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 100000; }
static uint32_t fake_esp_get_free_heap_size(void) { return 200000; }

bool stack_margin_register(const char *name, void *task_handle_slot, uint32_t configured_stack_bytes)
{
    (void)name; (void)task_handle_slot; (void)configured_stack_bytes;
    return true;
}

esp_err_t uart_protocol_send(uart_protocol_t *proto, uart_proto_device_t dst_device, uint8_t dst_task,
                             uint8_t src_task, const uint8_t *payload, size_t length, uint32_t ack_timeout_ms)
{
    (void)proto; (void)dst_device; (void)dst_task; (void)src_task; (void)payload; (void)length;
    (void)ack_timeout_ms;
    return ESP_OK;
}

// pref_cfg_fs.h's hook installer: capture the hooks so the tests drive them
// exactly as cfg_save_lock_take()/_give() do through
// pref_cfg_fs_save_section_enter()/_exit().
static bool (*g_hook_enter)(void);
static void (*g_hook_exit)(bool reserved);
static unsigned g_hook_installs;

void pref_cfg_fs_set_save_section_hooks(bool (*enter)(void), void (*exit_fn)(bool reserved))
{
    g_hook_enter = enter;
    g_hook_exit = exit_fn;
    g_hook_installs++;
}

#define xTaskGetCurrentTaskHandle fake_xTaskGetCurrentTaskHandle
#define xTaskCreatePinnedToCore fake_xTaskCreatePinnedToCore
#define xSemaphoreCreateRecursiveMutexStatic fake_xSemaphoreCreateRecursiveMutexStatic
#define xSemaphoreTakeRecursive fake_xSemaphoreTakeRecursive
#define xSemaphoreGiveRecursive fake_xSemaphoreGiveRecursive
#define xQueueCreate fake_xQueueCreate
#define xQueueSend fake_xQueueSend
#define xQueueReceive fake_xQueueReceive
#define vQueueDelete fake_vQueueDelete
#define xSemaphoreCreateBinary fake_xSemaphoreCreateBinary
#define xSemaphoreCreateMutex fake_xSemaphoreCreateMutex
#define xSemaphoreTake fake_xSemaphoreTake
#define xSemaphoreGive fake_xSemaphoreGive
#define vSemaphoreDelete fake_vSemaphoreDelete
#define heap_caps_get_largest_free_block fake_heap_caps_get_largest_free_block
#define esp_get_free_heap_size fake_esp_get_free_heap_size

#include "../drivers/bridge/uart_bridge_ext.c"

// ---------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------
static unsigned g_job_runs;
static bool g_job_ran_on_worker;
static int g_job_lock_depth_seen;

static void job_fn(void *arg)
{
    (void)arg;
    g_job_runs++;
    g_job_ran_on_worker = uart_bridge_ext_is_on_flash_worker();
    g_job_lock_depth_seen = g_rm.depth;
}

static unsigned g_posted_runs;
static bool g_posted_lock_held_by_worker;

static void posted_fn(void *arg)
{
    (void)arg;
    g_posted_runs++;
    g_posted_lock_held_by_worker = (g_rm.owner == TASK_WORKER);
}

static void reset_job_counters(void)
{
    g_job_runs = 0;
    g_job_ran_on_worker = false;
    g_job_lock_depth_seen = -1;
    g_posted_runs = 0;
    g_posted_lock_held_by_worker = false;
}

static void check_lock_quiescent(void)
{
    CHECK(g_rm.owner == NULL);
    CHECK(g_rm.depth == 0);
    CHECK(g_rm.takes == g_rm.gives);
    CHECK(g_rm.deadlocks == 0);
    CHECK(g_rm.bad_gives == 0);
}

// ---------------------------------------------------------------------
// Tests (run in order: the module's statics persist across them)
// ---------------------------------------------------------------------

// Finding 5: a save section entered BEFORE the worker task exists must still
// reserve s_bx_lock, so a worker created mid-section cannot run a job inside
// it. Before the fix the hook was installed only after the worker started,
// and bx_reserve_for_save_section() returned false while !s_bx_started.
static void test_reservation_before_worker_start(void)
{
    CHECK(!uart_bridge_ext_flash_worker_started());
    uart_bridge_ext_save_reservation_init();
    CHECK(g_hook_installs == 1);
    CHECK(g_rm.creates == 1);
    CHECK(g_hook_enter != NULL && g_hook_exit != NULL);

    // Idempotent.
    uart_bridge_ext_save_reservation_init();
    CHECK(g_hook_installs == 1);
    CHECK(g_rm.creates == 1);

    g_cur_task = TASK_A;
    bool reserved = g_hook_enter();
    CHECK(reserved);
    CHECK(g_rm.owner == TASK_A);
    CHECK(g_rm.depth == 1);

    // The section's own cfg write before the worker exists fails fast,
    // without blocking, exactly as before the change.
    reset_job_counters();
    CHECK(uart_bridge_ext_run_on_flash_worker(job_fn, NULL) == ESP_FAIL);
    CHECK(g_job_runs == 0);
    CHECK(g_rm.depth == 1);

    // The worker starts while task A is still inside its section (the
    // boot-order window the review flagged). Starting it must not create a
    // second lock or drop A's reservation.
    CHECK(uart_bridge_ext_worker_ensure_started());
    CHECK(uart_bridge_ext_flash_worker_started());
    CHECK(g_rm.creates == 1);
    CHECK(g_hook_installs == 1);
    CHECK(g_rm.owner == TASK_A);

    // A posted job arriving now must wait for A's section to end.
    g_cur_task = TASK_B;
    CHECK(uart_bridge_ext_post_on_flash_worker(posted_fn) == ESP_OK);
    run_worker_once();
    CHECK(g_posted_runs == 0);

    g_cur_task = TASK_A;
    g_hook_exit(reserved);
    run_worker_once();
    CHECK(g_posted_runs == 1);
    CHECK(g_posted_lock_held_by_worker);
    check_lock_quiescent();
}

// The worker itself never reserves (it would deadlock on itself); the exit
// hook with reserved=false must give nothing.
static void test_worker_does_not_reserve(void)
{
    unsigned takes = g_rm.takes;
    unsigned gives = g_rm.gives;
    g_cur_task = TASK_WORKER;
    bool reserved = g_hook_enter();
    CHECK(!reserved);
    CHECK(g_rm.owner == NULL);
    g_hook_exit(reserved);
    CHECK(g_rm.takes == takes);
    CHECK(g_rm.gives == gives);
    g_cur_task = TASK_A;
    check_lock_quiescent();
}

// Enter/exit balance, nested sections on one task, and a dispatch from
// inside a section: the dispatcher re-takes the recursive lock to depth 2,
// the job runs on the worker, and every take is matched by one give.
static void test_dispatch_inside_section_recursive_counts(void)
{
    reset_job_counters();
    g_cur_task = TASK_A;
    unsigned takes0 = g_rm.takes;

    bool r1 = g_hook_enter();
    bool r2 = g_hook_enter();  // nested save section on the same task
    CHECK(r1 && r2);
    CHECK(g_rm.depth == 2);

    int arg = 7;
    CHECK(uart_bridge_ext_run_on_flash_worker(job_fn, &arg) == ESP_OK);
    CHECK(g_job_runs == 1);
    CHECK(g_job_ran_on_worker);
    CHECK(g_job_lock_depth_seen == 3);  // two sections + the dispatcher
    CHECK(g_rm.owner == TASK_A);
    CHECK(g_rm.depth == 2);
    CHECK(!g_q.full);
    CHECK(g_done_count == 0);
    CHECK(g_done_wait_without_give == 0);

    g_hook_exit(r2);
    CHECK(g_rm.depth == 1);
    g_hook_exit(r1);
    CHECK(g_rm.takes - takes0 == 3);
    check_lock_quiescent();

    // A dispatch outside any section also balances.
    reset_job_counters();
    CHECK(uart_bridge_ext_run_on_flash_worker(job_fn, NULL) == ESP_OK);
    CHECK(g_job_runs == 1);
    CHECK(g_job_lock_depth_seen == 1);
    check_lock_quiescent();

    // A job dispatched from the worker itself runs inline and takes nothing.
    reset_job_counters();
    unsigned takes1 = g_rm.takes;
    g_cur_task = TASK_WORKER;
    CHECK(uart_bridge_ext_run_on_flash_worker(job_fn, NULL) == ESP_OK);
    CHECK(g_job_runs == 1);
    CHECK(g_rm.takes == takes1);
    g_cur_task = TASK_A;
    check_lock_quiescent();
}

// The bounded-acquire path: another task holds the worker (here through a
// save section), so the timed take fails. The job must not run, must not be
// queued, and nothing may be given back for a take that never happened.
static void test_timeout_path_gives_nothing(void)
{
    reset_job_counters();
    g_cur_task = TASK_B;
    bool reserved = g_hook_enter();
    CHECK(reserved);

    g_cur_task = TASK_A;
    unsigned takes = g_rm.takes;
    unsigned gives = g_rm.gives;
    unsigned failed = g_rm.failed_takes;
    CHECK(uart_bridge_ext_run_on_flash_worker_timeout(job_fn, NULL, 50) == ESP_ERR_TIMEOUT);
    CHECK(g_job_runs == 0);
    CHECK(!g_q.full);
    CHECK(g_rm.failed_takes == failed + 1);
    CHECK(g_rm.takes == takes);
    CHECK(g_rm.gives == gives);
    CHECK(g_rm.bad_gives == 0);
    CHECK(g_rm.owner == TASK_B);
    CHECK(g_rm.depth == 1);

    g_cur_task = TASK_B;
    g_hook_exit(reserved);
    check_lock_quiescent();

    // With the lock free the same call succeeds.
    g_cur_task = TASK_A;
    CHECK(uart_bridge_ext_run_on_flash_worker_timeout(job_fn, NULL, 50) == ESP_OK);
    CHECK(g_job_runs == 1);
    check_lock_quiescent();
}

// A posted job is delayed, never dropped, while the worker is reserved: it
// stays in its slot across worker passes, runs exactly once after release,
// and runs with the worker holding s_bx_lock.
static void test_posted_job_delayed_not_dropped(void)
{
    reset_job_counters();
    g_cur_task = TASK_A;
    bool reserved = g_hook_enter();
    CHECK(reserved);

    g_cur_task = TASK_B;
    CHECK(uart_bridge_ext_post_on_flash_worker(posted_fn) == ESP_OK);
    for (int i = 0; i < 5; i++) {
        run_worker_once();
    }
    CHECK(g_posted_runs == 0);
    // Still in the slot: a second post coalesces rather than replacing it.
    CHECK(uart_bridge_ext_post_on_flash_worker(posted_fn) == ESP_ERR_INVALID_STATE);

    g_cur_task = TASK_A;
    g_hook_exit(reserved);
    run_worker_once();
    CHECK(g_posted_runs == 1);
    CHECK(g_posted_lock_held_by_worker);
    run_worker_once();
    CHECK(g_posted_runs == 1);  // exactly once
    check_lock_quiescent();

    // The slot is free again.
    g_cur_task = TASK_B;
    CHECK(uart_bridge_ext_post_on_flash_worker(posted_fn) == ESP_OK);
    run_worker_once();
    CHECK(g_posted_runs == 2);
    g_cur_task = TASK_A;
    check_lock_quiescent();
}

int main(void)
{
    test_reservation_before_worker_start();
    test_worker_does_not_reserve();
    test_dispatch_inside_section_recursive_counts();
    test_timeout_path_gives_nothing();
    test_posted_job_delayed_not_dropped();
    CHECK(g_task_create_calls == 1);
    CHECK(g_q.send_while_full == 0);

    printf("uart_bridge_ext_worker: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
