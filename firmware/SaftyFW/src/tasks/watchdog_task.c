// watchdog_task.c -- see watchdog_task.h. Feeds the hardware watchdog iff
// EVERY registered task has checked in within ITS OWN deadline (see
// s_checkin_deadline_ms below), evaluated every SAFTYFW_PERIOD_WATCHDOG_TASK_MS
// (250 ms). The actual per-task comparison lives in watchdog_gate.c/.h, kept
// deliberately free of FreeRTOS/pico-sdk so it is directly host-testable
// (test/test_watchdog_gate.c) rather than only reachable via a source-text
// scan.
//
// This replaces an earlier gate that required every task to land in the SAME
// 250 ms window, which was structurally impossible: thermo_task's real
// cadence (500 ms whenever the MAX31856 is unconfigured -- true on this bench
// board, the safety thermocouple IC is not fitted) is itself longer than that
// window. See watchdog_gate.h's header comment and this file's git history
// for the measured consequence (a hardware-watchdog reset roughly every
// 9 s, TIMER reset reason) and how it was diagnosed.
//
// Also owns the physical heartbeat LED (TODO.md Phase 2, "Heartbeat LED,
// physical, on the safety processor itself" -- requested 2026-08-17). This
// is a side effect of the feed decision below, not a second check: it must
// never be able to make a stalled system look alive, so the LED is only
// ever touched in the same branch that decides whether to feed the real
// hardware watchdog, using the exact same `all_ok` result from
// watchdog_gate_all_within_deadline(). There is deliberately no separate
// "is everything ok" computation for the LED to drift out of sync with.
#include "watchdog_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "board_pins.h"
#include "startup_diag.h"
#include "task_priorities.h"
#include "watchdog_gate.h"

#define WATCHDOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE

// Every registered task's bit, per docs/ARCHITECTURE.md section 4. Still
// used for s_ever_checkin_mask / watchdog_task_all_checked_in_since_boot()
// and as the "everything present" value the SAFTYFW_LAST_CHECKIN_MASK_SCRATCH
// comment (startup_diag.h) compares a captured mask against.
#define WATCHDOG_CHECKIN_ALL_MASK   ((1u << WATCHDOG_CHECKIN_COUNT) - 1u)

// --- Per-task check-in deadlines --------------------------------------------
//
// Each deadline is derived from that task's own real worst-case check-in
// interval (its fixed period where it has one, task_priorities.h's
// SAFTYFW_PERIOD_* constants; its poll/queue-wait constant where it does
// not), times a flat WATCHDOG_DEADLINE_MARGIN_MULTIPLE for scheduling jitter
// and occasional slow passes, capped at WATCHDOG_DEADLINE_CAP_MS.
//
// The cap exists because every deadline must stay comfortably under
// SAFTYFW_WATCHDOG_TIMEOUT_MS (1000 ms, main.c) with room left for THIS
// task's own SAFTYFW_PERIOD_WATCHDOG_TASK_MS (250 ms) evaluation period --
// otherwise a stall could be detected too late for the "don't feed" decision
// to matter before the hardware watchdog fires anyway, and diagnosing a
// specific stalled task over SWD (the whole point of the scratch-register
// latch below) would be moot. 700 ms leaves 300 ms of headroom, which is
// more than one full watchdog_task period, so a deadline breach is always
// caught and latched at least one evaluation before the hardware timeout
// could fire on its own.
//
// A flat 3x margin on the two slowest tasks (LOG_TASK's 500 ms poll,
// THERMO_TASK's 500 ms unconfigured wait) would ask for a 1500 ms deadline,
// which does not fit under the cap. Per requirement 3: rather than fudge
// those two a bespoke, smaller multiple, they are simply capped at
// WATCHDOG_DEADLINE_CAP_MS (700 ms, a 1.4x margin) -- explicitly less grace
// than every other task gets, and still far more than their own period, so a
// task that is genuinely dead is still caught inside one hardware-watchdog
// timeout, just with less jitter tolerance than a 3x margin would have given
// it. If either task's real period ever grows, this comment and the cap need
// re-checking together.
#define WATCHDOG_DEADLINE_MARGIN_MULTIPLE 3u
#define WATCHDOG_DEADLINE_CAP_MS           700u

// clang-format off
static const uint32_t s_checkin_deadline_ms[WATCHDOG_CHECKIN_COUNT] = {
    // id                              = period_ms * MARGIN, capped            -- source of period_ms
    [WATCHDOG_CHECKIN_RELAY_OWNER]     = 200u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // RELAY_OWNER_QUEUE_WAIT_MS, relay_owner.c            (600 ms)
    [WATCHDOG_CHECKIN_SAFETY_CORE]     = 100u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // SAFTYFW_PERIOD_SAFETY_CORE_MS, task_priorities.h    (300 ms)
    [WATCHDOG_CHECKIN_DISCRETE_TASK]   =  10u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // SAFTYFW_PERIOD_DISCRETE_TASK_MS, task_priorities.h   (30 ms)
    [WATCHDOG_CHECKIN_THERMO_TASK]     = WATCHDOG_DEADLINE_CAP_MS,                  // THERMO_TASK_UNCONFIGURED_WAIT_MS, thermo_task.c: 500 ms * 3 = 1500 ms, capped (see comment above)
    [WATCHDOG_CHECKIN_CURRENT_TASK]    =  50u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // SAFTYFW_PERIOD_CURRENT_TASK_MS, task_priorities.h    (150 ms)
    [WATCHDOG_CHECKIN_LINK_TASK]       = 100u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // LINK_TASK_POLL_MS, link_task.c                       (300 ms)
    [WATCHDOG_CHECKIN_LOG_TASK]        = WATCHDOG_DEADLINE_CAP_MS,                  // LOG_TASK_POLL_MS, log_task.c: 500 ms * 3 = 1500 ms, capped (see comment above)
    [WATCHDOG_CHECKIN_UPDATE_TASK]     = 100u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // UPDATE_TASK_POLL_MS, update_task.c                   (300 ms)
};
// clang-format on

// Last-check-in tick, one per task, updated by watchdog_task_checkin() and
// read (under the same critical-section discipline) by watchdog_task_fn().
// Initialized in watchdog_task_start(), before the scheduler runs, to the
// tick count at that moment -- so a task that has not checked in even once
// yet reads as "silent since boot", not as some huge bogus elapsed value.
static volatile TickType_t s_last_checkin_tick[WATCHDOG_CHECKIN_COUNT];

// Cumulative-since-boot mask, never cleared -- see
// watchdog_task_all_checked_in_since_boot()'s doc comment in
// watchdog_task.h for why this needs to be a second, independent bitmask
// rather than reused from anything the deadline gate maintains (the
// deadline gate's own notion of "ok" is deliberately transient: a task can
// be within its own deadline again next window after having been silent
// past it, and that must not read as "always fine since boot").
static volatile uint32_t s_ever_checkin_mask = 0;
static TaskHandle_t s_task_handle = NULL;
// Only ever read/written from watchdog_task_fn() itself (single-writer, no
// other task touches the LED), so unlike the check-in state this needs
// neither volatile nor a critical section.
static bool s_led_state = false;

// Diagnostic: counts watchdog_task_fn() passes since boot. Kept (unlike the
// per-checkin stack-headroom instrumentation removed below) because it
// answers a question the scratch mask cannot -- whether this task is running
// at all between resets, and how many times -- and it costs nothing on the
// hot path other tasks go through (watchdog_task_checkin() itself touches
// none of this).
static volatile uint32_t s_diag_watchdog_loops = 0;

static void watchdog_task_fn(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_WATCHDOG_TASK_MS));

        TickType_t now = xTaskGetTickCount();

        // Snapshot last-check-in ticks under a short critical section:
        // checkin bits set by other tasks concurrently with this read land
        // either just before or just after the snapshot, which is fine
        // either way -- worst case this evaluation treats a check-in that
        // landed a tick or two ago as not-yet-happened, and it will simply
        // be picked up on the very next 250 ms pass.
        TickType_t last_checkin[WATCHDOG_CHECKIN_COUNT];
        taskENTER_CRITICAL();
        for (unsigned i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
            last_checkin[i] = s_last_checkin_tick[i];
        }
        taskEXIT_CRITICAL();

        watchdog_gate_entry_t entries[WATCHDOG_CHECKIN_COUNT];
        for (unsigned i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
            TickType_t elapsed_ticks = now - last_checkin[i]; // unsigned: wraps correctly
            entries[i].elapsed_ms = (uint32_t)(elapsed_ticks * portTICK_PERIOD_MS);
            entries[i].deadline_ms = s_checkin_deadline_ms[i];
        }

        uint32_t ok_mask = 0;
        bool all_ok = watchdog_gate_all_within_deadline(entries, WATCHDOG_CHECKIN_COUNT, &ok_mask);

        // Publish the observed per-task ok/not-ok mask where it survives the
        // reset it may be about to cause. A log sink cannot report a
        // starvation that reboots the board a few hundred milliseconds
        // later, and this board has no console header fitted anyway
        // (TODO.md 0.5a). A scratch register does survive, so after a
        // watchdog reboot the last pre-reset evaluation is still readable
        // over SWD.
        //
        // Meaning, now that the gate is per-task rather than one-window: bit
        // i set means check-in id i was WITHIN ITS OWN DEADLINE at this
        // evaluation, not "checked in during this exact 250 ms window" --
        // see startup_diag.h's updated comment on
        // SAFTYFW_LAST_CHECKIN_MASK_SCRATCH. ok_mask ^ WATCHDOG_CHECKIN_ALL_MASK
        // is still the set of tasks that were the problem, same as before.
        // Written unconditionally, before the feed decision, so it never
        // reads as "everything was fine" merely because the write was
        // skipped -- and OR'd with SAFTYFW_LAST_CHECKIN_WRITTEN so that a
        // stored zero is distinguishable from a register nothing has touched
        // since power-on (see startup_diag.h).
        watchdog_hw->scratch[SAFTYFW_LAST_CHECKIN_MASK_SCRATCH] =
            ok_mask | SAFTYFW_LAST_CHECKIN_WRITTEN;

        s_diag_watchdog_loops++;

        if (all_ok) {
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
        // else: at least one task is past its own deadline. Do NOT feed --
        // the watchdog will reboot the chip in <= 1s, which is the
        // fail-safe by construction the doc describes. TODO: log which
        // bit(s) were missing once log_task exists (Phase 2 later item /
        // Phase 8), so a watchdog reboot's cause is diagnosable rather than
        // just "it happened" -- the scratch register above already lets an
        // SWD session answer this today.
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
    TickType_t boot_tick = xTaskGetTickCount();
    for (unsigned i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
        s_last_checkin_tick[i] = boot_tick;
    }
    s_ever_checkin_mask = 0;

    // Heartbeat LED: plain digital out, driven low (off) until the first
    // full check-in window closes and the toggle above runs -- see
    // board_pins.h for why GPIO25 is a real pin here despite having no A1
    // schematic net (it is the Pico module's own onboard LED).
    s_led_state = false;
    gpio_init(SAFTYFW_PIN_HEARTBEAT_LED);
    gpio_set_dir(SAFTYFW_PIN_HEARTBEAT_LED, GPIO_OUT);
    gpio_put(SAFTYFW_PIN_HEARTBEAT_LED, s_led_state);

    // The hardware watchdog is armed exactly once, by main() step 3, using
    // SAFTYFW_WATCHDOG_TIMEOUT_MS. This function deliberately does NOT arm it.
    //
    // It used to, with its own hardcoded WATCHDOG_HW_TIMEOUT_MS, and that was
    // a real bug: watchdog_task_start() runs on core 0 after main() has
    // already armed the watchdog, so pico-sdk's watchdog_enable() reprogrammed
    // load/ctrl and silently overrode main()'s timeout. Editing
    // SAFTYFW_WATCHDOG_TIMEOUT_MS then had no effect on hardware at all --
    // the running chip kept showing a 1 s period no matter what main() asked
    // for, which cost a bench session before the second call was found.
    // Feeding the watchdog is this task's job; arming it is not.

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
    // safe against the periodic snapshot above running on the other core.
    // Cheap and never blocks the caller.
    //
    // A per-checkin uxTaskGetStackHighWaterMark(NULL) diagnostic used to live
    // here (2026-08-23), recording the calling task's own stack headroom.
    // It measured what it needed to -- no task is near overflow, the
    // tightest is current_task at 71 words free of 256 -- and its cost
    // (a walk of the unused-pattern fill, on EVERY check-in, from EVERY
    // task) distorted exactly the timing this code is about: the extra
    // jitter it added was enough by itself to drop measured uptime between
    // watchdog resets from ~9 s to ~1.1 s. Removed now that it has answered
    // its question; this function is back to just setting a bit and a
    // timestamp.
    taskENTER_CRITICAL();
    s_last_checkin_tick[id] = xTaskGetTickCount();
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
