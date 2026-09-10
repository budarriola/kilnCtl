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

#include "hal_gpio.h" // HAL Phase 1b/4 -- heartbeat LED, plain migration debt;
                       // hal_gpio_pico.c's own header comment names this
                       // file's LED as a client it must serve unchanged.
#include "hal_wdt.h"   // HAL Phase 4 -- watchdog_update() feed, exactly the
                       // call site hal_wdt.h's own header comment names
                       // (watchdog_task.c:161, "the recurring feed").

#include "board_pins.h"
#include "startup_diag.h"
#include "watchdog_overdue_diag.h" // 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's actual conclusion, see its own header comment
#include "task_priorities.h"
#include "watchdog_gate.h"

#define WATCHDOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE

// Every registered task's bit, per docs/ARCHITECTURE.md section 4. Still
// used for s_ever_checkin_mask / watchdog_task_all_checked_in_since_boot(),
// and to XOR against watchdog_gate_all_within_deadline()'s ok_mask below to
// get the overdue set watchdog_overdue_diag_mark() latches (2026-08-23,
// scratch[5], repurposed -- see watchdog_overdue_diag.h's own header
// comment).
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
    [WATCHDOG_CHECKIN_LINK_TASK]       =  10u * WATCHDOG_DEADLINE_MARGIN_MULTIPLE,  // LINK_TASK_POLL_MS, link_task.c (lowered 100->10 2026-08-28, see that constant's own comment) (30 ms)
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

        s_diag_watchdog_loops++;

        if (all_ok) {
            // 2026-09-10, opus review finding A (transient-overdue half):
            // clear any stale overdue latch a PRIOR, since-recovered miss
            // left behind, so a later, unrelated reset does not misreport
            // "check-in overdue" for a boot in which nothing was overdue.
            // Never touches a genuine fatal latch -- see the callee's own
            // comment.
            watchdog_overdue_diag_notify_recovered();
            (void)hal_wdt_feed();

            // Toggle, not set-high: a steady blink at half the feed period
            // (500 ms full cycle) is what makes this a *heartbeat* rather
            // than just an "ok" lamp -- a light that is merely on can also
            // be a light nobody is driving anymore (stuck GPIO, task
            // crashed after its last write). Toggling only ever happens
            // here, in the same branch as the real feed, so the LED cannot
            // physically keep moving once this branch stops running.
            s_led_state = !s_led_state;
            (void)hal_gpio_set(SAFTYFW_PIN_HEARTBEAT_LED, s_led_state);
        } else {
            // At least one task is past its own deadline. Do NOT feed --
            // the watchdog will reboot the chip in <= 1s, which is the
            // fail-safe by construction the doc describes.
            //
            // 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's
            // actual conclusion: that reboot was never a fault -- it is
            // THIS branch, firing because some task missed its own deadline
            // for a still-unidentified reason. watchdog_overdue_diag_mark()
            // latches exactly which task(s) (a bitmask, ok_mask's
            // complement) and, for whichever one missed by the largest
            // margin, by how much -- "missed by 20ms" and "missed by
            // 2000ms" point at completely different causes, so the worst
            // offender's overage is recorded, not just its identity.
            // Reset-surviving (watchdog_overdue_diag.h, scratch[5],
            // repurposed from this register's old unconditional-every-
            // evaluation write -- see that file's own header comment for
            // why that repurposing is safe) precisely because a log sink
            // cannot report a starvation that reboots the board a few
            // hundred milliseconds later, and this board has no console
            // header fitted anyway (TODO.md 0.5a).
            uint8_t overdue_mask = (uint8_t)((ok_mask ^ WATCHDOG_CHECKIN_ALL_MASK) & 0xFFu);
            uint8_t worst_task_id = 0;
            uint32_t worst_overage_ms = 0;
            for (unsigned i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
                if ((overdue_mask & (1u << i)) == 0u) {
                    continue; // this task was within its own deadline
                }
                if (entries[i].elapsed_ms <= entries[i].deadline_ms) {
                    continue; // defensive only -- overdue_mask says it should not be, but never trust a derived value over the raw facts it was derived from
                }
                uint32_t overage_ms = entries[i].elapsed_ms - entries[i].deadline_ms;
                if (overage_ms > worst_overage_ms) {
                    worst_overage_ms = overage_ms;
                    worst_task_id = (uint8_t)i;
                }
            }
            watchdog_overdue_diag_mark(overdue_mask, worst_task_id,
                                        (uint16_t)(worst_overage_ms > 0xFFFFu ? 0xFFFFu : worst_overage_ms));

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
    if (hal_gpio_init_out(SAFTYFW_PIN_HEARTBEAT_LED, s_led_state) != HAL_OK) {
        return false;
    }

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
