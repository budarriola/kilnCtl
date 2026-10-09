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
#include "profiles_types.h"
#include "safety_link.h"
#include "thermal_guard.h"

/* HW_ABSTRACTION.md "drivers/ layering" items 4/6: profile_exec_state_t,
 * profile_exec_status_t (and the structs it embeds) and
 * profile_executor_get_status() itself now live in this narrow header so
 * that bridge/hw and safety code can query executor state without pulling
 * in this header's start/stop/pause/resume/halt command surface. */
#include "profile_executor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

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


/* Set 2 (PID_EXPANSION_PLAN.md Phase 7a-2) -- one persisted snapshot of a
 * completed (or operator-stopped) firing, keyed by profile and kept as a
 * ring of the last PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH entries per
 * profile (profile_executor_get_firing_history()). Gains are copied in at
 * finalize time specifically so a later comparison against an OLDER entry
 * in the same ring cannot silently span a re-tune -- see this struct's
 * kp/ki/kd fields. */
typedef struct {
    bool     active;               /* this zone participated in the run this record describes */
    profile_exec_firing_stats_t stats;
    float    kp, ki, kd;           /* this zone's PID gains in force at run completion -- 0/0/0 for a
                                     * zone whose control_mode wasn't PID/PID_FUZZY, or that was never
                                     * autotuned/hand-tuned (a legitimate all-zero gain set) */
} profile_firing_zone_record_t;

typedef struct {
    uint8_t  profile_id;
    char     profile_name[PROFILE_NAME_MAX_LEN + 1]; /* copied at run start, not looked up again --
                                                       * a later profile rename/delete must not
                                                       * retroactively relabel old history */
    uint32_t run_started_unix_s;   /* time(NULL) at profile_executor_run(); 0 if the clock was never
                                     * synced (SNTP never completed) -- callers must treat 0 as
                                     * "unknown," not as an epoch date */
    uint32_t duration_s;           /* this run's total_elapsed_s at finalize */
    uint8_t  zone_mask;
    profile_firing_zone_record_t zones[MAX31856_CHANNEL_COUNT];
} profile_firing_run_record_t;

#define PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH 5




#define PROFILE_EXECUTOR_HYSTERESIS_C 2.0f /* BANGBANG mode's fixed band -- see profile_executor.c */

/* TODO.md 6A.2's cooling-limited diagnostic (see
 * profile_exec_zone_status_t::cooling_limited): how far above target and
 * how long before "duty pinned at 0" is reported as "cannot follow" rather
 * than just a normal, brief overshoot settling out. Firmware-wide constants
 * for now, like PROFILE_EXECUTOR_HYSTERESIS_C above -- not yet per-zone
 * config, since nothing has asked for that granularity here. */
#define PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C 2.0f
#define PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S 60.0f

/* HP-02 bench bug (2026-09-27): "wants heat, never gets a relay" reporting
 * (profile_exec_zone_status_t::relay_starved_s / relay_denied_reason).
 * A zone whose duty reads at least PROFILE_EXECUTOR_RELAY_STARVED_DUTY while
 * its relay is not commanded on is counting starvation seconds; the reason
 * codes below name the mechanism that refused the relay on the latest tick.
 * Values are part of the JSON contract (GET /api/profile_exec, /api/control)
 * and read by the bench harness (cases_heat.py) -- append, never renumber. */
#define PROFILE_EXECUTOR_RELAY_STARVED_DUTY 0.99f
typedef enum {
    PROFILE_EXEC_RELAY_DENIED_NONE = 0,
    /* The zone is typed ZONE_TYPE_ON_OFF and the profile has no enabled
     * on/off rule for it in the current segment, so docs/ON_OFF_ZONE.md
     * sec 3 rule 6 holds the relay OFF -- regardless of what its (unused)
     * control mode's duty says. profile_executor_run() refuses a profile
     * where this would hold for EVERY segment; this reports the per-segment
     * case that refusal cannot see. */
    PROFILE_EXEC_RELAY_DENIED_ON_OFF_NO_RULE = 1,
    /* max_simultaneous_relays denied this zone the relay this tick. */
    PROFILE_EXEC_RELAY_DENIED_LOAD_CAP = 2,
    /* relay_authority blocked the zone (heat_blocked/heat_blocked_sources
     * carry the detail). */
    PROFILE_EXEC_RELAY_DENIED_AUTHORITY = 3,
    /* Aux outputs only (profile_exec_status_t.aux[].rule_reason): the aux
     * could not be evaluated safely this tick (config unreadable, or a
     * temperature rule whose thermocouple zone has no usable reading) and
     * is held at its fixed fail-safe OFF. */
    PROFILE_EXEC_RELAY_DENIED_AUX_FAILSAFE = 4,
} profile_exec_relay_denied_t;

#define PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN 0.5f

#define PROFILE_EXECUTOR_TICK_MS 1000u /* 1 Hz per TODO.md 6A.7 */


/* TODO.md 6A.5(d): "ramp_lock_band_c (default 25 C)". Not yet per-profile/
 * per-zone configurable -- a single firmware-wide default, same status as
 * PROFILE_EXECUTOR_HYSTERESIS_C above. */
#define PROFILE_EXECUTOR_RAMP_LOCK_BAND_C 25.0f

/* History ring buffer (TODO.md section 0 / 6A.9): one sample per
 * HISTORY_SAMPLE_PERIOD_S while a profile is RUNNING, oldest overwritten
 * once full -- "this firing's trend," not indefinite history, matching
 * section 0's RAM-only settled design.
 *
 * 2026-09-01 (owner: "the duty cycle and all of the zones are not always
 * visible on the web graph" / "sometimes the legends are there and the plot
 * for it is gone too"): this WAS scoped to a single representative zone (the
 * lowest-indexed active zone in the run's zone_mask) -- every other zone's
 * trace, and ALL zones' duty, only ever existed client-side in the dashboard
 * JS from the moment the page was opened, so a reload (or opening the page
 * mid-firing) mid-run lost them. Root cause: the ring recorded one zone
 * because a naive per-zone multiply of the packed 23KB buffer (up to
 * MAX31856_CHANNEL_COUNT x) would have landed on top of this board's
 * documented internal-DRAM exhaustion failure (project_esp_internal_dram_
 * exhaustion.md -- ~11.9KB free resets HTTP sockets). Fixed by moving the
 * buffer to PSRAM (8MB, effectively unused -- see s_exec.history's own doc
 * comment in profile_executor_internal.h) instead of internal DRAM: sized
 * per zone there, memory is no longer the constraint, and the move actually
 * FREES the ~23KB this buffer used to hold in .bss. Every active zone's
 * actual/duty is now recorded every sample; the shared setpoint (target_c is
 * one ramp shared across a run's zones, TODO.md 6A.5) is recorded once, not
 * duplicated per zone. Resets to empty at the start of every
 * profile_executor_run(). */
#define HISTORY_SAMPLE_PERIOD_S 30u

/* 2026-09-02, owner follow-on to the flash-logging change above (this is
 * RAM, not flash, but the same "only keep what's actually consumed" logic
 * applies): "you should maintain some points only for display on the
 * screen in ram though, but only what is nessary to display the plots."
 * SUPERSEDES the "24h at 30s/sample" sizing this constant used to carry --
 * that was a memory-pressure-driven design point from before the ring
 * moved to PSRAM (this header's own comment above, "memory is no longer
 * the constraint"); it was never actually driven by what either display
 * consumer can render.
 *
 * Surveyed both real consumers (2026-09-02):
 *   - Web graph (main_page.html's drawHistoryChart(), fed by dashboard_
 *     http.c's GET /api/history.csv, which streams the WHOLE ring with no
 *     server-side limit or decimation of its own) -- the chart canvas has
 *     no hard pixel cap in this codebase (CSS-responsive, setupChart()'s
 *     own fallback is 600 CSS px when clientWidth is unavailable), so 600
 *     points is the largest count this code already treats as "a full-
 *     width chart" anywhere, and is generously above what any point-per-
 *     pixel legibility argument needs at typical browser widths.
 *   - LCD graph (ui_page_home.c) -- UI_PAGE_HOME_CHART_POINTS == 30,
 *     reading single samples by nearest-index lookup, no batch/decimation
 *     buffer of its own.
 *   - CSV export is the SAME /api/history.csv endpoint the web graph
 *     parses -- not a separate, larger consumer.
 *
 * HISTORY_MAX_SAMPLES is therefore sized to the larger of the two (600),
 * rounded up for a little slack: 640. At HISTORY_SAMPLE_PERIOD_S == 30s
 * that is ~5h20m of continuous history -- a real reduction in HOW FAR BACK
 * the ring remembers (was 24h), which is the actual, intended tradeoff:
 * the ring is display history, not a durable record (that role now
 * belongs to event_log.h's flash events for state transitions, and to the
 * debug-UART temperature feed for anything finer-grained than that). */
#define HISTORY_MAX_SAMPLES 640u

/* The shape callers see. Storage is NOT this struct -- see
 * profile_executor.c's packed history_slot_t, which holds the same sample
 * far more densely (see that type's own doc comment in profile_executor_
 * internal.h for the current per-slot byte count). The accessors below
 * unpack on read, so this API shape is the only thing callers depend on. */
typedef struct {
    uint32_t elapsed_s;                          /* since this run started */
    float    desired_c;                          /* the shared setpoint at this sample -- one ramp
                                                    * across every zone in the run (TODO.md 6A.5) */
    float    actual_c[MAX31856_CHANNEL_COUNT];    /* per zone; NAN = zone not active in this run, or
                                                    * this sample's reading was invalid */
    float    duty[MAX31856_CHANNEL_COUNT];        /* per zone; NAN = zone not active in this run */
    uint8_t  guard[MAX31856_CHANNEL_COUNT];       /* per zone thermal_guard_trip_t; 0 =
                                                    * THERMAL_GUARD_TRIP_NONE (also the value for an
                                                    * inactive zone -- check actual_c for NAN to tell
                                                    * "inactive" from "active, no trip") */
    uint8_t  zone_mask;                           /* which zones were active in THIS sample's run
                                                    * (bit i set = zone i had real data this row --
                                                    * matches actual_c[i]/duty[i] being non-NaN).
                                                    * Captured at sample time -- see history_slot_t's
                                                    * own doc comment (profile_executor_internal.h)
                                                    * for why a caller must use THIS field rather than
                                                    * the executor's current-run zone_mask when
                                                    * averaging/masking a row read back later. */
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

/* Same shutdown as profile_executor_halt() (relays off, fail-toward-off
 * cleanup, terminal transition, run_state_note()), except the ending is
 * recorded as PROFILE_EXEC_FAULTED with `reason` copied into
 * s_exec.fault_reason, not PROFILE_EXEC_IDLE/RUN_STATE_PHASE_HALTED --
 * for a controller-detected condition (e.g. a safety_ceiling_sync config/
 * ceiling divergence) that must never be recorded the same way as a
 * deliberate operator Stop (RUN_STATE_PHASE_HALTED and RUN_STATE_PHASE_
 * FAULTED are no longer interchangeable for display purposes -- see
 * main_page.html's renderLastRun(), which now surfaces FAULTED but hides
 * HALTED). If the run is already FAULTED (e.g. a guard trip beat this call
 * to it, same tick or an earlier one), `reason` is NOT applied -- the
 * existing fault_reason is kept, same "first fault wins" rule exec_mode_
 * state_check()'s was_faulted guard uses. Only a RUNNING/PAUSED run is
 * claimed: a DONE run (finished cleanly, awaiting dismissal) is dismissed
 * as DONE by the same halt() body, never rewritten as FAULTED.
 *
 * Idempotent under repeated calls while the underlying condition persists,
 * exactly like profile_executor_halt(): the first call drives the run to
 * PROFILE_EXEC_IDLE (via the same halt() body) and writes run_state once;
 * every call after that observes PROFILE_EXEC_IDLE up front and returns
 * immediately, without a second run_state/NVS write. A no-op from
 * PROFILE_EXEC_IDLE, same as profile_executor_halt(). `reason` may be NULL
 * (leaves fault_reason as whatever a prior fault already set, or empty). */
void profile_executor_fault_halt(const char *reason);

/* Pause/resume: pause drops every active zone's relays and freezes the
 * shared ramp/dwell schedule; resume picks up exactly where it left off.
 * Both return false (no state change) if the executor isn't in a state
 * where the transition makes sense. */
bool profile_executor_pause(void);
bool profile_executor_resume(void);


/* PID_EXPANSION_PLAN.md Phase 7a-2/7a-3: copies up to
 * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH persisted run records for
 * profile_id into out, newest-first, and returns how many were actually
 * written (0 if this profile has never completed/been-stopped-from a run,
 * or on an NVS read error -- both look the same to a caller, "nothing to
 * show yet"). Reads profiles_nvs; safe to call from any task/state, does
 * not touch s_exec. */
size_t profile_executor_get_firing_history(uint8_t profile_id, profile_firing_run_record_t *out,
                                            size_t max_entries);

/* PROFILE_SLOTS_100.md section 7 task 8: thin wrapper over
 * profile_executor_get_firing_history() that returns just the newest run's
 * run_started_unix_s (0 if never fired, or fired before the board had an
 * RTC/SNTP fix -- both look like "not recent" to a caller and that is the
 * correct behavior for the web "recently fired" ordering this feeds).
 * Declared narrowly on purpose, separate from the record-returning function
 * above: profiles_catalog_http.c calls this to add one field to the existing
 * /api/profiles list JSON (no new HTTP route -- the URI handler cap has one
 * spare slot) and forward-declares it locally rather than including this
 * whole header, so it does not pull MAX31856.h/kiln_io.h/pid.h/safety_link.h/
 * thermal_guard.h into that file or into test_profiles_http.c's host-test
 * build (which does not link profile_executor.c). */
uint32_t profile_executor_last_run_started_unix_s(uint8_t profile_id);

/* True if zone_index is one of the zones the currently RUNNING/PAUSED
 * profile (if any) targets -- autotune_engine.c uses this to refuse
 * starting a step test against a zone a profile is actively driving,
 * without having to refuse autotune globally just because *some* other
 * zone has a profile running (TODO.md 6A.5's multi-zone execution makes
 * that distinction meaningful in a way it wasn't before this pass). */
bool profile_executor_zone_is_active(uint8_t zone_index);

/* Narrow sibling of profile_executor_get_status(): true with *out_id set to
 * the currently RUNNING/PAUSED profile's id, false (with *out_id set to 0)
 * when idle/faulted/done/prestart. Deliberately avoids materializing a
 * profile_exec_status_t on the caller's stack -- use this instead of
 * profile_executor_get_status() from any task with a tight stack budget
 * (e.g. uart_bridge_ext_control.c's bx_flash_worker handlers). */
bool profile_executor_get_active_id(uint8_t *out_id);

/* Spare-relay WP-6: bit (relay-1) set for every aux relay the current run has
 * taken over (s_exec.aux_claim_mask), 0 when idle or before
 * profile_executor_start(). Narrow, one brief lock, pure field read -- for
 * /api/status and the LCD, which must not materialize a profile_exec_status_t. */
uint8_t profile_executor_aux_claim_mask(void);

/* docs/LIVE_PROFILE_EDIT.md pass 2: GET /api/profile/live's one locked
 * read of the live-edit-relevant slice of s_exec -- the run's identity/
 * segment position (meaningful only while active), and the last definitive
 * pickup refusal (s_exec.live_edit_last_refusal). This is the ONLY sanctioned
 * way for the HTTP layer to see that field; it must never reach into s_exec
 * directly. Takes s_exec.lock only, briefly, mirroring every other status
 * accessor in this file -- never call this while already holding a lock. */
typedef struct {
    bool active;             /* state is RUNNING, PAUSED or FAULTED (owner
                               * decision 4: live editing is allowed in all
                               * three) */
    uint8_t profile_id;      /* meaningful only when active */
    uint8_t segment_index;   /* meaningful only when active -- plan section
                               * 10's editable_from_segment derives from this */
    bool has_refusal;
    uint32_t refusal_generation;
    int refusal_result;      /* profile_live_pickup_result_t, cast to int so
                               * this header does not have to pull in
                               * profile_executor_live_pickup.h */
    char refusal_err_msg[128];
} profile_executor_live_status_t;

void profile_executor_get_live_status(profile_executor_live_status_t *out);

/* GET /api/cfgfs dual-write picture for the firing_stats_cfg_fs.c bridge --
 * see profile_executor_firing_stats.c's definition (firing_stats_get_
 * dualwrite_status()) for the aggregation shape and its documented
 * builtin-id scope gap. */
void firing_stats_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                        bool *diverged);
/* Same, plus *nvs_stale: true if any covered id's cfg file has a strictly higher
 * rev than its legacy NVS copy and the content differs (not a divergence). */
void firing_stats_get_dualwrite_status_ex(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                           bool *diverged, bool *nvs_stale);

/* docs/PROFILE_SLOTS_100.md section 7 task 10: deleting a profile slot
 * (profiles_http.c's nvs_erase_slot()) must also prune that id's firing
 * history, or a later id reused for a new, never-fired profile would read
 * back the PREVIOUS occupant's runs the first time someone opens its
 * history page. Erases "fs_<id>"/"fsr_<id>" from profiles_nvs/fire_stats
 * and the cfg-filesystem mirror file (stats/fs<id>.dat). ERASE-FIRST: the
 * legacy NVS keys go first and are checked; on failure the cfg file is left
 * intact and the error is returned (docs/CONFIG_FILESYSTEM.md "NVS
 * dual-write closed"). Safe to call for an id that never fired (both erases
 * read back "not found", already the success case). */
esp_err_t firing_stats_erase(uint8_t profile_id);

/* Drops every entry from the last-run-started RAM cache that
 * profile_executor_last_run_started_unix_s() reads (see
 * profile_executor_firing_stats.c's "last-run-started RAM cache" section).
 * Needed by any path that destroys "fs_<id>" WITHOUT going through
 * firing_stats_erase() -- today that is factory_reset.c's wholesale
 * hal_kv_erase_partition(PROFILES_NVS_PARTITION) for the "profiles" and
 * "all" scopes. That path does schedule a reboot, but the reboot is delayed
 * (and its task creation can fail), so without this the cache would keep
 * serving pre-erase timestamps to GET /api/profiles in the meantime: the
 * exact "reset one side of a pair" bug class CLAUDE.md lists. Safe to call
 * before the cache has ever been allocated (a no-op then). */
void firing_stats_cache_invalidate_all(void);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_EXECUTOR_H
