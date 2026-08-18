// watchdog_task.c -- see watchdog_task.h. Phase 2 skeleton: the bitmask,
// the feed-iff-all-checked-in gate, and the 250 ms period are real; what is
// NOT yet real is registering a WATCHDOG_CHECKIN_SELF_TEST-style timeout as
// SAFETY_TRIP_SELF_TEST in the boot_reason latch (Phase 5+, once relay_owner
// and safety_core have real bodies to time out in the first place).
//
// Also owns the physical heartbeat LED (TODO.md Phase 2, "Heartbeat LED,
// physical, on the safety processor itself" -- requested 2026-08-17). This
// is a side effect of the feed decision below, not a second check: it must
// never be able to make a stalled system look alive, so the LED is only
// ever touched in the same branch that decides whether to feed the real
// hardware watchdog, using the exact same `mask == WATCHDOG_CHECKIN_ALL_MASK`
// result. There is deliberately no separate "is everything ok" computation
// for the LED to drift out of sync with.
#include "watchdog_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "board_pins.h"
#include "task_priorities.h"

#define WATCHDOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE
#define WATCHDOG_HW_TIMEOUT_MS      1000

// Every registered task's bit, per docs/ARCHITECTURE.md section 4.
#define WATCHDOG_CHECKIN_ALL_MASK   ((1u << WATCHDOG_CHECKIN_COUNT) - 1u)

static volatile uint32_t s_checkin_mask = 0;
// Cumulative-since-boot mask, never cleared by watchdog_task_fn()'s periodic
// snapshot-and-clear below -- see watchdog_task_all_checked_in_since_boot()'s
// doc comment in watchdog_task.h for why this needs to be a second,
// independent bitmask rather than reusing s_checkin_mask.
static volatile uint32_t s_ever_checkin_mask = 0;
static TaskHandle_t s_task_handle = NULL;
// Only ever read/written from watchdog_task_fn() itself (single-writer, no
// other task touches the LED), so unlike s_checkin_mask this needs neither
// volatile nor a critical section.
static bool s_led_state = false;

static void watchdog_task_fn(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_WATCHDOG_TASK_MS));

        // Snapshot-and-clear under a short critical section: checkin bits set
        // by other tasks between the snapshot and the clear are for the
        // *next* window, which is correct (they proved liveness after this
        // window's deadline, so they still count for the next one).
        taskENTER_CRITICAL();
        uint32_t mask = s_checkin_mask;
        s_checkin_mask = 0;
        taskEXIT_CRITICAL();

        if (mask == WATCHDOG_CHECKIN_ALL_MASK) {
            watchdog_update();

            // Toggle, not set-high: a steady blink at half the feed period
            // (500 ms full cycle) is what makes this a *heartbeat* rather
            // than just an "ok" lamp -- a light that is merely on can also
            // be a light nobody is driving anymore (stuck GPIO, task
            // crashed after its last write). Toggling only ever happens
            // here, in the same branch as the real feed, so the LED cannot
            // physically keep moving once this branch stops running.
            s_led_state = !s_led_state;
            gpio_put(SAFTYFW_PIN_HEARTBEAT_LED, s_led_state);
        }
        // else: at least one task missed its window. Do NOT feed -- the
        // watchdog will reboot the chip in <= 1s, which is the fail-safe by
        // construction the doc describes. TODO: log which bit(s) were
        // missing once log_task exists (Phase 2 later item / Phase 8), so a
        // watchdog reboot's cause is diagnosable rather than just "it
        // happened".
        //
        // The LED is deliberately left untouched in this branch too: it
        // freezes at whatever level it was last driven to (per TODO.md's
        // "going dark or freezing should track the same condition") instead
        // of being forced to a fixed "fault" level. Forcing a level here
        // would mean this code path decides what the LED does on a miss,
        // which is exactly the kind of second, independent liveness
        // judgement the header comment above says must not exist -- a
        // frozen LED is simply what "the toggle above stopped running"
        // looks like from outside the chip.
    }
}

bool watchdog_task_start(void)
{
    s_checkin_mask = 0;
    s_ever_checkin_mask = 0;

    // Heartbeat LED: plain digital out, driven low (off) until the first
    // full check-in window closes and the toggle above runs -- see
    // board_pins.h for why GPIO25 is a real pin here despite having no A1
    // schematic net (it is the Pico module's own onboard LED).
    s_led_state = false;
    gpio_init(SAFTYFW_PIN_HEARTBEAT_LED);
    gpio_set_dir(SAFTYFW_PIN_HEARTBEAT_LED, GPIO_OUT);
    gpio_put(SAFTYFW_PIN_HEARTBEAT_LED, s_led_state);

    // pause_on_debug = true: hardcoded for now, no release/debug distinction
    // in this build yet. TODO: flip to false for a release build --
    // ARCHITECTURE.md section 8, watchdog_enable(ms, pause_on_debug).
    watchdog_enable(WATCHDOG_HW_TIMEOUT_MS, true);

    BaseType_t ok = xTaskCreate(watchdog_task_fn, "watchdog_task", WATCHDOG_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_WATCHDOG_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}

void watchdog_task_checkin(watchdog_checkin_id_t id)
{
    if (id >= WATCHDOG_CHECKIN_COUNT) {
        return;
    }

    // Single instruction-ish read-modify-write; a critical section keeps it
    // safe against the periodic snapshot-and-clear above running on the
    // other core. Cheap and never blocks the caller.
    taskENTER_CRITICAL();
    s_checkin_mask |= (1u << id);
    s_ever_checkin_mask |= (1u << id);
    taskEXIT_CRITICAL();
}

bool watchdog_task_all_checked_in_since_boot(void)
{
    taskENTER_CRITICAL();
    uint32_t mask = s_ever_checkin_mask;
    taskEXIT_CRITICAL();
    return mask == WATCHDOG_CHECKIN_ALL_MASK;
}
