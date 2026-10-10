// Host test for App/drivers/safety/danger_mode.c (HOST_TEST_COVERAGE_GAPS
// campaign 6): window lifecycle with a fake tick clock -- start, touch,
// expiry (via the real, static danger_mode_task body), stop, heat-enable
// refused outside the window, re-entry, and 32-bit tick wrap.
//
// danger_mode.c is #include'd directly. The FreeRTOS headers are pulled in
// first, THEN xTaskGetTickCount/vTaskDelay are macro-shadowed, so only
// danger_mode.c's calls are redirected. vTaskDelay longjmps out of the
// task's infinite loop after one full iteration.
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_common.h"

#include "kiln_io_owner.h"
#include "profile_executor.h"
#include "profile_executor_state.h"
#include "safety_link.h"
#include "stack_margin.h"
#include "startup_faults.h"

int g_test_failures = 0;
int g_test_count = 0;
#define CHK(c) TEST_CHECK((c), #c)

static uint32_t s_tick;
static jmp_buf s_task_jmp;
static int s_delay_calls;
#define xTaskGetTickCount() ((TickType_t)s_tick)
static void fake_vtaskdelay(TickType_t t)
{
    (void)t;
    if (++s_delay_calls >= 2) {
        longjmp(s_task_jmp, 1);
    }
}
#define vTaskDelay(t) fake_vtaskdelay(t)

#include "../drivers/safety/danger_mode.c"

/* ---- fakes ---- */
static int s_relays_off_calls;
static esp_err_t s_relays_off_result = ESP_OK;
static int s_enable_calls;
static bool s_enable_last;
static esp_err_t s_enable_result = ESP_OK;
static bool s_firing_active;

esp_err_t kiln_io_owner_command_all_relays_off(void)
{
    s_relays_off_calls++;
    return s_relays_off_result;
}
esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    (void)link;
    s_enable_calls++;
    s_enable_last = enable;
    return s_enable_result;
}
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    (void)out;
    return ESP_FAIL;
}
bool profile_executor_get_active_id(uint8_t *out_id)
{
    if (out_id) {
        *out_id = 1;
    }
    return s_firing_active;
}
bool stack_margin_register(const char *name, void *slot, uint32_t bytes)
{
    (void)name;
    (void)slot;
    (void)bytes;
    return true;
}
void startup_fault_note(startup_fault_t id)
{
    (void)id;
}

static void reset_counters(void)
{
    s_relays_off_calls = 0;
    s_enable_calls = 0;
    s_enable_last = true;
    s_relays_off_result = ESP_OK;
    s_enable_result = ESP_OK;
    s_firing_active = false;
}

/* Run exactly one expiry-check iteration of the real task body. */
static void run_task_once(void)
{
    s_delay_calls = 0;
    if (setjmp(s_task_jmp) == 0) {
        danger_mode_task(NULL);
    }
}

static void test_before_init(void)
{
    TEST_SECTION("danger_mode before init");
    s_tick = 1000;
    CHK(!danger_mode_request_start());
    CHK(!danger_mode_active());
    CHK(danger_mode_remaining_ms() == 0);
    CHK(!danger_mode_touch());
    CHK(!danger_mode_set_heat_enable_request(true));
    CHK(!danger_mode_get_heat_requested());
    CHK(s_enable_calls == 0);
}

static void test_lifecycle(void)
{
    TEST_SECTION("danger_mode start / touch / expiry / stop");
    g_test_stub_semaphore_take_default = 1;
    danger_mode_init((SafetyLinkClass *)0);
    CHK(s_dm.initialized);
    reset_counters();

    s_tick = 10000;
    CHK(!danger_mode_active());
    CHK(!danger_mode_touch()); /* nothing to touch */

    /* enable refused outside the window, nothing sent */
    CHK(!danger_mode_set_heat_enable_request(true));
    CHK(s_enable_calls == 0);

    CHK(danger_mode_request_start());
    CHK(danger_mode_active());
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS);
    CHK(!danger_mode_get_heat_requested());

    /* time passes, remaining shrinks */
    s_tick = 10000 + 100000;
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS - 100000u);
    /* touch resets the full window */
    CHK(danger_mode_touch());
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS);

    /* heat-enable inside the window: sent, recorded, and extends window */
    s_tick += 50000;
    CHK(danger_mode_set_heat_enable_request(true));
    CHK(s_enable_calls == 1 && s_enable_last == true);
    CHK(danger_mode_get_heat_requested());
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS);

    /* link refuses enable=true: reported false, flag unchanged */
    s_enable_result = ESP_ERR_INVALID_STATE;
    CHK(danger_mode_set_heat_enable_request(true) == false);
    CHK(danger_mode_get_heat_requested()); /* still the earlier request */
    /* release always succeeds from this module's view */
    s_enable_result = ESP_OK;
    CHK(danger_mode_set_heat_enable_request(false));
    CHK(!danger_mode_get_heat_requested());

    /* re-entry while open: stays open, full window. heat_requested after re-entry is NOT asserted: see HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md F6-1 */
    CHK(danger_mode_set_heat_enable_request(true));
    CHK(danger_mode_request_start());
    CHK(danger_mode_active());

    /* active() reads false at the deadline but does NOT self-close */
    s_tick += DANGER_MODE_WINDOW_MS;
    CHK(!danger_mode_active());
    CHK(danger_mode_remaining_ms() == 0);
    CHK(s_dm.window_open);
    reset_counters();
    run_task_once();
    CHK(!s_dm.window_open);
    CHK(s_relays_off_calls == 1);
    CHK(s_enable_calls == 1 && s_enable_last == false);
    /* second pass: already closed, no second release */
    run_task_once();
    CHK(s_relays_off_calls == 1 && s_enable_calls == 1);
    CHK(!danger_mode_touch());

    /* task one ms before the deadline does nothing; at the deadline expires */
    CHK(danger_mode_request_start());
    s_tick += DANGER_MODE_WINDOW_MS - 1;
    reset_counters();
    run_task_once();
    CHK(s_dm.window_open && s_relays_off_calls == 0);
    s_tick += 1;
    run_task_once();
    CHK(!s_dm.window_open && s_relays_off_calls == 1);

    /* stop: releases only when open */
    reset_counters();
    danger_mode_stop("test");
    CHK(s_relays_off_calls == 0 && s_enable_calls == 0);
    CHK(danger_mode_request_start());
    CHK(danger_mode_set_heat_enable_request(true));
    reset_counters();
    danger_mode_stop(NULL);
    CHK(!danger_mode_active());
    CHK(!danger_mode_get_heat_requested());
    CHK(s_relays_off_calls == 1 && s_enable_calls == 1 && s_enable_last == false);
    CHK(!danger_mode_set_heat_enable_request(true)); /* refused after stop */

    /* stop with a failing owner still attempts the enable release */
    CHK(danger_mode_request_start());
    reset_counters();
    s_relays_off_result = ESP_ERR_TIMEOUT;
    danger_mode_stop("fail");
    CHK(s_relays_off_calls == 1 && s_enable_calls == 1);
    CHK(!s_dm.window_open);

    /* start refused while a firing is active */
    reset_counters();
    s_firing_active = true;
    CHK(!danger_mode_request_start());
    CHK(!danger_mode_active());
    s_firing_active = false;
}

static void test_clock_wrap(void)
{
    TEST_SECTION("danger_mode 32-bit tick wrap");
    reset_counters();
    /* start 1 s before the tick counter wraps */
    s_tick = 0xFFFFFFFFu - 999u;
    CHK(danger_mode_request_start());
    CHK(danger_mode_active());
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS);
    s_tick += 2000u; /* wrapped past zero */
    CHK(s_tick < 5000u);
    CHK(danger_mode_active());
    CHK(danger_mode_remaining_ms() == DANGER_MODE_WINDOW_MS - 2000u);
    run_task_once();
    CHK(s_dm.window_open && s_relays_off_calls == 0);
    s_tick += DANGER_MODE_WINDOW_MS - 2000u;
    CHK(!danger_mode_active());
    run_task_once();
    CHK(!s_dm.window_open && s_relays_off_calls == 1);
}

static void test_lock_timeout(void)
{
    TEST_SECTION("danger_mode lock timeout");
    reset_counters();
    s_tick = 500;
    g_test_stub_semaphore_fail_nth = 1;
    CHK(!danger_mode_request_start());
    CHK(!s_dm.window_open);
    CHK(danger_mode_request_start());
    g_test_stub_semaphore_fail_nth = 1;
    CHK(!danger_mode_active());
    g_test_stub_semaphore_fail_nth = 1;
    CHK(!danger_mode_touch());
    danger_mode_stop("cleanup");
}

int main(void)
{
    test_before_init();
    test_lifecycle();
    test_clock_wrap();
    test_lock_timeout();
    if (g_test_failures) {
        printf("test_danger_mode: %d FAILED\n", g_test_failures);
        return 1;
    }
    printf("\n%d checks passed\n", g_test_count);
    return 0;
}
