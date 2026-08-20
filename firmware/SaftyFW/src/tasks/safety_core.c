// safety_core.c -- Phase 4/5: assembles a safety_guard_input_t every tick,
// calls the pure evaluator, and on a new trip commands relay_owner and
// latches the reason. The GRACE/ARMED/TRIPPED state machine itself lives in
// relay_owner.c (Phase 5) -- this task only ever issues energize/trip/clear
// commands and never tracks that state on its own, per
// docs/ARCHITECTURE.md's "safety_core commands relay_owner" arrow.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): no #include of
// any link or uart header, ever, in this file. Nothing below reaches for one,
// and it should stay that way -- a frozen ESP must not be able to hang this
// task, and the only way to make that a structural property rather than a
// promise is for this compilation unit to be physically incapable of calling
// into the link path. tools/check_isolation.ps1 greps for this.
// discrete_task_estop_pressed() and relay_owner.h/boot_reason.h are fine to
// include here -- neither is link-shaped, and check_isolation.ps1 only
// greps for "uart"/"link" in an #include line, not for these.
#include "safety_core.h"

#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/time.h" // to_ms_since_boot(get_absolute_time()) -- hardware timing, not
                        // link/uart-shaped, fine for check_isolation.ps1 (same header
                        // link_task.c uses for its own uptime_ms fields, so
                        // safety_core_get_trip_event()'s out_uptime_ms shares that clock base)

#include "task_priorities.h"
#include "watchdog_task.h"

#include "boot_reason.h"
#include "discrete_task.h"
#include "reboot_announce.h" // SAFETY_CMD_ANNOUNCE_REBOOT (0x18) grace-window fact for
                              // S6b -- see that header's own doc comment for why this,
                              // and not link_task.h, is the safe way to cross the
                              // link->safety_core boundary tools/check_isolation.ps1 enforces.
#include "relay_owner.h"
#include "safety_guards.h"
#include "thermo_task.h"

#define SAFETY_CORE_STACK_WORDS   configMINIMAL_STACK_SIZE

// KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP reboots"
// line: how long S6b's trip stays suppressed after the most recent
// ANNOUNCE_REBOOT frame. Chosen the same way SAFETY_LINK_STALE_MS/SAFETY_
// LINK_FIRING_ABORT_SILENCE_MS (firmware/KilnFW/App/drivers/safety_link.h)
// were -- a reasonable software timeout with generous margin, not a measured
// physical constant (unlike S8's rate-of-rise threshold, which SAFETY_MODEL.md
// explicitly forbids guessing because it depends on this kiln's actual mass
// and element power). An ESP32-S3 OTA self-reboot needs to: reset, run the
// ROM/second-stage bootloader, bring up FreeRTOS and Wi-Fi, and reach
// safety_link's first PUSH_CONTEXT poll cycle -- plausibly a few seconds on a
// clean boot. 20s gives roughly 2x margin over that without getting anywhere
// close to link_dead_hard_s's own 120s unconditional backstop (so a genuinely
// stuck reboot still gets caught by the hard backstop shortly after this
// window closes, not held open indefinitely).
#define REBOOT_GRACE_WINDOW_MS 20000u

static TaskHandle_t s_task_handle = NULL;

// Thresholds. Zero-initialized (static storage, so this is implicit, but
// spelled out for the reader): abs_max_temp_c has no default at all
// (safety_guards.h's own doc comment, SAFETY_MODEL.md section 4, S1) so 0
// here correctly means "not commissioned, S1 never trips" rather than a
// silently-substituted ceiling. S5/S11/S12's thresholds DO have real
// defaults per that same doc convention, and safety_guards.c substitutes
// them itself whenever the corresponding cfg field is 0 -- confirmed by
// reading safety_guards.c's effective_f()/effective_u16() call sites, not
// guessed. Nothing here needs to set them.
// TODO (Phase 9, config_store): load real commissioned values (abs_max_temp_c
// above all) from flash once config_store.c exists. Until then this cfg is
// permanently the conservative "nothing commissioned" state.
static safety_guard_cfg_t s_guard_cfg;
static safety_guard_state_t s_guard_state;

// Trip-event capture (Frame D, SAFETY_CMD_TRIP_EVENT) -- written only from
// safety_core_task() the instant safety_guards_tick() reports newly_tripped
// (this task's own thread), read from any task via
// safety_core_get_trip_event(). Same single-writer/plain-read reasoning
// link_task.c already documents for its own s_context_frames_ok/bad and
// s_status_tx_ok_count: one task ever writes these, so no lock is needed for
// a handful of scalars this small, and a torn read here is no worse than
// this codebase's existing pattern for the same class of counter.
// s_trip_seq == 0 means "no trip yet this boot" (matches
// safety_core_get_trip_event()'s documented return-false contract); the
// first real trip makes it 1, wrapping uint8_t thereafter -- 255 trips in one
// boot without a reboot in between is not a case this needs to handle
// specially, wrapping back to 0 just means the 256th event reports the same
// sequence number the "never tripped" state would have, which is an
// acceptable, undocumented edge this shares with any other wrapping counter
// in this codebase (e.g. link_task's own s_msg_index).
static uint8_t s_trip_seq = 0;
static safety_trip_t s_trip_reason = SAFETY_TRIP_NONE;
static uint32_t s_trip_uptime_ms = 0;
static float s_trip_tc_c = 0.0f; // meaningless while s_trip_seq == 0 -- the getter
static float s_trip_deciding_threshold = 0.0f; // never reports these until a real trip sets them

// Builds one tick's worth of safety_guard_input_t from the current live
// snapshots -- factored out of safety_core_task()'s loop so
// safety_core_request_clear_trip() below can build the exact same fresh
// input for its re-evaluation retick (safety_guards_try_clear()) instead of
// duplicating this construction.
static safety_guard_input_t safety_core_build_input(void)
{
    // Phase 3: thermo_task now owns a real MAX31856 (max31856.c, ported
    // from KilnFW) and publishes a thermo_snapshot_t every ~DRDY edge
    // (or marks itself invalid on DRDY silence -- see thermo_task.c).
    // Pulled here, never pushed -- thermo_task_get_snapshot() is a
    // mutex-guarded copy, matching ARCHITECTURE.md section 2's
    // "safety_core pulls, nothing pushes into it" rule. A snapshot that
    // was never published (thermo_task not started, or called before its
    // first read) comes back tc_valid = false / NaN via
    // thermo_task_get_snapshot()'s own documented failure contract, so
    // the fallback here needs no special-casing beyond the struct's
    // default field values.
    //
    // heat_commanded stays false -- still correctly out of scope (no
    // current sense yet, Phase 6); this is unchanged from the Phase 2
    // stub and remains the honest, conservative answer to "do you know
    // heat is happening?" until current_task exists.
    thermo_snapshot_t thermo;
    (void)thermo_task_get_snapshot(&thermo);

    // SAFETY_CMD_ANNOUNCE_REBOOT grace window: this is the ONE place that
    // does the announced-timestamp-to-now arithmetic -- safety_guards.c
    // receives only the already-computed bool, per that struct field's own
    // doc comment. reboot_announce_get() returning false ("never announced
    // this boot") correctly collapses to reboot_grace_active = false, the
    // same "unknown means not-suppressed" default every other guard input
    // in this function already uses.
    uint32_t announced_at_ms = 0;
    bool reboot_grace_active = false;
    if (reboot_announce_get(&announced_at_ms)) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        // Unsigned subtraction is deliberately safe here even across a
        // pico/time wraparound: both operands come from the same
        // to_ms_since_boot() clock, so (now_ms - announced_at_ms) wraps
        // correctly in uint32_t arithmetic regardless of which side is
        // numerically larger, identical to how link_task.c already reasons
        // about its own tick-based age calculations.
        uint32_t age_ms = now_ms - announced_at_ms;
        reboot_grace_active = age_ms < REBOOT_GRACE_WINDOW_MS;
    }

    return (safety_guard_input_t){
        .tc_valid = thermo.valid,
        .tc_c = thermo.tc_c,
        .cj_c = thermo.cj_c,
        .fault_bits = thermo.fault_bits,
        .spi_failed = thermo.spi_failed,
        .estop_pressed = discrete_task_estop_pressed(),
        .heat_commanded = false,
        .reboot_grace_active = reboot_grace_active,
        .dt_s = (float)SAFTYFW_PERIOD_SAFETY_CORE_MS / 1000.0f,
    };
}

static void safety_core_task(void *arg)
{
    (void)arg;

    safety_guards_reset(&s_guard_state);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_SAFETY_CORE_MS));

        safety_guard_input_t input = safety_core_build_input();

        bool newly_tripped = safety_guards_tick(&s_guard_state, &s_guard_cfg, &input);
        if (newly_tripped) {
            // SAFETY_MODEL.md section 6's 4-step trip order: (1) relay_owner
            // de-energizes K4 first, before anything else, before logging;
            // (2) the trip reason (and a snapshot of the deciding inputs --
            // s_guard_state already holds one) is latched.
            // relay_owner_command_trip() posts to relay_owner's queue;
            // relay_owner runs at priority 7 (this task is priority 5), so
            // FreeRTOS preempts this task to service that command before
            // execution returns to the line below -- see relay_owner.h's
            // doc comment on relay_owner_command_trip() for why that
            // ordering is real and not just hoped for.
            (void)relay_owner_command_trip(s_guard_state.reason);
            boot_reason_latch_trip((uint32_t)s_guard_state.reason);

            // Capture Frame D's "at the instant of the trip" values right
            // here, same tick, before anything else runs -- LINK_PROTOCOL.md
            // sec 6: "half a second later the temperature has changed... the
            // evidence is gone." s_trip_seq incrementing LAST is deliberate:
            // safety_core_get_trip_event() is lock-free, so a reader must
            // never be able to observe the new seq before the fields it
            // describes are already written.
            s_trip_reason = s_guard_state.reason;
            s_trip_uptime_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
            s_trip_tc_c = input.tc_valid ? input.tc_c : NAN;
            s_trip_deciding_threshold = safety_guards_deciding_threshold_c(s_guard_state.reason,
                                                                            &s_guard_cfg);
            s_trip_seq++; // wraps uint8_t -- see the variable's own doc comment

            // TODO (Phase 8): log_task has no ring/drain/transport yet
            // (task_priorities.h's own shell-only status) -- there is
            // nowhere to log this trip to yet, so step 4 of the 4-step
            // order (logging) is deliberately not implemented here rather
            // than faked with a printf that goes nowhere real.
        }

        // TODO (Phase 7): context_snapshot_t is read here too, once
        // link_task publishes one -- but only ever READ, never called into;
        // safety_core pulls, link_task never pushes (section 2's diagram).

        watchdog_task_checkin(WATCHDOG_CHECKIN_SAFETY_CORE);
    }
}

void safety_core_get_output_status(bool *out_relay_energized, bool *out_heating_enabled)
{
    if (out_relay_energized) {
        *out_relay_energized = relay_owner_is_energized();
    }
    if (out_heating_enabled) {
        *out_heating_enabled = (relay_owner_get_state() == RELAY_OWNER_STATE_ARMED);
    }
}

void safety_core_get_diag_status(safety_trip_t *out_trip_reason, bool *out_warn_active,
                                  uint8_t *out_diag_state)
{
    // s_guard_state is this task's own local static -- safe to read from any
    // task the same way relay_owner's state is (single-word/small-struct
    // reads of fields only this task's own tick ever writes; no torn-read
    // hazard worse than the volatile-read pattern relay_owner.h already
    // documents for the same reason, and a stale-by-one-tick (100ms) read is
    // immaterial for a diagnostic frame).
    bool warn_active = s_guard_state.s5_warn || s_guard_state.s12_warn;

    if (out_trip_reason) {
        *out_trip_reason = s_guard_state.is_tripped ? s_guard_state.reason : SAFETY_TRIP_NONE;
    }
    if (out_warn_active) {
        *out_warn_active = warn_active;
    }
    if (out_diag_state) {
        relay_owner_state_t relay_state = relay_owner_get_state();
        uint8_t state;
        if (s_guard_state.is_tripped) {
            state = 4; // tripped -- takes priority over everything else
        } else if (warn_active) {
            state = 3; // warn -- a guard is warning but hasn't tripped; not a
                       // relay_owner_state_t value at all (see this
                       // function's header comment), so it must be decided
                       // here rather than by mapping relay_state alone
        } else if (relay_state == RELAY_OWNER_STATE_ARMED) {
            state = 2;
        } else if (relay_state == RELAY_OWNER_STATE_GRACE) {
            state = 1;
        } else {
            state = 0; // RELAY_OWNER_STATE_INIT, or any future value --
                       // conservative default rather than guessing
        }
        *out_diag_state = state;
    }
}

bool safety_core_get_trip_event(uint8_t *out_trip_seq, safety_trip_t *out_trip_reason,
                                 uint32_t *out_uptime_ms, float *out_tc_c,
                                 float *out_deciding_threshold)
{
    // Read s_trip_seq FIRST, matching the write-order comment at the capture
    // site in safety_core_task() above -- if a trip lands between this read
    // and the field reads below, the caller sees either the old, fully
    // consistent set (this seq, these fields) or -- at worst -- reports one
    // trip event a poll cycle later than it happened. It can never observe a
    // torn mix of one trip's seq with another trip's fields, because the
    // writer always finishes writing every field before bumping the seq.
    uint8_t seq = s_trip_seq;

    if (out_trip_seq) {
        *out_trip_seq = seq;
    }
    if (seq == 0) {
        // No trip yet this boot -- every other output is meaningless
        // (matches the header comment's documented "false, everything
        // zeroed" contract) rather than whatever s_trip_* happen to hold
        // (their static-storage zero-init, in this case, but that is an
        // implementation detail the caller must not rely on).
        if (out_trip_reason) {
            *out_trip_reason = SAFETY_TRIP_NONE;
        }
        if (out_uptime_ms) {
            *out_uptime_ms = 0;
        }
        if (out_tc_c) {
            *out_tc_c = NAN;
        }
        if (out_deciding_threshold) {
            *out_deciding_threshold = NAN;
        }
        return false;
    }

    if (out_trip_reason) {
        *out_trip_reason = s_trip_reason;
    }
    if (out_uptime_ms) {
        *out_uptime_ms = s_trip_uptime_ms;
    }
    if (out_tc_c) {
        *out_tc_c = s_trip_tc_c;
    }
    if (out_deciding_threshold) {
        *out_deciding_threshold = s_trip_deciding_threshold;
    }
    return true;
}

// Explicit operator-acknowledged clear -- the only way out of a latched
// trip (SAFETY_MODEL.md section 2). Re-evaluates against a fresh input
// snapshot before honoring the clear (safety_guards_try_clear(), see its
// own doc comment for exactly what this does and does not catch). If the
// clear holds, tells relay_owner to leave TRIPPED; if refused, s_guard_state
// is already freshly re-tripped by safety_guards_try_clear() and
// relay_owner's own TRIPPED latch (set at the original trip) is untouched
// and still correct -- nothing further to do in that branch.
//
// NOT YET CALLED FROM ANYWHERE in this build -- same honesty as
// relay_owner_clear_trip()'s own header comment: the real trigger is
// Phase 7's link_task CLEAR_TRIP (0x0A) command from the ESP/GUI, which
// does not exist yet. This function is the policy/API half of that; wiring
// an actual caller is a separate, later pass.
bool safety_core_request_clear_trip(void)
{
    if (!s_guard_state.is_tripped) {
        return true;
    }

    safety_guard_input_t input = safety_core_build_input();
    bool cleared = safety_guards_try_clear(&s_guard_state, &s_guard_cfg, &input);
    if (cleared) {
        (void)relay_owner_clear_trip();
    }
    return cleared;
}

bool safety_core_start(void)
{
    BaseType_t ok = xTaskCreate(safety_core_task, "safety_core", SAFETY_CORE_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_SAFETY_CORE, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}
