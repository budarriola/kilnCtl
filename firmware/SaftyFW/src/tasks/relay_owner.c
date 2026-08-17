// relay_owner.c -- Phase 5 ("Relay authority"): the INIT/GRACE/ARMED/TRIPPED
// state machine and latching trip semantics, on top of the Phase 2 task/
// queue/GPIO6-write skeleton.
//
// docs/ARCHITECTURE.md section 3: relay_owner is the ONLY code in the build
// that writes GPIO6, and it is the highest-priority task in the system so a
// command to de-energize can never be stuck behind anything else on its core.
//
// GRACE-entry judgement call: docs/ARCHITECTURE.md section 5 step 8 says
// "enter GRACE" right after step 7 ("start tasks"), and main.c's own TODO at
// steps 8-9 flagged this as unimplemented. Rather than adding a separate
// "relay_owner_enter_grace()" call for main.c to remember to make -- one
// more thing that can be wired in the wrong order or simply forgotten --
// this file starts the GRACE timer as the literal first thing
// relay_owner_task() does once it runs. Since relay_owner is both (a) the
// highest-priority task in the system and (b) the first task main.c starts
// in step 7, "GRACE begins when the task starts running" and "GRACE begins
// right after step 7" are the same instant in practice, with no window
// where some other command could be honoured before GRACE is active. See
// relay_owner.h's header comment for the state summary.
#include "relay_owner.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "hardware/gpio.h"

#include "board_pins.h"
#include "task_priorities.h"
#include "watchdog_task.h"

#define RELAY_OWNER_STACK_WORDS      configMINIMAL_STACK_SIZE
#define RELAY_OWNER_QUEUE_LEN        4 // small and non-blocking; a backlog here means something is wrong upstream
// Bounded wait rather than portMAX_DELAY -- relay_owner must still check in
// with watchdog_task even during a long idle stretch with no commands, or
// its own liveness proof would depend on someone else calling it, which is
// exactly backwards for the highest-priority task in the system.
#define RELAY_OWNER_QUEUE_WAIT_MS    200

// SAFETY_MODEL.md section 2, "startup is not steady state": 60s default.
// Not yet commissionable (Phase 9, config_store) -- a fixed constant here is
// the documented default, same status as safety_guards.h's other Phase-9-
// pending thresholds.
#define SAFTYFW_STARTUP_GRACE_MS     60000u

typedef enum {
    RELAY_OWNER_CMD_ENERGIZE,
    RELAY_OWNER_CMD_TRIP,
    RELAY_OWNER_CMD_CLEAR_TRIP,
} relay_owner_cmd_type_t;

typedef struct {
    relay_owner_cmd_type_t type;
    bool          energize; // RELAY_OWNER_CMD_ENERGIZE only
    safety_trip_t reason;   // RELAY_OWNER_CMD_TRIP only
} relay_owner_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task_handle = NULL;

// volatile: written only by relay_owner_task, read from any task (the
// energize-while-TRIPPED fast-refusal check below, and
// relay_owner_get_state() for safety_core/telemetry) -- same single-writer/
// volatile-read pattern discrete_task.c uses for its debounced outputs.
static volatile relay_owner_state_t s_state = RELAY_OWNER_STATE_INIT;
// True only while GPIO6 is actually driven high -- see relay_owner_is_
// energized()'s doc comment. Written only by relay_owner_task, alongside
// every gpio_put(SAFTYFW_PIN_RELAY, ...) call below so the two can never
// drift apart.
static volatile bool s_energized = false;

static void relay_owner_task(void *arg)
{
    (void)arg;

    // GRACE begins here -- see this file's header comment for why that is
    // the chosen entry point rather than a separate call from main.c.
    s_state = RELAY_OWNER_STATE_GRACE;
    TickType_t grace_start = xTaskGetTickCount();

    for (;;) {
        relay_owner_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(RELAY_OWNER_QUEUE_WAIT_MS)) == pdTRUE) {
            switch (cmd.type) {
            case RELAY_OWNER_CMD_ENERGIZE:
                if (s_state == RELAY_OWNER_STATE_ARMED) {
                    // GPIO6 high = energized (docs/HARDWARE.md Pico I/O
                    // map). This is the single line in the whole build that
                    // is allowed to do this with a caller-requested "true".
                    gpio_put(SAFTYFW_PIN_RELAY, cmd.energize);
                    s_energized = cmd.energize;
                } else {
                    // GRACE: command accepted/tracked but never actually
                    // energizes (ARCHITECTURE.md section 5 step 8).
                    // TRIPPED: refused entirely -- latched, per
                    // SAFETY_MODEL.md section 6, "a trip latches ...
                    // refused while latched". INIT: not reachable here,
                    // s_state is set to GRACE above before the loop ever
                    // runs. In every one of these cases GPIO6 must not go
                    // high, so it is driven/left low explicitly rather than
                    // relying on cmd.energize being false.
                    gpio_put(SAFTYFW_PIN_RELAY, 0);
                    s_energized = false;
                }
                break;

            case RELAY_OWNER_CMD_TRIP:
                // SAFETY_MODEL.md section 6, trip semantics step 1:
                // de-energize first, before anything else. Latching the
                // state (which is step 2's "reason latched" counterpart on
                // this side -- safety_core.c latches the reason itself via
                // boot_reason_latch_trip() immediately after this command
                // is posted) happens in the same switch case so no other
                // command can be interleaved between "off" and "latched".
                gpio_put(SAFTYFW_PIN_RELAY, 0);
                s_energized = false;
                s_state = RELAY_OWNER_STATE_TRIPPED;
                break;

            case RELAY_OWNER_CMD_CLEAR_TRIP:
                // Unconditional TRIPPED -> ARMED if called -- see
                // relay_owner.h's doc comment on relay_owner_clear_trip():
                // the "refused while the condition still holds" check does
                // not belong here and nothing calls this yet.
                if (s_state == RELAY_OWNER_STATE_TRIPPED) {
                    s_state = RELAY_OWNER_STATE_ARMED;
                }
                break;
            }
        }

        // GRACE -> ARMED, timer-driven, checked every loop iteration
        // (whether or not a command arrived) so a quiet period with no
        // commands still lets the timer expire on schedule.
        if (s_state == RELAY_OWNER_STATE_GRACE) {
            if ((xTaskGetTickCount() - grace_start) >= pdMS_TO_TICKS(SAFTYFW_STARTUP_GRACE_MS)) {
                s_state = RELAY_OWNER_STATE_ARMED;
            }
        }

        watchdog_task_checkin(WATCHDOG_CHECKIN_RELAY_OWNER);
    }
}

bool relay_owner_start(void)
{
    s_cmd_queue = xQueueCreate(RELAY_OWNER_QUEUE_LEN, sizeof(relay_owner_cmd_t));
    if (s_cmd_queue == NULL) {
        return false;
    }

    BaseType_t ok = xTaskCreate(relay_owner_task, "relay_owner", RELAY_OWNER_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_RELAY_OWNER, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}

bool relay_owner_command_energize(bool energize)
{
    if (s_cmd_queue == NULL) {
        return false;
    }
    if (s_state == RELAY_OWNER_STATE_TRIPPED) {
        // Fast, synchronous refusal -- no point queuing a command
        // relay_owner_task will refuse anyway (it re-checks state itself
        // too, since s_state can change between this read and the task
        // servicing the queue).
        return false;
    }

    relay_owner_cmd_t cmd = { .type = RELAY_OWNER_CMD_ENERGIZE, .energize = energize };
    // 0 ticks to wait: a full queue means relay_owner is not draining it,
    // which is a bug worth surfacing as a dropped command rather than
    // blocking the caller (never-block rule, ARCHITECTURE.md section 1).
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool relay_owner_command_trip(safety_trip_t reason)
{
    if (s_cmd_queue == NULL) {
        return false;
    }

    relay_owner_cmd_t cmd = { .type = RELAY_OWNER_CMD_TRIP, .reason = reason };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool relay_owner_clear_trip(void)
{
    if (s_cmd_queue == NULL) {
        return false;
    }

    relay_owner_cmd_t cmd = { .type = RELAY_OWNER_CMD_CLEAR_TRIP };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

relay_owner_state_t relay_owner_get_state(void)
{
    return s_state;
}

bool relay_owner_is_energized(void)
{
    return s_energized;
}
