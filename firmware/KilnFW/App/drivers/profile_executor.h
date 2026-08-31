// profile_executor -- TODO.md section 6 + 6A: the task that drives a
// profile's ramp/dwell schedule across the zone(s) it targets, through one
// of three control modes per zone (zones_http.h's zone_control_mode_t),
// with the full thermal-protection guard suite (thermal_guard.h) armed on
// every active zone.
//
// Module layout (TODO.md 6A.7): this is the only module in the set that
// talks to FreeRTOS, kiln_io, relay_authority, or the HTTP layer. The
// control math (pid.h), the safety guards (thermal_guard.h), and the
// duty-to-relay rendering (heater_output.h) are all pure and live in their
// own files specifically so they can be tested without this one.
//
// TODO.md 6A.5: a profile can drive more than one zone concurrently,
// sharing a single ramp/dwell schedule -- profile_t.zone_mask, not a single
// zone_index. Each active zone runs its own independent PID/guard/relay
// ("detuned decentralized PID", 6A.5(a)) against one shared setpoint, and
// that shared setpoint's ramp only advances while every active,
// not-already-per-zone-faulted zone is within PROFILE_EXECUTOR_RAMP_LOCK_BAND_C
// of it -- "ramp-lock" / setpoint governor, 6A.5(d), "the slowest zone sets
// the pace". A per-zone guard trip (guards 1/2/4/7) drops only that zone and
// the run continues for the rest; a global guard trip (guards 3/5/6) drops
// every zone and faults the whole run, same escalation split as before this
// pass, just now applied per-zone within one run instead of being the whole
// run's only zone.
//
// TODO.md 6A.3, "No auto-resume across reboot": this module writes a small
// fixed-size breadcrumb to NVS (run_state.h) on every meaningful transition
// -- run start, segment change, pause/resume, fault, done, operator halt --
// plus a slow periodic refresh while running. profile_executor_start() loads
// the previous boot's record and logs it, loudly at WARN if that firing was
// never cleanly ended. It is loaded, logged, and served over HTTP, and that
// is all: no path exists from a stored record back into
// profile_executor_run(), and the executor always comes up IDLE with relays
// off (kiln_io_init()'s latch ordering). A firing must never restart itself
// -- see run_state.h for why "resume where it left off" is wrong even when
// it would be safe.
//
// Scope of what's built in this pass vs. TODO.md 6A's full plan:
//   - Built: multi-zone concurrent execution sharing one ramp/dwell
//     schedule, ramp-lock (6A.5(d)), per-zone guard escalation within a
//     multi-zone run, all three control modes per zone, a second watchdog
//     task for guard 9, calibration applied to the control-facing reading.
//   - Built elsewhere since this pass: the coupling-matrix capture itself
//     (6A.5(b)) landed 2026-08-11 in autotune_engine.c
//     (autotune_engine_get_coupling_matrix(), served as GET
//     /api/autotune/matrix), so guard 8's threshold design is no longer
//     blocked on a missing matrix -- only on nobody having written the
//     guard yet.
//   - NOT built (see TODO.md 6A for the full list): guard 8 (cross-zone
//     plausibility), the RGA display (6A.5(c)), the static decoupler
//     (6A.5(e), explicitly gated on the RGA saying it's warranted),
//     electrical load staggering, and PID autotune's own multi-zone
//     awareness (autotune still runs one zone at a time, unaffected by this
//     pass -- see autotune_engine.h).
//
// HARDWARE STATUS AS WRITTEN: built and logic-verified against the absent-
// hardware code paths (io/thermo_bus NULL, matching every other driver's
// non-fatal convention) but NOT verified against a real relay board or
// thermocouple daughterboard, and specifically NOT verified against a real
// *multi-zone* firing (no way to attach more than the absent hardware at
// all right now) -- neither a relay board nor a thermocouple daughterboard
// is attached to this bench unit as of this writing. Treat every
// control/guard/ramp-lock behavior here as unverified until it has actually
// driven real relays against real readings across more than one zone at
// once. See docs/PROJECT_STATUS.md.
#ifndef PROFILE_EXECUTOR_H
#define PROFILE_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "pid.h"
#include "profiles_http.h"
#include "safety_link.h"
#include "thermal_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROFILE_EXEC_IDLE = 0,   /* nothing running */
    PROFILE_EXEC_RUNNING,
    PROFILE_EXEC_PAUSED,
    PROFILE_EXEC_DONE,       /* ran to completion; relays off; call profile_executor_run() to go again */
    PROFILE_EXEC_FAULTED,    /* a GLOBAL thermal guard tripped (or every active zone individually
                              * faulted -- nothing left to run); relays off across the whole run;
                              * stays here until profile_executor_halt() explicitly acknowledges it
                              * (latching, TODO.md 6A.3) */
} profile_exec_state_t;

/* ---- Guard 9's pure decision core (ROADMAP.md "safety processor faults
 * should stop firing" pass) -------------------------------------------------
 * Pulled out of profile_executor.c's watchdog_task_entry() into a pure,
 * dependency-free `static inline` function so it can be host-tested without
 * pulling in FreeRTOS/kiln_io/relay_authority/etc -- the same "pure logic
 * lives in its own file so it can be tested without this one" split this
 * header's own top comment already describes for pid.h/thermal_guard.h,
 * applied to the one piece of watchdog_task_entry() that is itself pure
 * (everything else in that task is I/O: the safety_link_get_status() call,
 * s_exec.lock, kiln_io_all_relays_off(), run_state_note()). All of that I/O
 * stays exactly where it was, entirely inside watchdog_task_entry() -- this
 * function only classifies "given what the caller already learned this
 * tick, what should happen," and touches nothing itself. Called from inside
 * s_exec.lock, same as the code it replaces; it does no locking or I/O of
 * its own so that's still the caller's discipline to keep, not this
 * function's job to enforce. */
/* How long SAFETY_FAULT_SRC_PC_LINK must read continuously asserted before a
 * RUNNING/PAUSED firing is aborted (profile_executor_wd_decide()'s
 * pc_link_down_sustained). Deliberately its OWN threshold, not a reuse of
 * either neighboring constant:
 *
 *   - NOT UART_BRIDGE_LINK_TIMEOUT_MS (5000ms, uart_bridge.h): that is when
 *     relays get force-dropped, and a brief drop-and-recover there (a USB
 *     re-enumeration, a driver hiccup, a cable wiggle) is exactly the kind
 *     of blip a firing must survive without aborting -- "relays came back
 *     off for 5 seconds" is not "the operator's link is gone." Aborting a
 *     multi-hour firing on a 5-second hiccup would be worse than the bug
 *     this constant exists to fix.
 *   - Matches SAFETY_LINK_FIRING_ABORT_SILENCE_MS (30000ms, safety_link.h)
 *     instead, and for the identical reason that constant's own doc comment
 *     gives (LINK_PROTOCOL.md sec 8 / ROADMAP.md M6): "a single dropped
 *     frame must not abort a twelve-hour firing," but by 30s of continuous
 *     outage the relays have been sitting force-off the whole time (5s to
 *     drop them, then 25s more with them held off) and the run is no longer
 *     doing anything the display claims it is. Giving the two independent
 *     link failures -- safety-link silence and PC-link loss -- the same
 *     abort horizon means an operator learns one number for "how long can a
 *     comms outage run before the firing itself is declared over," instead
 *     of two link-specific ones to remember.
 *
 * The relay-drop and the run-abort intentionally do NOT share a timeout: a
 * brief drop that recovers inside this window is survivable (relays resume
 * normal control the moment the PC link watchdog clears the fault source);
 * a sustained one past it is not, and must stop pretending. */
#define PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS 30000u

typedef enum {
    PROFILE_EXECUTOR_WD_ACTION_NONE = 0,            /* nothing to do this tick */
    PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF,    /* already FAULTED; keep retrying the relay-off write, no new fault */
    PROFILE_EXECUTOR_WD_ACTION_FAULT,               /* RUNNING/PAUSED -> FAULTED: relays off once, latch fault_reason */
    PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP,       /* trip reported with no run to abort -- log only, no state change */
} profile_executor_wd_action_t;

typedef struct {
    bool     tick_stale;               /* control task's own liveness tick is too old (guard 9 proper) */
    uint32_t tick_stale_ms;            /* how long, for the fault_reason text; meaningful only if tick_stale */
    bool     safety_processor_tripped; /* diag_state == TRIPPED off a FRESH (non-stale) DIAG frame --
                                         * the caller is responsible for the staleness gate (against
                                         * SAFETY_LINK_STALE_MS) before setting this, same as the
                                         * caller already gates safety_link_silent_30s below against
                                         * its own, much larger threshold. */
    uint8_t  safety_trip_reason;       /* SAFETY_TRIP_* (SaftyFW's safety_guards.h); meaningful only if
                                         * safety_processor_tripped */
    bool     safety_link_silent_30s;   /* link silent >= SAFETY_LINK_FIRING_ABORT_SILENCE_MS */
    bool     pc_link_down_sustained;   /* SAFETY_FAULT_SRC_PC_LINK has read asserted continuously
                                         * for >= PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS -- the
                                         * caller owns timing this (the source is a level, not an
                                         * event, so there is no single "age" to compare like the
                                         * safety-link case above); see that constant's doc comment
                                         * for why this is a separate threshold from both
                                         * SAFETY_LINK_FIRING_ABORT_SILENCE_MS and
                                         * UART_BRIDGE_LINK_TIMEOUT_MS. */
    bool     state_running_or_paused;  /* s_exec.state == PROFILE_EXEC_RUNNING || PROFILE_EXEC_PAUSED */
    bool     state_faulted;            /* s_exec.state == PROFILE_EXEC_FAULTED */
} profile_executor_wd_input_t;

typedef struct {
    profile_executor_wd_action_t action;
    char fault_reason[96];  /* only meaningful when action == PROFILE_EXECUTOR_WD_ACTION_FAULT --
                              * same size as profile_exec_status_t.fault_reason below */
} profile_executor_wd_result_t;

/* Mirrors SaftyFW's safety_guards.h SAFETY_TRIP_* enum, in words -- KilnFW
 * cannot #include that header (a separate build/repository). This is the
 * one shared copy; main_page.html's SAFETY_TRIP_WORDS and ui_page_home.c's
 * safety_trip_words_short() are independent, differently-sized copies for
 * their own surfaces (a 96-byte fault_reason has room for the full sentence
 * this table returns; the LCD's single, unwrappable summary line does not).
 * Keep all three in sync if safety_guards.h's enum changes. */
static inline const char *profile_executor_safety_trip_words(uint8_t reason)
{
    switch (reason) {
    case 0:  return "none";
    case 1:  return "chamber over absolute temperature limit (S1)";
    case 2:  return "chamber over setpoint for too long (S2)";
    case 3:  return "relay/contactor stuck on, no heat commanded (S3)";
    case 5:  return "safety thermocouple reading invalid (S5)";
    case 6:  return "main controller (ESP) reported a fault (S6a)";
    case 7:  return "safety link to main controller went silent (S6b)";
    case 8:  return "E-stop pressed (S7)";
    case 9:  return "rising faster than physically possible (S8)";
    case 10: return "contactor welded on, trip did NOT cut power (S9)";
    case 12: return "safety sensor reading frozen, not updating (S11)";
    case 13: return "enclosure/cold-junction over-temperature (S12)";
    case 14: return "borrowed zone's TC stopped updating (S13)";
    case 15: return "safety config corrupt";
    case 16: return "safety self-test failed";
    default: return "unknown guard";
    }
}

static inline profile_executor_wd_result_t profile_executor_wd_decide(const profile_executor_wd_input_t *in)
{
    profile_executor_wd_result_t out;
    memset(&out, 0, sizeof(out));
    out.action = PROFILE_EXECUTOR_WD_ACTION_NONE;

    if (in->tick_stale) {
        /* Guard 9 proper: the caller forces relays off unconditionally for
         * this case regardless of what this function returns (see this
         * header's comment above) -- this only decides whether a RUNNING/
         * PAUSED run also latches FAULTED over it. */
        if (in->state_running_or_paused) {
            out.action = PROFILE_EXECUTOR_WD_ACTION_FAULT;
            snprintf(out.fault_reason, sizeof(out.fault_reason),
                     "control task tick stale for %lums", (unsigned long)in->tick_stale_ms);
        }
        return out;
    }

    if (in->safety_processor_tripped && in->state_running_or_paused) {
        /* The gap this whole pass closes: a live safety-processor trip is a
         * GLOBAL abort, same as any other global guard here -- fail closed,
         * and name the actual guard in words, distinct from the silent-link
         * wording below on purpose (a guard firing and a dead peer are
         * different failures an operator needs to tell apart). */
        out.action = PROFILE_EXECUTOR_WD_ACTION_FAULT;
        snprintf(out.fault_reason, sizeof(out.fault_reason),
                 "safety processor tripped: %s, firing aborted",
                 profile_executor_safety_trip_words(in->safety_trip_reason));
        return out;
    }

    if (in->safety_processor_tripped && in->state_faulted) {
        /* Already faulted (this cause or another) but the Pico still
         * reports the trip latched -- keep retrying the relay-off write,
         * "dropped and retried until the write succeeds" (LINK_PROTOCOL.md
         * sec 8), without touching fault_reason so whichever cause faulted
         * the run first keeps its own wording. */
        out.action = PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF;
        return out;
    }

    if (in->safety_link_silent_30s && in->state_running_or_paused) {
        out.action = PROFILE_EXECUTOR_WD_ACTION_FAULT;
        snprintf(out.fault_reason, sizeof(out.fault_reason),
                 "safety processor link silent for >=%lums, firing aborted",
                 (unsigned long)SAFETY_LINK_FIRING_ABORT_SILENCE_MS);
        return out;
    }

    if (in->safety_link_silent_30s && in->state_faulted) {
        out.action = PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF;
        return out;
    }

    if (in->pc_link_down_sustained && in->state_running_or_paused) {
        /* uart_bridge.c's link_watchdog_task already force-dropped every
         * relay 5s (UART_BRIDGE_LINK_TIMEOUT_MS) into this outage, and has
         * been re-dropping them every 250ms since -- but it has no notion of
         * "a firing is running" and never touches this module's state. Left
         * alone, a firing stays RUNNING and keeps advancing its ramp/dwell
         * schedule while the elements are actually forced off and the kiln
         * cools, and both GUIs keep showing progress on a firing that ISN'T
         * one any more. Worded distinctly from the safety-link-silent case
         * above (a dead PC and a dead safety processor are different
         * failures an operator needs to tell apart) and from the
         * safety-trip wording further above. */
        out.action = PROFILE_EXECUTOR_WD_ACTION_FAULT;
        snprintf(out.fault_reason, sizeof(out.fault_reason),
                 "PC control link silent for >=%lums, firing aborted",
                 (unsigned long)PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS);
        return out;
    }

    if (in->pc_link_down_sustained && in->state_faulted) {
        out.action = PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF;
        return out;
    }

    if (in->safety_processor_tripped) {
        /* Tripped with nothing RUNNING/PAUSED/FAULTED (IDLE or DONE) -- must
         * NOT fabricate a FAULTED run out of nothing: there is no schedule
         * to stop advancing and no run-specific relay state to drop
         * (kiln_io_init() already brings relays up off, and relay_authority
         * independently refuses new relay-on while the Pico reports a trip,
         * same as it already does for the unhealthy-link case). Log only. */
        out.action = PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP;
    }

    return out;
}

/* Per-zone status within the current (or last) run. Only zones[i] with
 * .active == true participated in this run -- the rest are zeroed. */
typedef struct {
    bool     active;
    float    actual_c;        /* calibration-corrected; meaningless if !actual_valid */
    bool     actual_valid;
    bool     relay_commanded_on;
    float    duty;
    uint8_t  control_mode;    /* zone_control_mode_t */
    bool     faulted;         /* this zone's own per-zone guard tripped (guards 1/2/4/7) --
                               * the run continues for other active zones unless every
                               * active zone ends up faulted, see profile_exec_state_t. A
                               * GLOBAL trip (guards 3/5/6) instead sets the whole run to
                               * PROFILE_EXEC_FAULTED and every zone shows faulted here. */
    char     fault_reason[96];
    uint8_t  fault_guard;     /* thermal_guard_trip_t, only meaningful when faulted */
    /* Control wanted heat this tick and relay_authority refused it. NOT a
     * fault: the run keeps going and every other field looks healthy, which
     * is exactly why this has to be reported. A firing blocked from its first
     * tick otherwise shows state "running", a climbing target, duty 0.0 and
     * faulted false, with nothing anywhere naming the reason. `sources` is a
     * bitwise OR of safety_fault_source_t (safety_link.h). */
    bool     heat_blocked;
    uint32_t heat_blocked_sources;
    /* TODO.md 6A.9's "/api/control... PID term breakdown", per zone.
     * Only meaningful when control_mode == ZONE_CONTROL_MODE_PID. */
    float    pid_p;
    float    pid_i;
    float    pid_d;
    /* TODO.md 6A.2's model feedforward, (T_sp - T_ambient)/K_dc +
     * (dT_sp/dt)*tau/K_dc, clamped to [0,1]. Exactly 0 for a zone with no
     * autotune-identified plant model, which is how a never-tuned zone keeps
     * running on feedback alone. Reading it against pid_i is the point of
     * publishing the split: a healthy tuned zone should show most of its duty
     * under ff with i small and steady, and a large i sitting under a
     * non-zero ff means the model is wrong, not that the loop is. */
    float    pid_ff;
    /* TODO.md 6A.2's "Output clamp [0,1]... the controller should report
     * 'cannot follow, cooling-limited' rather than sit at u=0 looking
     * healthy while the actual curve diverges." True once this zone's PID
     * output has sat at 0 (nothing to give -- there is no active cooling)
     * for PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S while still reading more
     * than PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C above the ramp target
     * -- i.e. the kiln is losing heat slower than the profile asked it to,
     * and nothing the loop can command will close that gap. Only
     * meaningful when control_mode == ZONE_CONTROL_MODE_PID; always false
     * for BANGBANG (no continuous u=0 to observe the same way) and OFF. */
    bool     cooling_limited;
} profile_exec_zone_status_t;

typedef struct {
    profile_exec_state_t state;
    uint8_t  profile_id;
    char     profile_name[PROFILE_NAME_MAX_LEN + 1];
    uint8_t  zone_mask;        /* which zones this run targets (profile_t.zone_mask) */
    uint8_t  segment_index;    /* 0-based; valid when state is RUNNING/PAUSED/DONE/FAULTED */
    uint8_t  segment_count;
    bool     dwelling;         /* false = ramping toward target_c, true = holding it */
    float    target_c;         /* current shared commanded target (interpolated during a ramp) */
    uint32_t segment_elapsed_s; /* only advances while ramp-lock is satisfied -- see this
                                 * header's top comment */
    uint32_t dwell_remaining_s; /* only meaningful while dwelling */
    bool     ramp_lock_held;    /* true if the shared setpoint is NOT advancing this tick
                                 * because at least one active, non-faulted zone is outside
                                 * PROFILE_EXECUTOR_RAMP_LOCK_BAND_C of target_c */
    uint8_t  ramp_lock_lagging_mask; /* which zone(s) are the reason, if ramp_lock_held */
    profile_exec_zone_status_t zones[MAX31856_CHANNEL_COUNT];

    /* Duration-model inputs for /api/profile_exec's total_planned_s/
     * elapsed_s/remaining_s and for /api/profile_plan's polyline against
     * the profile that is actually running (dashboard_http.c does the
     * arithmetic; this struct just hands out the two things it cannot get
     * any other way -- the segment data as run, and the temperature
     * segment 0's ramp actually started from).
     *
     * segments/segment_count: a COPY of the profile this run is executing
     * (s_exec.profile can differ from whatever profiles_http.c holds for
     * this id right now -- profile_executor_run() takes its own copy at
     * start, same reasoning as s_exec_state_t.profile in the .c file), so a
     * caller computing this run's planned duration is guaranteed to match
     * the schedule actually driving relays, not a schedule since edited
     * under it. */
    profile_segment_t segments[PROFILE_MAX_SEGMENTS];

    /* The temperature segment 0's ramp treated as its starting point --
     * profile_executor_run()'s baseline_target_c, captured once from the
     * first active zone's actual reading at firing start (or that segment's
     * own target if no reading was available yet). Fixed for the life of
     * the run so a caller's total-planned-seconds answer does not drift
     * tick to tick as target_c itself moves through the ramp -- only
     * meaningful when state != PROFILE_EXEC_IDLE. */
    float    run_start_c;

    /* Real seconds since this run started (profile_executor_run()), NOT
     * reset at a segment boundary -- unlike segment_elapsed_s above, this
     * only freezes across a PAUSE (the control task simply doesn't tick it
     * while PROFILE_EXEC_PAUSED, same discipline as segment_elapsed_s) and
     * otherwise keeps counting even while ramp-lock holds the shared
     * setpoint, because wall-clock time is genuinely passing then -- ramp-
     * lock is exactly the situation /api/profile_exec's
     * remaining_is_estimate exists to flag, not a reason to stop the clock.
     * This is "elapsed_s" in the API contract. */
    uint32_t total_elapsed_s;

    char     fault_reason[96];   /* only meaningful when state == PROFILE_EXEC_FAULTED (a GLOBAL trip) */
    uint8_t  fault_guard;        /* thermal_guard_trip_t, only meaningful when state == PROFILE_EXEC_FAULTED */

    /* Warm-start (PROFILES.md "Warm-start: joining a profile already at
     * temperature", owner request 2026-08-30): profile_executor_run() found
     * the kiln already hotter than one or more of the profile's opening
     * segments and entered the schedule partway through instead of
     * commanding a setpoint below the actual temperature. Set once at
     * profile_executor_run() and unchanged for the life of the run --
     * segment_index/dwelling/target_c above already reflect the entry point
     * this decided on, this just says WHY (Q6: "the operator must see it").
     * false for every run that started at segment 0 normally, including a
     * cold-kiln run of a profile the kiln happens to already satisfy at
     * segment 0 (see profile_executor.c's profile_executor_plan_warm_start()
     * doc comment for why that case is deliberately not "warm started"). */
    bool     warm_started;
    char     warm_start_reason[128]; /* only meaningful when warm_started; human-readable,
                                       * e.g. "starting at segment 4 -- kiln already at 312.0 C" */
    /* Q1's decided replay: which segment indices (0-based) had their
     * RELAY_IO on/off command reapplied before the first ramp tick because
     * warm-start skipped past them, in the order they were replayed (profile
     * order). Only the first warm_start_replayed_count entries are valid. */
    uint8_t  warm_start_replayed_segments[PROFILE_MAX_SEGMENTS];
    uint8_t  warm_start_replayed_count;
} profile_exec_status_t;

#define PROFILE_EXECUTOR_HYSTERESIS_C 2.0f /* BANGBANG mode's fixed band -- see profile_executor.c */

/* TODO.md 6A.2's cooling-limited diagnostic (see
 * profile_exec_zone_status_t::cooling_limited): how far above target and
 * how long before "duty pinned at 0" is reported as "cannot follow" rather
 * than just a normal, brief overshoot settling out. Firmware-wide constants
 * for now, like PROFILE_EXECUTOR_HYSTERESIS_C above -- not yet per-zone
 * config, since nothing has asked for that granularity here. */
#define PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C 2.0f
#define PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S 60.0f

#define PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN 0.5f

#define PROFILE_EXECUTOR_TICK_MS 1000u /* 1 Hz per TODO.md 6A.7 */


/* TODO.md 6A.5(d): "ramp_lock_band_c (default 25 C)". Not yet per-profile/
 * per-zone configurable -- a single firmware-wide default, same status as
 * PROFILE_EXECUTOR_HYSTERESIS_C above. */
#define PROFILE_EXECUTOR_RAMP_LOCK_BAND_C 25.0f

/* History ring buffer (TODO.md section 0 / 6A.9): one sample per
 * HISTORY_SAMPLE_PERIOD_S while a profile is RUNNING, oldest overwritten
 * once full -- "this firing's trend," not indefinite history, matching
 * section 0's RAM-only settled design. Scoped to a single representative
 * zone (the lowest-indexed active zone in the run's zone_mask) even though
 * multi-zone concurrent execution now exists -- true per-zone history would
 * multiply this buffer's 23KB (was ~58KB before the 2026-08-12 packing --
 * see profile_history_entry_t) by up to MAX31856_CHANNEL_COUNT, and 6A.9
 * never actually asked for per-zone history, only for the buffer to gain a
 * duty field (done). Revisit if a real multi-zone firing shows the
 * single-series view isn't enough. Resets to empty at the start of every
 * profile_executor_run(). */
#define HISTORY_SAMPLE_PERIOD_S 30u
#define HISTORY_MAX_SAMPLES 2880u /* 24h at 30s/sample, per section 0's settled sizing */

/* The shape callers see. Storage is NOT this struct -- see
 * profile_executor.c's packed history_slot_t, which holds the same sample in
 * 8 bytes instead of 20. That packing is not premature: at 2880 samples this
 * struct cost 57.6 KB of .bss, and on 2026-08-12 the board was found booting
 * with 7 KB of free heap, unable to start tasks or serve HTTP, precisely
 * because this buffer and the autotune trace had eaten the DRAM before Wi-Fi
 * came up. The accessors below unpack on read, so this API is unchanged. */
typedef struct {
    uint32_t elapsed_s;   /* since this run started */
    float    actual_c;
    float    desired_c;
    float    duty;
    uint8_t  guard;       /* thermal_guard_trip_t; 0 = THERMAL_GUARD_TRIP_NONE */
} profile_history_entry_t;

/* Total number of valid history samples right now (0..HISTORY_MAX_SAMPLES),
 * for a caller that wants to page through profile_executor_get_history()
 * without allocating a HISTORY_MAX_SAMPLES-sized buffer up front. */
size_t profile_executor_get_history_count(void);

/* Copies up to max_entries of this run's history into out, starting at
 * chronological index start_index (0 = oldest), oldest-to-newest. Returns
 * the number actually written (less than max_entries at the tail of the
 * buffer). Safe to call from any state (an IDLE/DONE/FAULTED executor still
 * has the last run's buffer until the next profile_executor_run() clears
 * it). Paged rather than all-at-once so a caller streaming this out (e.g.
 * a CSV HTTP response) doesn't need a HISTORY_MAX_SAMPLES-sized buffer of
 * its own -- at 2880 entries that's real memory, worth not doubling up. */
size_t profile_executor_get_history(profile_history_entry_t *out, size_t start_index, size_t max_entries);

/* Brings the module up and starts its control task plus a second,
 * independent watchdog task (guard 9 -- "a control loop cannot be its own
 * watchdog", TODO.md 6A.3). Matches every other driver's
 * *_start(..., io_or_null, ...) non-fatal-bring-up convention. Safe to call
 * with io_or_null/thermo_bus_or_null/safety_or_null NULL -- the executor
 * still runs its state machine (ramp/dwell timing, the dashboard's status
 * view) but withholds heat and skips the guards it can't evaluate without
 * a reading, per the "no valid data -> the safe default is never command
 * heat" rule used throughout this codebase. */
esp_err_t profile_executor_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                  SafetyLinkClass *safety_or_null);

/* Begins running profile_id from its first segment across every zone in its
 * zone_mask, each using that zone's currently configured control mode
 * (zones_config_get_control_mode()). Re-runs the TODO.md section 5
 * feasibility check against every participating zone's *current* max-ramp
 * ceiling. Refuses if already RUNNING/PAUSED (call profile_executor_halt()
 * first), or if any targeted zone's guard state is still FAULTED from a
 * previous run (same reason -- halt() first), or if an autotune run is
 * active on any zone this profile targets (TODO.md 6A.4/6A.5's "zones must
 * be autotuned one at a time" ordering constraint, enforced from both
 * directions -- see autotune_engine.h). */
bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap);

/* Stops whatever is running/paused/faulted, drops every active zone's
 * relays (fail toward off), clears any fault this run asserted (global
 * safety_link source and/or each zone's relay_authority block), and resets
 * every active zone's thermal guard latch -- the one explicit operator
 * acknowledgement TODO.md 6A.3 requires to leave a tripped state. A no-op
 * from PROFILE_EXEC_IDLE. */
void profile_executor_halt(void);

/* Pause/resume: pause drops every active zone's relays and freezes the
 * shared ramp/dwell schedule; resume picks up exactly where it left off.
 * Both return false (no state change) if the executor isn't in a state
 * where the transition makes sense. */
bool profile_executor_pause(void);
bool profile_executor_resume(void);

/* Snapshot for the dashboard's status API -- never blocks on the executor
 * task. */
void profile_executor_get_status(profile_exec_status_t *out);

/* True if zone_index is one of the zones the currently RUNNING/PAUSED
 * profile (if any) targets -- autotune_engine.c uses this to refuse
 * starting a step test against a zone a profile is actively driving,
 * without having to refuse autotune globally just because *some* other
 * zone has a profile running (TODO.md 6A.5's multi-zone execution makes
 * that distinction meaningful in a way it wasn't before this pass). */
bool profile_executor_zone_is_active(uint8_t zone_index);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_EXECUTOR_H
