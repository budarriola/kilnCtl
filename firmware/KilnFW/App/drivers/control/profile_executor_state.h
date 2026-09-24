// profile_executor_state.h -- narrow, control-layer "executor state" query
// surface split out of profile_executor.h (HW_ABSTRACTION.md "drivers/
// layering" items 4 and 6). Bridge/hw code (gpio_probe.c, boot_button.c) and
// safety code (safety_link_frames.c, danger_mode.c) only ever need to ASK
// "what is the executor doing right now" -- profile_exec_state_t/
// profile_exec_status_t and the one accessor that fills one in,
// profile_executor_get_status(). They must never see profile_executor.h's
// start/stop/pause/resume/halt command surface, so that surface stays out of
// this header entirely. profile_executor.h #includes this header so nothing
// about the type/function definitions themselves changes -- their real
// definitions still live in profile_executor.c/profile_executor_status.c,
// this only relocates the declarations a pure state query needs.
#ifndef PROFILE_EXECUTOR_STATE_H
#define PROFILE_EXECUTOR_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "MAX31856.h"
#include "kiln_io.h"
#include "pid.h"
#include "profiles_types.h"
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

/* PID_EXPANSION_PLAN.md Phase 7a: per-zone tracking-quality accumulator for
 * one profile run -- "how well did this firing follow its own ramp/dwell
 * schedule," comparable across profiles of different length and setpoint
 * span. Accumulated tick-by-tick in the executor (profile_executor.c's
 * control task -- NOT the UI, which polls at an unreliable rate and would
 * alias the integral), against the EXECUTOR's current ramped setpoint
 * (s_exec_state_t.target_c), never the segment's end temperature -- scoring
 * every ramp as a huge error by construction is exactly the bug this avoids.
 *
 * Embedded directly in profile_exec_zone_status_t below so the dashboard can
 * read live, still-accumulating figures while RUNNING, not just the final
 * per-run record persisted at completion (profile_firing_run_record_t
 * further down). The two share this same field layout on purpose -- the
 * live view and the historical record are the same measurement at different
 * points in its life.
 *
 * DEFINED BEHAVIOR for the edge cases Phase 7a calls out:
 *   - Samples where actual_valid == false are EXCLUDED, never treated as
 *     zero error -- counted instead in excluded_sample_count, so a run with
 *     heavy sensor dropout is recognisable as untrustworthy rather than
 *     quietly scoring well.
 *   - Zone-not-yet-at-temperature at run start: NOT specially excluded.
 *     Accumulation starts on this zone's very first RUNNING tick. A cold
 *     zone chasing a ramp from a large initial gap IS real ramp error --
 *     excluding it would make a firing that started far from its target
 *     look better than it tracked. (A zone whose reading is outright
 *     invalid at start falls under the actual_valid exclusion above, not
 *     this one.)
 *   - Ramp vs dwell classification uses the shared executor's `dwelling`
 *     flag as of this tick. A RELAY_IO segment leaves target_c/dwelling
 *     frozen at whatever the last ZONE_RAMP segment left them (see
 *     profile_executor.c's segment-stepping block) -- ticks spent in such a
 *     segment are attributed to whichever bucket was current when it began,
 *     a deliberate simplification since a RELAY_IO segment does not command
 *     a temperature at all.
 *   - Normalized IAE's setpoint_span_c divides into duration_s*span_c; a
 *     profile whose target never moves (a pure single-dwell run) has span_c
 *     == 0, which would make the normalization divide by zero. Floored at
 *     PROFILE_EXECUTOR_FIRING_STATS_MIN_SPAN_C so the number stays finite --
 *     documented as a real, if unusual, degenerate case rather than an
 *     unhandled one. */
typedef struct {
    float    mean_error_c;         /* SIGNED (actual - target), sum/sample_count; sign says
                                     * hot-running (+) vs cold-running (-) zone */
    float    max_overshoot_c;      /* largest POSITIVE (actual - target) seen, 0 if never overshot */
    uint32_t max_overshoot_elapsed_s; /* profile_exec_status_t.total_elapsed_s at that tick */
    uint8_t  max_overshoot_segment;   /* segment_index at that tick */
    float    max_undershoot_c;     /* largest POSITIVE (target - actual) seen, 0 if never undershot --
                                     * i.e. a magnitude, not signed the opposite way from overshoot */
    uint32_t max_undershoot_elapsed_s;
    uint8_t  max_undershoot_segment;
    float    iae_raw_c_s;          /* raw integral(|error|)dt, degC*seconds -- grows with run length,
                                     * kept alongside the normalized figure because it's free once the
                                     * accumulator exists and someone will want the un-normalized value */
    float    iae_normalized;       /* iae_raw_c_s / (duration_s * max(setpoint_span_c, MIN_SPAN_C)) --
                                     * THE comparable-across-firings figure; dimensionless */
    float    ramp_err_mean_c;      /* mean |error| while dwelling == false */
    float    ramp_err_max_c;
    float    dwell_err_mean_c;     /* mean |error| while dwelling == true */
    float    dwell_err_max_c;
    uint32_t sample_count;         /* valid (actual_valid == true) ticks counted into the above */
    uint32_t excluded_sample_count; /* actual_valid == false ticks -- NOT counted as zero error;
                                     * high relative to sample_count means "don't trust this run's
                                     * numbers," not "this zone tracked perfectly" */
    uint32_t duration_s;           /* wall-clock seconds this zone was active (RUNNING, not
                                     * per-zone-faulted) and being accumulated over */
} profile_exec_firing_stats_t;

#define PROFILE_EXECUTOR_FIRING_STATS_MIN_SPAN_C 1.0f

/* ROADMAP.md M15 B4: one struct that accounts for the whole duty
 * composition across all four assembly stages -- feedforward
 * (profile_executor_feedforward.c), PID (pid.c's pid_update_terms()),
 * load-cap boost (profile_executor_pid_tick.c), and what's actually handed
 * to heater_output_duty(). Every historical interaction bug in this
 * pipeline (PWM-chop guard disarm, dwell credit, the integral floor) lived
 * at a boundary between two of these stages, and until now no single
 * record captured all of them together -- pid_terms_t (pid.h) only ever
 * saw p/i/d/ff, and the load-cap boost applied after pid.c's own clamp was
 * invisible everywhere off-board.
 *
 * READ-ONLY instrumentation: every field here is either copied from a value
 * the pipeline already computes, or (coupling_correction) a value broken
 * out of an existing accumulator by ALSO accumulating it into a second,
 * parallel float -- nothing here changes what duty is computed or how it
 * is clamped. See the field-by-field write sites: zone_feedforward()
 * (profile_executor_feedforward.c) for ff_hold/ff_climb/
 * coupling_correction, pid_family_zone_tick() (profile_executor_pid_tick.c)
 * for the rest.
 *
 * Deliberately NOT stored: p/i/d/ff duplicate profile_exec_zone_status_t's
 * existing pid_p/pid_i/pid_d/pid_ff (TODO.md 6A.9) -- same values, already
 * on the wire, so they are read from there rather than doubled here.
 * pre_clamp_total is exactly pid_p+pid_i+pid_d+pid_ff (pid.c's `unclamped`
 * local after the integral floor, before the final [0,1] clamp) and is
 * likewise not a separately-written field -- the JSON layer derives it so
 * there is exactly one place that could disagree with itself. */
typedef struct {
    /* Stage A -- feedforward (zone_feedforward()'s return, pre the
     * function's own final [0,1] clamp on hold+climb). */
    float ff_hold;               /* out_hold: hold term INCLUDING the cross-zone coupling
                                  * correction below, EXCLUDING climb -- see zone_feedforward()'s
                                  * own doc comment for why the two must stay split this way. */
    float ff_climb;              /* climb term, computed from the POST-taper rate below */
    float coupling_correction;   /* the Phase-3b cross-zone term folded into ff_hold above,
                                  * broken out so an operator can see how much of ff_hold is
                                  * "this zone's own hold" vs. "a neighbour running off-target" --
                                  * 0.0f whenever no neighbour qualifies or every coupling_coeff
                                  * row is 0 (the common, uncommissioned case). */
    float ff_rate_pretaper_c_per_s;  /* s_exec.target_rate_c_per_s, before zone_taper_climb_rate() */
    float ff_rate_posttaper_c_per_s; /* the rate actually fed to the climb solve -- equal to
                                      * pretaper outside the terminal ease-off window, or while
                                      * dwelling (taper is skipped entirely, see the call site) */
    /* Stage B -- gains actually in force this tick (PID_FUZZY may have
     * adjusted these from z->pid_cfg; plain PID mode reports z->pid_cfg's
     * own values unchanged). p/i/d/ff themselves are NOT duplicated here --
     * see this struct's own doc comment -- read pid_p/pid_i/pid_d/pid_ff. */
    float kp_effective;
    float ki_effective;
    float kd_effective;
    float post_clamp_total;      /* pid_update_terms()'s returned duty -- p+i+d+ff after pid.c's
                                  * own [0,1] clamp, i.e. z->duty before the load-cap boost below */
    /* Stage C -- load-cap deferred-credit boost (profile_executor_pid_tick.c),
     * applied AFTER pid.c's clamp and therefore able to push the commanded
     * duty above post_clamp_total -- exactly the term that used to be
     * invisible in pid_terms_t. */
    float load_cap_boost;        /* boosted_duty - post_clamp_total; 0.0f whenever no credit was
                                  * available/consumed this tick */
    float final_commanded;       /* boosted_duty -- what heater_output_duty() actually received.
                                  * final_commanded == post_clamp_total + load_cap_boost by
                                  * construction; Stage D (window quantization/floors) happens
                                  * inside heater_output_duty() itself and is not duty, it is
                                  * on-time -- out of scope for this struct. */
} zone_duty_breakdown_t;

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
    /* Coupled-hold solve status (Opus review, blocker 3 -- these used to be
     * written by zone_feedforward() and read nowhere off-board, exactly the
     * "producer with no consumer" bug class this repo has hit repeatedly).
     * ff_hold_used_matrix false means this zone's hold term is the legacy
     * per-zone diagonal fallback, not the coupled solve -- expected and
     * harmless for an uncommissioned zone, worth noticing for one that
     * should be coupled. ff_hold_infeasible true means the coupled solve
     * needed to clamp some zone's duty UP to 1.0 -- the commanded setpoint
     * combination is not physically achievable as specified; only
     * meaningful alongside ff_hold_used_matrix == true. */
    bool     ff_hold_used_matrix;
    bool     ff_hold_infeasible;
    /* Coupled-CLIMB solve status -- same meaning as the two fields just
     * above, for the coupled climb term (the "extra duty to make the
     * temperature rise at the commanded ramp rate" half of feedforward,
     * previously computed per-zone/uncoupled -- see
     * zone_coupling_solve_climb()'s doc comment in zone_coupling_solve.h).
     * NOT yet wired into dashboard_http.c's JSON as of this field's
     * addition -- the UI-owning agent still needs to add
     * "ff_climb_used_matrix"/"ff_climb_infeasible" alongside the existing
     * "ff_hold_used_matrix"/"ff_hold_infeasible" keys at that call site. */
    bool     ff_climb_used_matrix;
    bool     ff_climb_infeasible;
    /* Running total of coupled-system membership changes this run (Opus
     * review round 3, item 3) -- each one triggers a PID-integral reseed
     * (profile_executor.c's pid_family_zone_tick(), on ff_membership_
     * changed) that keeps commanded duty from stepping, but a fast-growing
     * count means a flapping interlock/coupled neighbour is repeatedly
     * forcing that reseed, which leaves integral action effectively off for
     * this zone -- a safe but otherwise silent failure mode. */
    uint32_t ff_membership_change_count;
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
    /* ROADMAP.md M15 B4: the rest of the duty pipeline that pid_p/pid_i/
     * pid_d/pid_ff alone never captured -- feedforward's hold/climb split,
     * the cross-zone coupling term, the gains actually in force this tick,
     * and the load-cap boost applied after pid.c's own clamp (previously
     * invisible off-board entirely). See zone_duty_breakdown_t's own doc
     * comment for the field-by-field meaning and write sites. */
    zone_duty_breakdown_t duty_breakdown;
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

    /* PID_EXPANSION_PLAN.md Phase 7a: this run's tracking-quality figures so
     * far -- live and still accumulating while state == RUNNING/PAUSED,
     * frozen at their final values once state == DONE/FAULTED (or an
     * operator halt() ends the run early). See profile_exec_firing_stats_t's
     * own doc comment for the exclusion/edge-case rules. Meaningless (all
     * zero) for a zone with .active == false. UI SERIALIZATION: this whole
     * struct is what dashboard_http.c/dashboard_json.* should expose per
     * zone for "this firing's tracking quality so far" -- see each field's
     * comment for units and sign. */
    profile_exec_firing_stats_t firing_stats;

    /* docs/audits/iter_tune_decision_2026-09-07.md prep: this zone's
     * actual_c at its first accumulated tick this run -- see zone_runtime_t.
     * fs_start_temp_c's own doc comment (profile_executor_internal.h) for
     * the exact capture rule. Deliberately NOT added to profile_exec_
     * firing_stats_t/profile_firing_zone_record_t above: that struct is
     * embedded byte-for-byte in profile_firing_history_blob_t, which is
     * persisted to NVS as a fixed-size blob with no version field
     * (profile_executor_firing_stats.c's firing_stats_persist()/
     * firing_stats_load()) -- widening it changes that blob's sizeof and is
     * a schema bump, out of scope for this reversible prep pass. This
     * field is LIVE-ONLY (profile_executor_get_status(), /api/profile_exec)
     * and carries no persisted counterpart yet. */
    float    start_temp_c;

    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: sustained-lag detection, reported
     * REGARDLESS of ramp_assist_enabled (dashboard_http.c's own field of
     * that name) -- see profile_executor_ramp_assist.c and EXEC_SUSTAINED_
     * LAG_S (profile_executor_internal.h) for the threshold and reasoning.
     * ramp_lag_held_s is 0/false-sustained for a zone that is not currently
     * inside s_exec.ramp_lock_lagging_mask, or that has been for less than
     * EXEC_SUSTAINED_LAG_S continuously -- a brief hold during normal PID
     * settling is not reported as sustained. The two rate fields are only
     * meaningful while ramp_lag_sustained is true: commanded is this
     * segment's own signed ramp_c_per_hr, achieved is measured from actual_c
     * over the time this lag has been held. */
    bool     ramp_lag_sustained;
    float    ramp_lag_held_s;
    float    ramp_lag_commanded_rate_c_per_hr;
    float    ramp_lag_achieved_rate_c_per_hr;

    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit, reported REGARDLESS of
     * ramp_assist_enabled (same "accumulate/report always, spend gated"
     * split as the lag fields just above) -- live heat-work-seconds this
     * zone has banked toward its next dwell, reset to 0 the instant that
     * dwell is entered whether or not the flag was on to actually spend it.
     * See zone_runtime_t.dwell_credit_s (profile_executor_internal.h). */
    float    ramp_dwell_credit_s;
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

    /* PID_EXPANSION_PLAN.md sec 7.2: auto-stretch INSTRUMENTATION -- how
     * much of segment_elapsed_s/target_c's real-world advance has been
     * "extra" time the ramp-lock (sec 7.1) spent holding the setpoint,
     * because ramp_assist_cfg_enabled() was true while this run needed it.
     * Both 0 for the whole life of a run started with the flag off, or for
     * any run that never lagged. See s_exec_state_t.stretch_by_segment_s/
     * stretch_total_s (profile_executor_internal.h) for the accumulation
     * rule -- this is CURRENT segment / running total, not the full
     * per-segment array (dashboard_json.h's per-field buffer budget). */
    float    ramp_stretch_segment_s;
    float    ramp_stretch_total_s;

    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit actually SPENT against
     * the most recent dwell's timer, in seconds -- 0.0f for a run started
     * with ramp_assist_cfg_enabled() false (load-bearing: dwell timing must
     * stay bit-identical with the flag off, see ramp_assist_cfg.h). Mirrors
     * ramp_stretch_total_s's "0 whole-run-life unless assist actually
     * changed something" shape. See s_exec_state_t.dwell_credit_applied_s
     * (profile_executor_internal.h) for the exact accumulation/reset rule. */
    float    ramp_dwell_credit_applied_s;
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

    /* docs/audits/profile_executor_panic_2026-09-24.md: true when THIS run's
     * FAULTED state (fault_reason/fault_guard above) came from
     * exec_handle_mode_state_violation() forcing a mode-state-check
     * violation to FAULTED, rather than an ordinary thermal guard trip --
     * fault_guard reads THERMAL_GUARD_TRIP_NONE (0) in that case, since this
     * is not a thermal_guard_trip_t fault. mode_state_violation_count is a
     * lifetime-of-this-boot diagnostics counter (never reset by a run
     * start), reported unconditionally so it is visible even once the
     * board has moved past the run that latched it. */
    bool     mode_state_fault_latched;
    uint32_t mode_state_violation_count;
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
    /* SAFETY_CMD_SET_FIRING_CEILING's firing_max_c for the firing in progress
     * (s_exec.firing_ceiling_c) while RUNNING or PAUSED; 0.0f ("no firing /
     * no ceiling known", LINK_PROTOCOL.md sec 4) in every other state.
     * safety_build_and_send_context() resends this every poll period, so the
     * Pico's RAM-only copy is level-triggered: it recovers from a lost frame
     * and from a Pico reboot mid-firing, and an ESP reboot (which reports
     * IDLE) clears whatever stale ceiling the Pico still held. */
    float    firing_ceiling_c;
} profile_exec_status_t;

/* Snapshot for the dashboard's status API -- never blocks on the executor
 * task. Fills *out from live state under s_exec.lock; safe to call before
 * profile_executor_start() (reports a zeroed/IDLE snapshot). */
void profile_executor_get_status(profile_exec_status_t *out);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_EXECUTOR_STATE_H
