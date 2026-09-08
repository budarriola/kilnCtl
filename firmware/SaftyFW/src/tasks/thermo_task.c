// thermo_task.c -- Phase 3: real ~DRDY (GPIO12) interrupt wiring, the ported
// MAX31856 driver (max31856.c/.h, "port firmware/KilnFW/App/drivers/hw/MAX31856.c,
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

#include "hardware/gpio.h" // permanent holdout: DRDY, one of the two raw GPIO IRQ owners (shared per-core dispatcher, a second registrant would clobber it) -- see firmware/hwAbstraction/README.md "Permanent holdouts"

#include "board_pins.h"
#include "config_store.h" // safety_tc_installed (0x0211) -- the structural injection gate, see thermo_task.h
#include "log_task.h" // log_task_log() -- one WARN line when the reconfig retry gives up, see below
#include "max31856.h"
#include "max31856_reconfig_retry.h" // periodic re-probe while tc_type is unverified, see its own header
#include "max31856_tc_range_policy.h" // per-tc_type plausibility band, see its own header for the full argument
#include "task_priorities.h"
#include "watchdog_task.h"

#define THERMO_TASK_STACK_WORDS configMINIMAL_STACK_SIZE

// DRDY-silence margin (THERMOCOUPLE.md section 1: "~2x the expected
// conversion interval"). Applied to max31856_conversion_time_ms() at
// runtime, not baked into a constant, so a future change to AVGSEL/filter in
// max31856.c does not leave this multiplier silently out of sync.
#define THERMO_TASK_DRDY_SILENCE_MULTIPLIER 2u

/* SAFTYFW_THERMO_ASSUME_DRDY -- BENCH-ONLY, and deliberately loud.
 *
 * WHAT IT DOES. When set, a DRDY-silence timeout below stops meaning "the
 * part has stopped converting, publish sensor-invalid" and instead means
 * "assume a conversion has completed by now and read the burst anyway". The
 * ~DRDY interrupt path is left completely intact: if a real falling edge
 * arrives it is used exactly as before, and this fallback never runs. This
 * only ever changes what happens when the edge does NOT arrive.
 *
 * WHY IT EXISTS. On the bench of 2026-08-25 the fixture standing in for the
 * MAX31856 (firmware/SimFW's PIO slave emulation) has its SPI bus wired but
 * its ~DRDY line NOT wired -- fixture GP26 to this board's GP12 is simply
 * absent, confirmed by toggling each signal on one board and reading it on
 * the other: SCK, MOSI, CS and MISO all follow, ~DRDY does not. With GP12
 * sitting high on its 10k pull-up (R2) no falling edge can ever occur, so
 * thermo_task never issues a single read and every snapshot is
 * sensor-invalid. That blocks all other thermocouple-path work behind one
 * missing jumper, which is the only reason this switch exists.
 *
 * WHY THE EXISTING TIMEOUT IS ALREADY THE RIGHT DELAY, with datasheet
 * numbers rather than a guessed sleep. The wait below is
 * max31856_conversion_time_ms() * THERMO_TASK_DRDY_SILENCE_MULTIPLIER. For
 * this board's fixed configuration -- 60Hz notch, AVGSEL = 4 samples, set in
 * max31856.c's configure() -- the datasheet gives (p.4 tCONV table, and the
 * averaging note on p.20):
 *
 *   steady state, auto mode conversions 2..n, 60Hz:
 *       90 ms max + (4-1) * 16.67 ms  = 140 ms
 *   first conversion after configure(), 60Hz:
 *       155 ms max + (4-1) * 33.33 ms = 255 ms
 *
 * max31856_conversion_time_ms() reports 151 ms (100 + 3*17, already rounded
 * up from the 140 ms steady-state figure), so the wait is 302 ms. That
 * exceeds BOTH the 140 ms steady-state worst case and the 255 ms
 * first-conversion worst case, so by the time this fallback fires a
 * conversion has certainly completed and the registers hold a real result.
 * No additional sleep is needed or wanted; adding one would only slow the
 * sample rate. Sampling lands at about 3.3 Hz, against the roughly 7 Hz a
 * working ~DRDY would give.
 *
 * WHAT IS LOST, stated plainly because it is a safety-relevant reduction.
 * DRDY silence is a real, hardware-backed staleness detector -- it is how
 * this firmware notices that the part has died, hung, or been unplugged
 * while still returning whatever its registers last held. With this option
 * on that detector is gone: a genuinely dead MAX31856 will be read on a
 * timer and its stale registers published as though fresh. The part's own
 * fault bits and the per-type plausibility band still apply, but neither of
 * them catches "the part stopped converting". That is precisely why this
 * defaults to OFF, is not something to leave on, and must be removed the
 * moment the ~DRDY wire is fitted.
 *
 * The build-time announcement below is deliberate: it is the tripwire that
 * stops an enabled configure() from being forgotten in a checked-in build
 * directory, since a CMake cache persists across rebuilds and nobody reads
 * configure output twice. It is a #pragma message and NOT a #warning on
 * purpose -- this project builds with -Werror=cpp, so a #warning here does
 * not warn, it fails the build outright and makes the option unusable. A
 * #pragma message prints on every compile of this file without that. The
 * CMakeLists.txt option also emits a configure-time message(WARNING). */
#ifndef SAFTYFW_THERMO_ASSUME_DRDY
#define SAFTYFW_THERMO_ASSUME_DRDY 0
#endif
#if SAFTYFW_THERMO_ASSUME_DRDY
#pragma message("SAFTYFW_THERMO_ASSUME_DRDY is ON: ~DRDY staleness detection is DISABLED (bench-only; turn this off once the ~DRDY wire is fitted)")
#endif

/* Counts fallback reads taken without a ~DRDY edge. Non-zero is impossible in
 * a normal build (the branch is compiled out), so this doubles as the
 * SWD-readable proof of which mode a running board is actually in. */
static volatile uint32_t s_drdy_assumed_reads = 0;

// Bring-up bug (TODO.md): the safety MAX31856's ONE configure() attempt
// (main.c, pre-scheduler) fails if the IC is not powered/settled yet,
// latching "safety TC invalid" for the rest of the boot. This state drives
// max31856_reconfig_retry.h's periodic re-probe from this task's own loop
// below -- see that header for the full cadence/bound rationale.
// s_reconfig_retries (SWD-readable, same discipline as s_drdy_assumed_
// reads above) mirrors the state struct's retry_count so a bench session
// can see how many re-probes a given boot actually needed.
static max31856_reconfig_retry_state_t s_reconfig_retry;
static volatile uint32_t s_reconfig_retries = 0;
static volatile bool s_reconfig_gave_up = false;

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

    max31856_reconfig_retry_init(&s_reconfig_retry);

    for (;;) {
        // Re-probe the part while its tc_type has never verified -- see
        // max31856_reconfig_retry.h for the cadence/bound. Attempted before
        // this cycle's conv_ms/timeout are computed so that a retry which
        // succeeds THIS iteration already gets the real DRDY-based timeout
        // below, instead of waiting one more full cycle on the
        // unconfigured fallback. Does not touch S5/trip-latch semantics:
        // it only ever changes whether max31856_tc_type_verified() -- and
        // therefore snap.valid below -- can become true again; a trip that
        // already latched stays latched regardless.
        uint32_t retry_now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool verified_before_retry = max31856_tc_type_verified();
        if (max31856_reconfig_retry_should_attempt(&s_reconfig_retry, verified_before_retry,
                                                    retry_now_ms)) {
            (void)max31856_configure(config_store_get_tc_type());
            bool verified_after_retry = max31856_tc_type_verified();
            if (verified_after_retry && !verified_before_retry) {
                // max31856_configure() just wrote fresh CR1 tc-type bits, but a
                // conversion started under the OLD CR1 may already be in
                // flight and could deliver its DRDY edge (and stale-format
                // result) any time after this point. Discard any notification
                // already pending -- and any that arrives from that in-flight
                // conversion -- so the wait below only ever wakes on a
                // conversion started after the reconfigure, never decodes a
                // burst read against the type that is now live.
                (void)ulTaskNotifyTake(pdTRUE, 0);
            }
            max31856_reconfig_retry_note_result(&s_reconfig_retry, verified_after_retry,
                                                 retry_now_ms);
            s_reconfig_retries = s_reconfig_retry.retry_count;
            bool gave_up_before = s_reconfig_gave_up;
            s_reconfig_gave_up = s_reconfig_retry.gave_up;
            if (s_reconfig_gave_up && !gave_up_before) {
                // Bound reached with no success -- log once, loudly, per the
                // bug report's "then give up loudly" (see
                // max31856_reconfig_retry.h's MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS
                // comment). No new wire field: this reuses the existing
                // best-effort log path the same way safety_core.c's trip
                // logging does.
                log_task_log(LOG_LEVEL_WARN, "thermo_task",
                             "safety MAX31856 reconfig retry gave up, tc_type unverified");
            }
        }

        uint32_t conv_ms = max31856_conversion_time_ms();
        uint32_t timeout_ms = (conv_ms > 0)
                                   ? conv_ms * THERMO_TASK_DRDY_SILENCE_MULTIPLIER
                                   : THERMO_TASK_UNCONFIGURED_WAIT_MS;

        uint32_t notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));

        thermo_snapshot_t snap;
        snap.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        // SAFTYFW_THERMO_ASSUME_DRDY: treat the elapsed wait as proof that a
        // conversion finished, rather than as proof the part is dead. See that
        // macro's comment above for the datasheet arithmetic showing the wait
        // already exceeds the worst-case conversion time, and for exactly what
        // detection this gives up.
        bool assume_ready = false;
#if SAFTYFW_THERMO_ASSUME_DRDY
        if (notifications == 0) {
            assume_ready = true;
            s_drdy_assumed_reads++;
        }
#endif

        if (notifications == 0 && !assume_ready) {
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
            // tc_offset_c (owner request 2026-09-08): a calibration
            // correction ADDED here, at the earliest point this task ever
            // has a real hot-junction reading, so every downstream consumer
            // -- fault-bit NaN-ing already applied inside max31856_read(),
            // the per-type plausibility check just below, S1/S2/S10/S13's
            // guard inputs, and the telemetry frame this snapshot feeds --
            // sees the CORRECTED value uniformly. Applied only when the raw
            // reading is itself finite (an already-NaN'd half-reading plus a
            // finite offset must stay NaN, not become a plausible-looking
            // number); config_store_get_tc_offset_c() returns 0.0f (a no-op
            // add) both before commissioning and before config_store_boot_
            // load() has even run, so this is unconditionally safe to call.
            float raw_tc_c = ok ? reading.tc_temperature_c : NAN;
            snap.tc_c = isnan(raw_tc_c) ? raw_tc_c : (raw_tc_c + config_store_get_tc_offset_c());
            snap.cj_c = ok ? reading.cj_temperature_c : NAN;
            // "valid" is the snapshot-level fact safety_guards.h's tc_valid
            // maps onto directly: a successful transfer. Per-half NaN-ing
            // for individual SR fault bits already happened inside
            // max31856_read() (max31856.h's doc comment on
            // max31856_reading_t) and is preserved as-is -- see
            // snapshots.h's thermo_snapshot_t doc comment for why valid ==
            // true does not itself guarantee tc_c/cj_c are both finite.
            snap.valid = ok && !reading.spi_failed;

            // Per-type plausibility (TODO.md Phase 3, "Per-type plausibility
            // ranges" -- max31856_tc_range_policy.h). Checked here, the
            // smallest place that can feed the existing S5 sensor-invalid
            // path with no changes to safety_guards.h/.c or its input
            // struct: this task already owns the one and only place
            // thermo_snapshot_t.valid is decided from a real transfer, and
            // safety_core.c already maps that straight onto
            // safety_guard_input_t.tc_valid, which s5_bad_read_now()
            // (safety_guards.c) already treats as a bad read.
            //
            // Only ever downgrades valid -> invalid, never the reverse: a
            // reading that already failed for any other reason (spi_failed,
            // !ok) stays invalid regardless of what this check would say.
            //
            // Keyed off config_store_get_tc_type() -- the OPERATOR'S
            // commissioned belief -- not off whatever CR1 the part itself
            // currently holds. This is deliberately a SECOND, independent
            // layer over the MAX31856's own unmaskable TCRANGE fault bit
            // (already folded into fault_bits/s5_bad_read_now() above): it
            // catches config_store and the part's actual CR1 going out of
            // sync (e.g. a failed max31856_configure() SPI write leaving the
            // part on its old/default type while config_store believes a
            // different one was commissioned) -- see
            // max31856_tc_range_policy.h's file header for the full
            // "confident, plausible, WRONG" argument.
            //
            // UPDATED 2026-08-24: tc_type now has a real commissioning bit
            // (config_store_is_tc_type_set(), CONFIG_STORE_SET_TC_TYPE --
            // config_store.h), so this check no longer applies a single
            // type's exact band unconditionally, replacing the old
            // "applied UNCONDITIONALLY" behaviour this comment used to
            // describe:
            //   - genuinely commissioned: the operator's own type gets the
            //     tight datasheet band exactly as before.
            //   - never commissioned: config_store_get_tc_type() still
            //     returns a real, usable byte (CONFIG_STORE_DEFAULT_TC_TYPE
            //     == K), but treating that byte as a confirmed type would be
            //     asserting precision commissioning never granted -- so an
            //     uncommissioned board instead gets max31856_tc_range_
            //     policy.h's widest-band floor, deliberately WIDER than K's
            //     own band (see that header's file comment for the union
            //     math and why this is a safe widening, not a regression:
            //     this check was never the primary ceiling for an
            //     uncommissioned board, and both S1's abs_max_temp_c gating
            //     and the MAX31856's own hardware TCRANGE bit are unaffected
            //     by this change).
            //
            // Part B, same "only ever downgrades" rule: max31856_tc_type_
            // verified() reports whether the LAST max31856_configure() call
            // confirmed, via a CR1 readback, that the part actually accepted
            // the type it was asked to run -- a cheap cached-bool read here,
            // not a fresh SPI transfer (see that getter's own doc comment,
            // max31856.h). A part running a DIFFERENT type than either
            // config_store or this task believes defeats the plausibility
            // band above just as thoroughly as it defeats the part's own
            // TCRANGE bit, so this is checked independently and combines
            // with the band result rather than replacing it.
            if (snap.valid) {
                if (!max31856_tc_type_verified()) {
                    snap.valid = false;
                } else {
                    uint8_t configured_type = config_store_get_tc_type();
                    bool plausible = config_store_is_tc_type_set()
                        ? max31856_tc_range_is_plausible(configured_type, snap.tc_c)
                        : max31856_tc_range_is_plausible_uncommissioned(snap.tc_c);
                    if (!plausible) {
                        snap.valid = false;
                    }
                }
            }
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
