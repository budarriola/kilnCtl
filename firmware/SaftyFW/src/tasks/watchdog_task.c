// watchdog_task.c -- see watchdog_task.h. Phase 2 skeleton: the bitmask,
// the feed-iff-all-checked-in gate, and the 250 ms period are real; what is
// NOT yet real is registering a WATCHDOG_CHECKIN_SELF_TEST-style timeout as
// SAFETY_TRIP_SELF_TEST in the boot_reason latch (Phase 5+, once relay_owner
// and safety_core have real bodies to time out in the first place).
#include "watchdog_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/watchdog.h"

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
        }
        // else: at least one task missed its window. Do NOT feed -- the
        // watchdog will reboot the chip in <= 1s, which is the fail-safe by
        // construction the doc describes. TODO: log which bit(s) were
        // missing once log_task exists (Phase 2 later item / Phase 8), so a
        // watchdog reboot's cause is diagnosable rather than just "it
        // happened".
    }
}

bool watchdog_task_start(void)
{
    s_checkin_mask = 0;
    s_ever_checkin_mask = 0;

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
