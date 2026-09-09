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

#include "hal_gpio.h" // HAL Phase 1b/4 -- discrete_task is a hal_gpio client now;
                       // plain migration debt, see docs/HW_ABSTRACTION.md's
                       // "hal_gpio" section and hal_gpio_pico.c's own header
                       // comment, which names this file's E-stop/main-fault
                       // inputs as clients this backend must serve unchanged.

#include "board_pins.h"
#include "debounce_policy.h"
#include "config_store.h"
#include "discrete_pin_policy.h"
#include "task_priorities.h"
#include "watchdog_task.h"

// 2026-09-09: configMINIMAL_STACK_SIZE overflowed on core1 (confirmed via
// SWD -- core1 halted forever inside vApplicationStackOverflowHook, core0
// then deadlocked forever inside xQueueGenericSend's spin_lock_unsafe_
// blocking) during the 2026-09-09 dual-processor flash/commissioning pass.
// Same treatment as current_task.c's CURRENT_TASK_STACK_WORDS: a per-task
// bump rather than raising configMINIMAL_STACK_SIZE itself (that global
// bump was tried and reverted -- see FreeRTOSConfig.h's comment -- because
// every *N-multiplied task's stack scales off it too and it blew the heap).
#define DISCRETE_TASK_STACK_WORDS   (configMINIMAL_STACK_SIZE * 4)

// SAFTYFW_ESTOP_DEBOUNCE_MS / SAFTYFW_MAIN_FAULT_DEBOUNCE_MS /
// SAFTYFW_DEBOUNCE_SAMPLES() now live in debounce_policy.h (2026-09-04) so
// the host tests that exercise the real debounce (test/test_debounce_
// nuisance.c) can #include the same source of truth this file uses instead
// of hand-copying it. SAFTYFW_DEBOUNCE_SAMPLES() here takes the period
// explicitly -- evaluated at compile time from task_priorities.h's period
// constant below, so a period change (e.g. a faster discrete_task) widens or
// narrows N automatically instead of silently under- or over-debouncing.

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_estop_pressed = false;
static volatile bool s_main_fault = false;

// The consecutive-sample debounce itself now lives in debounce_policy.h/.c
// (a pure, host-tested module -- test/test_debounce_policy.c and
// test/test_guard_nuisance.c), pulled out for exactly the reason discrete_
// pin_policy.h/.c was: it is not reachable from a host test as a static
// function gated behind this file's FreeRTOS/RP2040-GPIO includes, and
// GUARD_TEST_MATRIX.md section 1's S6a/S7 nuisance rows need to drive the
// real debounce logic, not a description of it. debounce_policy_state_t /
// debounce_policy_update() below are aliased so the rest of this function
// reads unchanged.
typedef debounce_policy_state_t debounce_state_t;
#define debounce_update debounce_policy_update

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

    const uint32_t estop_n = SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS, SAFTYFW_PERIOD_DISCRETE_TASK_MS);
    const uint32_t main_fault_n = SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS, SAFTYFW_PERIOD_DISCRETE_TASK_MS);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_DISCRETE_TASK_MS));

        // Raw-level-to-logical-meaning mapping now lives in discrete_pin_
        // policy.h/.c (a pure, host-tested module -- test/test_discrete_
        // pin_policy.c) rather than inline here, specifically so the two
        // pins' OPPOSITE polarities are each pinned down by their own named
        // function instead of one bare `!`/no-`!` choice a future edit
        // could silently get backwards again. See that header's own
        // comment for the full HARDWARE.md section 5 reasoning (GPIO9
        // active HIGH for stop, covering pressed/cut-wire/unfitted
        // identically; GPIO10 active LOW for mainFault) and for why this
        // bug -- GPIO9 read inverted for a long stretch, silently disabling
        // S7 in both directions -- was invisible to the existing test
        // suite (virtual_dut synthesizes `estop_pressed` directly and never
        // exercises a GPIO read).
        //
        // Polarity is configurable as of 2026-09-08 (param 0x0212), read
        // fresh on every sample -- the same "re-read where it's used"
        // pattern config_store_get_tc_type() uses, so a SET_PARAM takes
        // effect without a reboot, and so a caller running before the store
        // is loaded gets the fail-safe ACTIVE_HIGH rather than a zeroed
        // cache. The DEFAULT is unchanged and is what this bench is wired
        // for (GPIO9 measured LOW = healthy, safe to fire), so this
        // addition changes no behaviour on any existing board.
        bool estop_raw = discrete_pin_policy_estop_asserted_ex(
            hal_gpio_get(SAFTYFW_PIN_ESTOP), config_store_get_estop_active_level());
        bool main_fault_raw =
            discrete_pin_policy_main_fault_asserted(hal_gpio_get(SAFTYFW_PIN_MAIN_FAULT));

        s_estop_pressed = debounce_update(&estop_db, estop_raw, estop_n);
        s_main_fault = debounce_update(&main_fault_db, main_fault_raw, main_fault_n);

        watchdog_task_checkin(WATCHDOG_CHECKIN_DISCRETE_TASK);
    }
}

bool discrete_task_start(void)
{
    // matches R10's external pull-up; belt and suspenders
    if (hal_gpio_init_in(SAFTYFW_PIN_ESTOP, HAL_GPIO_PULL_UP) != HAL_OK) {
        return false;
    }

    // matches R8's external pull-up
    if (hal_gpio_init_in(SAFTYFW_PIN_MAIN_FAULT, HAL_GPIO_PULL_UP) != HAL_OK) {
        return false;
    }

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
