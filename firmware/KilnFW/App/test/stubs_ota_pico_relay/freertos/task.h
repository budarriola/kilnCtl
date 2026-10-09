// Host-test stub -- PRIVATE to test_ota_pico_relay.c's own executable (its
// /I for this directory precedes @hostTestsRsp's own /I list, same
// "private shim wins by include order" convention test_dashboard_status_http.c
// uses for its own stubs_dashboard_status/ directory -- see build_host_tests.ps1's
// comment on that executable).
//
// Why this executable needs its OWN task.h instead of the shared
// stubs/freertos/task.h every other host test uses: that shared stub
// hardcodes xTaskGetTickCount() to always return 0 and no-ops vTaskDelay(),
// which is fine for every other host test (none of them exercises real-time
// deadline math) but makes ota_pico_relay.c's relay_wait_for_states()
// timeout branch (`now_ms() >= deadline`) LITERALLY UNREACHABLE -- since
// now_ms() never advances, an unmatched wait would spin forever instead of
// timing out, hanging this test executable (and therefore
// check_00_kilnfw_host_tests.ps1) rather than failing cleanly.
//
// Fix: a controllable fake clock. vTaskDelay(ticks) advances it (so
// relay_wait_for_states()'s own polling loop -- which really does call
// vTaskDelay(pdMS_TO_TICKS(RELAY_STATUS_POLL_MS)) between polls -- drives
// real, bounded timeout behaviour), and xTaskGetTickCount() reads it back.
// g_ota_pico_relay_fake_ticks is defined (not just declared) in
// test_ota_pico_relay.c itself, reset to 0 between test cases by that file's
// own test_reset_relay_state() helper.
#ifndef TEST_STUB_TASK_H
#define TEST_STUB_TASK_H

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
// ota_pico_relay.c also uses portMUX_TYPE/taskENTER_CRITICAL/taskEXIT_CRITICAL
// directly (s_status_mux) -- nothing in the shared FreeRTOS.h stub pulls in
// portmacro.h on its own (no other host test needs it transitively), so pull
// it in explicitly here.
#include "freertos/portmacro.h"

typedef struct task_s *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define tskIDLE_PRIORITY 0u
#define tskNO_AFFINITY ((BaseType_t)-1)

extern uint32_t g_ota_pico_relay_fake_ticks;

static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)g_ota_pico_relay_fake_ticks; }

static inline void vTaskDelay(TickType_t ticks) { g_ota_pico_relay_fake_ticks += (uint32_t)ticks; }

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned long stack_depth,
                                                  void *arg, UBaseType_t priority, TaskHandle_t *out_handle,
                                                  BaseType_t core_id)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    (void)core_id;
    if (out_handle) {
        *out_handle = (TaskHandle_t)1;
    }
    return pdPASS;
}

// Never invokes `task` -- same convention as the shared stub. This test
// drives relay_task_fn() by calling it directly (it is the function under
// test), never by letting xTaskCreate() run it on a real scheduler that does
// not exist on the host.
static inline BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_depth,
                                      void *arg, UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    if (out_handle) {
        *out_handle = (TaskHandle_t)1;
    }
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t task) { (void)task; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }

static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096u;
}

#endif // TEST_STUB_TASK_H
