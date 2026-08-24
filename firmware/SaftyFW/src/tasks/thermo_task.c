// thermo_task.c -- Phase 3: real ~DRDY (GPIO12) interrupt wiring, the ported
// MAX31856 driver (max31856.c/.h, "port firmware/KilnFW/App/drivers/MAX31856.c,
// do not rewrite it"), and thermo_snapshot_t publication. Replaces the Phase 2
// timed-fallback skeleton this file used to be -- see git history for that
// version if it is ever useful as a reference.
//
// Wake source: a falling-edge IRQ on SAFTYFW_PIN_THERMO_DRDY gives this task
// a notification (vTaskNotifyGiveFromISR / ulTaskNotifyTake), matching the
// TODO the Phase 2 skeleton left here. Unlike the main board (DRDY behind an
// SX1509 I/O expander, THERMOCOUPLE.md section 1), this is a real Pico GPIO,
// so "no DRDY edge within ~2x the expected conversion interval" is a
// hardware fact this task can detect directly -- not an elapsed-time guess
// like KilnFW's driver has to fall back to. That detection is the whole
// reason THERMOCOUPLE.md says this port is worth doing carefully: it feeds
// straight into S5 as a sensor-invalid condition (via thermo_snapshot_t.valid
// == false -> safety_guard_input_t.tc_valid == false), which KilnFW's own
// driver structurally cannot produce.
#include "thermo_task.h"

#include <math.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "hardware/gpio.h"

#include "board_pins.h"
#include "config_store.h" // safety_tc_installed (0x0211) -- the structural injection gate, see thermo_task.h
#include "max31856.h"
#include "task_priorities.h"
#include "watchdog_task.h"

#define THERMO_TASK_STACK_WORDS configMINIMAL_STACK_SIZE

// DRDY-silence margin (THERMOCOUPLE.md section 1: "~2x the expected
// conversion interval"). Applied to max31856_conversion_time_ms() at
// runtime, not baked into a constant, so a future change to AVGSEL/filter in
// max31856.c does not leave this multiplier silently out of sync.
#define THERMO_TASK_DRDY_SILENCE_MULTIPLIER 2u

// Before the MAX31856 is configured (or if it never comes up --
// docs/ARCHITECTURE.md section 5 step 6: "failure is logged, not fatal"),
// max31856_conversion_time_ms() returns 0. Wait a bounded, conservative
// interval in that case instead of computing a 0ms (i.e. immediate, busy-
// looping) timeout -- long enough that a genuinely absent part does not spin
// this task, short enough that watchdog_task_checkin() below still runs well
// inside any task's own deadline.
#define THERMO_TASK_UNCONFIGURED_WAIT_MS 500u

static TaskHandle_t s_task_handle = NULL;

static SemaphoreHandle_t s_snapshot_lock = NULL;
static thermo_snapshot_t s_snapshot; // guarded by s_snapshot_lock
static bool s_snapshot_published = false;

// TC value injection (thermo_task.h) -- both guarded by s_snapshot_lock,
// same as s_snapshot above. s_inject_active starts false, is only ever set
// by thermo_task_inject_reading() (after that call's own gate check), and
// is never touched by anything that reaches flash -- a reboot always
// starts this back at false, satisfying "never persisted, cleared on every
// reboot" structurally (there is simply no code path that could make it
// otherwise survive one).
static bool s_inject_active = false;
static thermo_snapshot_t s_inject_snapshot; // meaningless while !s_inject_active

// The only GPIO-IRQ callback registered anywhere in this firmware today
// (discrete_task polls instead of using an IRQ) -- pico-sdk routes every
// GPIO IRQ on a core through one dispatcher, so this checks `gpio` before
// acting rather than assuming it is only ever called for DRDY. Runs in
// interrupt context: touches nothing but the task notification, per
// FreeRTOS's from-ISR API rules.
static void thermo_drdy_isr(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio != SAFTYFW_PIN_THERMO_DRDY) {
        return;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void thermo_task_publish(const thermo_snapshot_t *snap)
{
    if (!s_snapshot_lock) {
        return;
    }
    // Short, bounded wait: this is a single struct copy under the lock, not
    // a bus transaction, so contention should never last long enough to
    // matter -- but a finite wait still beats a task that can hang here
    // forever (matches max31856's/relay_owner's own "report and return
    // rather than wait out a deadlock" discipline).
    if (xSemaphoreTake(s_snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_snapshot = *snap;
    s_snapshot_published = true;
    xSemaphoreGive(s_snapshot_lock);
}

bool thermo_task_get_snapshot(thermo_snapshot_t *out)
{
    if (!out) {
        return false;
    }
    out->timestamp_ms = 0;
    out->valid = false;
    out->tc_c = NAN;
    out->cj_c = NAN;
    out->fault_bits = 0;
    out->spi_failed = false;

    if (!s_snapshot_lock || !s_snapshot_published) {
        return false;
    }
    if (xSemaphoreTake(s_snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    *out = s_snapshot;

    // Injection override, re-gated on EVERY read, not just at the moment
    // thermo_task_inject_reading() was called -- see thermo_task.h's own
    // comment on why this live re-check is what actually closes the "real
    // sensor commissioned mid-injection" window. config_store_get_full_
    // record() is the same cheap cached-copy read safety_core.c's build_
    // input() now does every tick; calling it again here (rather than
    // trusting s_inject_active alone) means a config change from another
    // task is visible on the very next snapshot read, with no separate
    // notification path required.
    if (s_inject_active) {
        config_store_record_t rec;
        config_store_get_full_record(&rec);
        if (rec.safety_tc_installed == 0u) {
            *out = s_inject_snapshot;
        } else {
            // The gate closed underneath an active injection (safety_tc_
            // installed flipped back to 1 by a SET_PARAM/COMMIT_CONFIG since
            // the injection was armed) -- deactivate it here rather than
            // just skipping the override this one call, so the next call
            // does not have to repeat this same "was it still gated"
            // question, and thermo_task_injection_active() (read by link_
            // task's status builder) stops reporting an injection that can
            // no longer take effect.
            s_inject_active = false;
        }
    }

    xSemaphoreGive(s_snapshot_lock);
    return true;
}

bool thermo_task_inject_reading(bool tc_valid, float tc_c, float cj_c, uint8_t fault_bits)
{
    // Structural gate: refuse unless the operator has declared the safety
    // TC not installed. This check is what makes the gate structural rather
    // than advisory -- it runs here, inside the one function that can ever
    // turn injection on, regardless of what any caller (link_task.c's wire
    // command handler, a future test harness, anything else) does or fails
    // to check on its own.
    config_store_record_t rec;
    config_store_get_full_record(&rec);
    if (rec.safety_tc_installed != 0u) {
        return false;
    }

    if (!s_snapshot_lock) {
        return false;
    }
    if (xSemaphoreTake(s_snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    s_inject_snapshot.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_inject_snapshot.valid = tc_valid;
    // Same "NaN when !valid, never 0, never stale" contract every producer
    // in this codebase follows for thermo_snapshot_t (this file's own
    // header comment, snapshots.h's doc comment) -- injected data is not
    // exempt from it.
    s_inject_snapshot.tc_c = tc_valid ? tc_c : NAN;
    s_inject_snapshot.cj_c = tc_valid ? cj_c : NAN;
    s_inject_snapshot.fault_bits = fault_bits;
    s_inject_snapshot.spi_failed = false; // injection models a successful synthetic transfer;
                                           // callers wanting to exercise S5's spi_failed path
                                           // pass tc_valid=false instead, same as a real bad read
    s_inject_active = true;

    xSemaphoreGive(s_snapshot_lock);
    return true;
}

void thermo_task_inject_clear(void)
{
    if (!s_snapshot_lock) {
        return;
    }
    if (xSemaphoreTake(s_snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_inject_active = false;
    xSemaphoreGive(s_snapshot_lock);
}

bool thermo_task_injection_active(void)
{
    return s_inject_active;
}

static void thermo_task_fn(void *arg)
{
    (void)arg;

    // ~DRDY: input, external 10k pull-up (R2, THERMOCOUPLE.md section 1) --
    // no internal pull requested, the board already provides one. Falling
    // edge: the part drives ~DRDY low when a new conversion result is
    // available (datasheet; also THERMOCOUPLE.md section 1's "~DRDY problem"
    // discussion).
    //
    // Armed HERE, at the top of the task body, not from thermo_task_start()
    // (main.c, pre-scheduler) -- deliberately. max31856_configure() (main.c
    // step 6) leaves CMODE running (automatic conversion), so ~DRDY starts
    // toggling on the part's own free-running cadence the moment that call
    // returns, well before main() reaches vTaskStartScheduler() (steps 7's
    // other _start() calls all still have to run first). Arming the GPIO IRQ
    // any earlier than this -- e.g. back in thermo_task_start() -- lets
    // thermo_drdy_isr() fire and call vTaskNotifyGiveFromISR()/
    // portYIELD_FROM_ISR() while the scheduler has not started yet: on this
    // SMP port, core1 and the cross-core critical-section locks it and
    // pxCurrentTCBs[] depend on are not brought up until
    // vTaskStartScheduler() runs (xPortStartScheduler() in port.c), so an
    // ISR touching kernel task/notification state ahead of that is exactly
    // the kind of pre-scheduler FreeRTOS-API-from-ISR call the docs warn
    // against -- and, on real hardware, corrupts scheduler state badly
    // enough to double-fault the very first time it matters (observed via
    // OpenOCD/GDB the first time this firmware ran on a real Pico). This
    // task function only ever runs once vTaskStartScheduler() has handed it
    // control, so arming the IRQ as the first thing it does is the same
    // "task handle must exist before the ISR can fire" discipline the old
    // comment here described, extended one step further: the *scheduler*
    // must be running too.
    gpio_init(SAFTYFW_PIN_THERMO_DRDY);
    gpio_set_dir(SAFTYFW_PIN_THERMO_DRDY, GPIO_IN);
    gpio_set_irq_enabled_with_callback(SAFTYFW_PIN_THERMO_DRDY, GPIO_IRQ_EDGE_FALL, true,
                                        &thermo_drdy_isr);

    for (;;) {
        uint32_t conv_ms = max31856_conversion_time_ms();
        uint32_t timeout_ms = (conv_ms > 0)
                                   ? conv_ms * THERMO_TASK_DRDY_SILENCE_MULTIPLIER
                                   : THERMO_TASK_UNCONFIGURED_WAIT_MS;

        uint32_t notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));

        thermo_snapshot_t snap;
        snap.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (notifications == 0) {
            // DRDY silence: no falling edge within ~2x the expected
            // conversion interval. THERMOCOUPLE.md section 1: "the part has
            // stopped converting" -- a real, hardware-backed staleness
            // signal, not an inference. Do not even attempt a burst read;
            // whatever is in the registers is, by definition, not something
            // a fresh DRDY edge released. Feed straight into S5 as
            // sensor-invalid.
            snap.valid = false;
            snap.tc_c = NAN;
            snap.cj_c = NAN;
            snap.fault_bits = 0;
            snap.spi_failed = false;
        } else {
            // A DRDY edge fired -- sample the burst now, per max31856.h's
            // contract (reading CJTH/CJTL is what releases ~DRDY high again,
            // so this must happen promptly after the wake, not queued behind
            // other work).
            max31856_reading_t reading;
            bool ok = max31856_read(&reading);

            snap.spi_failed = !ok || reading.spi_failed;
            snap.fault_bits = ok ? reading.fault_status : 0;
            snap.tc_c = ok ? reading.tc_temperature_c : NAN;
            snap.cj_c = ok ? reading.cj_temperature_c : NAN;
            // "valid" is the snapshot-level fact safety_guards.h's tc_valid
            // maps onto directly: a successful transfer. Per-half NaN-ing
            // for individual SR fault bits already happened inside
            // max31856_read() (max31856.h's doc comment on
            // max31856_reading_t) and is preserved as-is -- see
            // snapshots.h's thermo_snapshot_t doc comment for why valid ==
            // true does not itself guarantee tc_c/cj_c are both finite.
            snap.valid = ok && !reading.spi_failed;
        }

        thermo_task_publish(&snap);

        watchdog_task_checkin(WATCHDOG_CHECKIN_THERMO_TASK);
    }
}

bool thermo_task_start(void)
{
    s_snapshot_lock = xSemaphoreCreateMutex();
    if (!s_snapshot_lock) {
        return false;
    }

    BaseType_t ok = xTaskCreate(thermo_task_fn, "thermo_task", THERMO_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_THERMO_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);

    // The ~DRDY GPIO IRQ is deliberately NOT armed here. s_task_handle is
    // valid at this point (xTaskCreate() above already set it), but the
    // *scheduler* is not running yet -- thermo_task_start() is called from
    // main(), before vTaskStartScheduler(). Arming the IRQ this early lets it
    // fire (the MAX31856 free-runs ~DRDY once max31856_configure() leaves
    // CMODE running, main.c step 6, well before step 7 finishes) and call
    // FreeRTOS ISR-safe APIs before the kernel's SMP state exists yet -- see
    // thermo_task_fn()'s header comment, where the IRQ is armed instead, for
    // the full reasoning and the real hardware fault this caused.
    return true;
}
