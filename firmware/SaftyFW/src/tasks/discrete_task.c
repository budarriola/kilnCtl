// discrete_task.c -- Phase 4/5: GPIO init, the periodic loop, and a real
// consecutive-sample debounce for both discretes. E-stop needs N samples
// covering 50ms (SAFETY_MODEL.md section 4, S7); mainFault needs N samples
// covering 200ms (S6a). N is derived from this task's own period
// (SAFTYFW_PERIOD_DISCRETE_TASK_MS, task_priorities.h) via a ceiling divide,
// not hardcoded, so the debounce window stays correct if the period ever
// changes -- the same "compute don't assume" discipline thermal_guard.c's
// guard 6 debounce uses (KilnFW side), just windowed in time rather than
// counted in raw ticks there.
#include "discrete_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/gpio.h"

#include "board_pins.h"
#include "task_priorities.h"
#include "watchdog_task.h"

#define DISCRETE_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE

#define SAFTYFW_ESTOP_DEBOUNCE_MS       50u  // SAFETY_MODEL.md section 4, S7
#define SAFTYFW_MAIN_FAULT_DEBOUNCE_MS  200u // SAFETY_MODEL.md section 4, S6a

// Ceiling divide: N = ceil(window_ms / period_ms), always >= 1 for a
// non-zero window. Evaluated at compile time from task_priorities.h's
// period constant, so a period change (e.g. a faster discrete_task) widens
// or narrows N automatically instead of silently under- or over-debouncing.
#define SAFTYFW_DEBOUNCE_SAMPLES(window_ms) \
    (((window_ms) + SAFTYFW_PERIOD_DISCRETE_TASK_MS - 1u) / SAFTYFW_PERIOD_DISCRETE_TASK_MS)

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_estop_pressed = false;
static volatile bool s_main_fault = false;

// Standard consecutive-sample debounce: a new raw value is only published
// once it has been seen `n_samples` times in a row. Any disagreement resets
// the streak against the new value, so a single noisy sample cannot
// "borrow" progress from an unrelated earlier streak.
typedef struct {
    bool     candidate;
    uint32_t streak;
    bool     published;
} debounce_state_t;

static bool debounce_update(debounce_state_t *db, bool raw, uint32_t n_samples)
{
    if (db->streak == 0u || raw != db->candidate) {
        db->candidate = raw;
        db->streak = 1u;
    } else if (db->streak < n_samples) {
        db->streak++;
    }

    if (db->streak >= n_samples) {
        db->published = db->candidate;
    }
    return db->published;
}

static void discrete_task_fn(void *arg)
{
    (void)arg;

    // Debounce state starts at the pins' known idle-safe reading (not
    // pressed / not asserted) rather than an unknown value, so a boot that
    // never samples fast enough to clear a full window still reports the
    // conservative "not yet proven bad" state instead of an uninitialised
    // one. gpio_init() below establishes the real electrical state before
    // the first sample anyway; this only matters for the first
    // (n_samples - 1) ticks.
    static debounce_state_t estop_db = { .published = false };
    static debounce_state_t main_fault_db = { .published = false };

    const uint32_t estop_n = SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS);
    const uint32_t main_fault_n = SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_DISCRETE_TASK_MS));

        bool estop_raw = !gpio_get(SAFTYFW_PIN_ESTOP);      // active low
        bool main_fault_raw = !gpio_get(SAFTYFW_PIN_MAIN_FAULT); // active low

        s_estop_pressed = debounce_update(&estop_db, estop_raw, estop_n);
        s_main_fault = debounce_update(&main_fault_db, main_fault_raw, main_fault_n);

        watchdog_task_checkin(WATCHDOG_CHECKIN_DISCRETE_TASK);
    }
}

bool discrete_task_start(void)
{
    gpio_init(SAFTYFW_PIN_ESTOP);
    gpio_set_dir(SAFTYFW_PIN_ESTOP, false); // input
    gpio_pull_up(SAFTYFW_PIN_ESTOP); // matches R10's external pull-up; belt and suspenders

    gpio_init(SAFTYFW_PIN_MAIN_FAULT);
    gpio_set_dir(SAFTYFW_PIN_MAIN_FAULT, false); // input
    gpio_pull_up(SAFTYFW_PIN_MAIN_FAULT); // matches R8's external pull-up

    BaseType_t ok = xTaskCreate(discrete_task_fn, "discrete_task", DISCRETE_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_DISCRETE_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}

bool discrete_task_estop_pressed(void)
{
    return s_estop_pressed;
}

bool discrete_task_main_fault(void)
{
    return s_main_fault;
}
