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
// greps for "uart"/"link" in an #include line, not for these. Same for
// current_task.h (its own ADC0/1/2, independent of the link entirely -- see
// SAFETY_MODEL.md section 3) and snapshots.h, which is where link_task.c's
// context/link-liveness getters are (re-)declared specifically so this file
// can call them without a "link"-named #include -- see that header's own
// doc comment on that section, and reboot_announce.h's comment two lines up
// for the precedent this follows.
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
#include "current_task.h" // any_current_present (S3/S4/S6b) -- current_task is a real, independent
                           // Phase-6 producer (its own ADC, not link-derived), not link/uart-shaped,
                           // fine for check_isolation.ps1
#include "discrete_task.h"
#include "reboot_announce.h" // SAFETY_CMD_ANNOUNCE_REBOOT (0x18) grace-window fact for
                              // S6b -- see that header's own doc comment for why this,
                              // and not link_task.h, is the safe way to cross the
                              // link->safety_core boundary tools/check_isolation.ps1 enforces.
#include "relay_owner.h"
#include "safety_guards.h"
#include "snapshots.h" // context_snapshot_t + link_task_get_context_snapshot()/
                        // link_task_get_degraded_no_context()/link_task_link_up()/
                        // link_task_get_relay_on_continuous_ms() -- declared here, not in
                        // link_task.h, specifically so this file can see them without an
                        // #include naming "link"/"uart"; see snapshots.h's own doc comment
                        // on that section for the full reasoning, and reboot_announce.h's
                        // comment just above for the precedent this follows.
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

// SAFETY_MODEL.md section 5 rule 2: "Stale context is no context. Older than
// context_max_age_s (default 5s, i.e. 10 poll periods) and the context-
// consuming guards go inactive, not pessimistic." Not a
// safety_guard_cfg_t field (context_valid is a caller-computed bool, not a
// per-tick raw value the guard module itself ages -- see
// safety_guard_input_t's own doc comment on context_valid), so this lives
// here as a local constant, same status as REBOOT_GRACE_WINDOW_MS just
// above: a documented software default from SAFETY_MODEL.md, not an
// invented one.
#define CONTEXT_MAX_AGE_MS 5000u

// Mirrors safety_guards.c's own I_PRESENT_A_DEFAULT / CORRELATION_WINDOW_S_
// DEFAULT (safety_guards.h's cfg field doc comment: "0 -> i_present_a=2.0A,
// correlation_window_s=150.0s") -- duplicated here, not exported from
// safety_guards.c, because this file needs the SAME "0 means not
// configured, substitute the default" substitution safety_guards.c already
// does internally in order to compute any_current_present/
// relay_commanded_continuously as INPUT facts, one tick before
// safety_guards_tick() itself runs. Both numbers are the already-documented
// SAFETY_MODEL.md defaults, not new ones invented for this file -- if
// safety_guards.c's own defaults ever change, these must change with them
// (same risk any duplicated constant carries; there is no third home to put
// a single copy in without violating the isolation boundary this whole file
// exists to keep, see snapshots.h's own doc comment on why the shared pure
// helpers live there instead of in safety_guards.c or link_frame.c).
#define SAFETY_CORE_I_PRESENT_A_DEFAULT          2.0f
#define SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT 150.0f

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
    // heat_commanded stays false -- deliberately still out of scope for
    // THIS pass, not because current_task doesn't exist (it does, Phase 6 is
    // built, and any_current_present below now reads it), but because S11's
    // own wiring is a separate, unauthorised-for-this-change gap: S11 is not
    // one of the guards this pass's fix is scoped to. false remains the
    // honest, conservative answer to "do you know heat is happening?" until
    // that separate wiring lands.
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

    // --- Context from link_task (SAFETY_MODEL.md section 5) ----------------
    // Pulled here, never pushed -- same "safety_core pulls, link_task never
    // pushes into it" discipline as the thermo snapshot above. This was the
    // TODO (Phase 7) this function carried for every prior pass: context_
    // snapshot_t is now actually read, not just declared reachable.
    context_snapshot_t ctx;
    bool ctx_published = link_task_get_context_snapshot(&ctx);

    // "Stale context is no context" -- collapses never-received, stale, AND
    // a version mismatch (DEGRADED_NO_CONTEXT, SAFETY_MODEL.md section 6a:
    // "the context-dependent guards report as disabled") into the single
    // context_valid fact safety_guards.c's own doc comment expects: it does
    // not, and must not, need to re-derive any of these three conditions
    // itself.
    bool context_valid = false;
    if (ctx_published && ctx.valid && !link_task_get_degraded_no_context()) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        // Same wraparound-safe unsigned subtraction reasoning as
        // reboot_grace_active above -- both ctx.timestamp_ms and now_ms come
        // from the same to_ms_since_boot() clock.
        uint32_t age_ms = now_ms - ctx.timestamp_ms;
        context_valid = age_ms < CONTEXT_MAX_AGE_MS;
    }

    // S2/S10's zone reduction -- context_reduce_zones() (snapshots.h) is the
    // pure, host-tested function that turns the raw per-zone array into the
    // two scalars those guards actually consume; see its own doc comment for
    // why *_zone_count is a count of ELIGIBLE (active + measured-valid)
    // zones, not ctx.zone_count itself. Only meaningful (and only computed
    // against a real tc_c) when context is valid -- when it is not, zeroing
    // zone_count below is what tells safety_guards.c "no active zones this
    // tick", the same inactive treatment as context_valid == false itself
    // (safety_guards.c resets S2/S10's own accumulators either way).
    uint8_t zone_count = 0;
    float   max_zone_setpoint_c = 0.0f;
    float   nearest_zone_measured_c = 0.0f;
    if (context_valid) {
        context_reduce_zones(&ctx, thermo.tc_c, &zone_count, &max_zone_setpoint_c,
                              &nearest_zone_measured_c);
    }

    // S3/S4/S6b's current-presence fact -- current_task is a real, already-
    // running Phase 6 producer (its own ADC0/1/2, independent of the link;
    // SAFETY_MODEL.md section 3 -- "not an over/under-current guard", so this
    // is presence/absence only). current_any_present() (snapshots.h) applies
    // the SAME threshold substitution safety_guards.c's own effective_f()
    // does internally (see SAFETY_CORE_I_PRESENT_A_DEFAULT's declaration
    // comment) -- unconditional, not gated on context_valid, matching
    // safety_guards.c's own S6b block which reads any_current_present
    // regardless of context (SAFETY_MODEL.md section 2: "a quiet link with
    // current flowing" needs current sensing to work even with no ESP
    // context at all).
    current_snapshot_t current;
    current_task_get_snapshot(&current);
    float i_present_a = (s_guard_cfg.i_present_a > 0.0f) ? s_guard_cfg.i_present_a
                                                           : SAFETY_CORE_I_PRESENT_A_DEFAULT;
    bool any_current_present = current_any_present(&current, i_present_a);

    // S3/S4's relay-correlation facts (SAFETY_MODEL.md section 4) -- both
    // derived from context, so both collapse to false whenever context_valid
    // is false, matching safety_guards.c's own "reset the accumulator, don't
    // just skip the check" discipline for the whole context-dependent block.
    bool relay_commanded_recently = false;
    bool relay_commanded_continuously = false;
    if (context_valid) {
        relay_commanded_recently = ctx.relay_recent_mask != 0u;
        float correlation_window_s = (s_guard_cfg.correlation_window_s > 0.0f)
                                          ? s_guard_cfg.correlation_window_s
                                          : SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT;
        uint32_t continuous_ms = link_task_get_relay_on_continuous_ms();
        relay_commanded_continuously = (float)continuous_ms >= correlation_window_s * 1000.0f;
    }

    // S13's sample_counter_advancing: deliberately left false. SAFETY_MODEL.md
    // section 3 requires a commissioned `borrowed_zone_index` (0..2) to say
    // WHICH context zone is "the" borrowed channel before this fact means
    // anything at all -- that field is documented (docs/CONFIG_REFERENCE.md,
    // SAFETY_MODEL.md section 3) but does not exist anywhere in this
    // codebase yet (grep-confirmed: no config_store field, no
    // safety_guard_cfg_t field), the same "Phase 9, not commissioned" gap
    // abs_max_temp_c is in for S1. Guessing a zone index (e.g. hardcoding
    // zone 0) would be inventing a commissioning decision this pass is not
    // authorised to make, and it would be silently WRONG the moment a real
    // installation's borrowed zone is not zone 0. This is harmless today
    // regardless: cfg.tc_source has no default and stays SAFETY_TC_SOURCE_
    // OWN_J7 (0) absent Phase 9 commissioning, and safety_guards.c's own S13
    // block is gated on `cfg->tc_source == BORROWED_ZONE || BOTH` before it
    // ever reads this field -- so S13 stays correctly dormant either way,
    // the same "config gap, not a missing-producer gap" category
    // GUARD_TEST_MATRIX.md section 6 already documents for S1. Revisit once
    // borrowed_zone_index is real.
    bool sample_counter_advancing = false;

    return (safety_guard_input_t){
        .tc_valid = thermo.valid,
        .tc_c = thermo.tc_c,
        .cj_c = thermo.cj_c,
        .fault_bits = thermo.fault_bits,
        .spi_failed = thermo.spi_failed,
        .estop_pressed = discrete_task_estop_pressed(),
        .heat_commanded = false,
        .context_valid = context_valid,
        .zone_count = zone_count,
        .max_zone_setpoint_c = max_zone_setpoint_c,
        .nearest_zone_measured_c = nearest_zone_measured_c,
        .any_current_present = any_current_present,
        .relay_commanded_recently = relay_commanded_recently,
        .relay_commanded_continuously = relay_commanded_continuously,
        .sample_counter_advancing = sample_counter_advancing,
        .link_up = link_task_link_up(),
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

bool safety_core_request_enable(bool enable)
{
    // See safety_core.h's doc comment: deliberately a thin forward, no
    // second policy layer. relay_owner_command_energize() already refuses
    // while TRIPPED, accepts-but-never-applies during GRACE, and only
    // actually drives GPIO6 high while ARMED.
    return relay_owner_command_energize(enable);
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
