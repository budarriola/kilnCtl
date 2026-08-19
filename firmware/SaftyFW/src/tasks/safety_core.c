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

#include "task_priorities.h"
#include "watchdog_task.h"

#include "boot_reason.h"
#include "discrete_task.h"
#include "relay_owner.h"
#include "safety_guards.h"
#include "thermo_task.h"

#define SAFETY_CORE_STACK_WORDS   configMINIMAL_STACK_SIZE

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

    return (safety_guard_input_t){
        .tc_valid = thermo.valid,
        .tc_c = thermo.tc_c,
        .cj_c = thermo.cj_c,
        .fault_bits = thermo.fault_bits,
        .spi_failed = thermo.spi_failed,
        .estop_pressed = discrete_task_estop_pressed(),
        .heat_commanded = false,
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
