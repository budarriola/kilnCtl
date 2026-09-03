#ifndef PROFILE_EXECUTOR_INTERNAL_H
#define PROFILE_EXECUTOR_INTERNAL_H

/* Internal seams for the profile_executor.c split (2026-09-01, "files over
 * 1500 lines should be broken up where it makes sense" -- profile_executor.c
 * had grown to 4962 lines, the last of the day's four biggest firmware
 * files; uart_bridge.c, zones_http.c and safety_link.c were split earlier).
 * This header is NOT public API -- profile_executor.h stays that -- it
 * exists purely so pieces that used to be one translation unit (and could
 * reach each other's `static` state and helpers for free) can still do so
 * now that they are three:
 *
 *   profile_executor.c               -- task/lifecycle, segment stepping,
 *                                        run()/halt()/pause()/resume()/
 *                                        get_status(), config reload, PID
 *                                        family tick, relay/IO segments,
 *                                        guard escalation, watchdog task
 *   profile_executor_feedforward.c   -- zone model load, cross-zone coupling
 *                                        wrappers, zone_feedforward(),
 *                                        bumpless-transfer seeding
 *   profile_executor_firing_stats.c  -- firing_stats_* accounting/NVS
 *                                        persistence, the run-snapshot
 *                                        reboot breadcrumb
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly -- see each new file's own
 * top-of-file comment for which of these it defines vs. only consumes.
 * s_exec_state_t/zone_runtime_t/etc. moved here unchanged so all three
 * files see the identical layout; the original anonymous
 * `static s_exec_state_t s_exec;` is now defined (non-static) in
 * profile_executor.c and `extern`-declared here.
 *
 * test_profile_executor_prestart.c #includes all three .c files directly
 * into one translation unit (same reason as test_zones_http.c -- see that
 * file's own header comment), so PE_TAG is defined once, non-static, in
 * profile_executor.c and only declared `extern` here -- three independent
 * `static const char *PE_TAG = ...` in one TU would be a redefinition error
 * there even though it's fine in the normal separate-TU firmware build. */

#include "profile_executor.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "heater_output.h"
#include "run_state.h"
#include "zone_coupling_solve.h"

/* ---- shared log tag ------------------------------------------------------ */
extern const char *PE_TAG;

/* Pre-start warnings from the POLLED readers (profile_executor_status.c),
 * throttled to one line each per boot.
 *
 * The action entry points (run/halt/pause/resume) log every time, which is
 * right -- each is a discrete operator request that got refused, and there
 * are never many. The readers are different: safety_link.c's poll task calls
 * profile_executor_get_status() every 500 ms, the dashboard polls it every
 * 2 s, and every GET /api/status hits it too. In recovery mode, where this
 * module is deliberately never started, an unthrottled warning there is a
 * continuous stream that floods the UART log bridge (this board already
 * drops lines when that queue fills) and buries the recovery-mode banner --
 * degrading exactly the mode these guards exist to make survivable. Once per
 * boot says everything a reader needs; the hundredth copy says nothing. */
#define LOG_PRESTART_ONCE(msg)                                                                       do {                                                                                                  static bool s_warned_once = false;                                                                if (!s_warned_once) {                                                                                 s_warned_once = true;                                                                             ESP_LOGW(PE_TAG, msg " (further occurrences this boot are suppressed)");                         }                                                                                              } while (0)

/* Time-proportioning window defaults (TODO.md 6A.1), used when a zone
 * hasn't configured its own via zones_config_get_heater_cfg() (6A.9). 60s
 * matches the doc's "mechanical relay" default (the open question about SSR
 * vs. direct element drive is still open per TODO.md 6A.0). */
#define HEATER_WINDOW_MS HEATER_DEFAULT_WINDOW_MS
#define HEATER_MIN_ON_MS HEATER_DEFAULT_MIN_ON_MS
#define HEATER_MIN_OFF_MS HEATER_DEFAULT_MIN_OFF_MS

/* PID defaults (TODO.md 6A.2) -- not yet per-zone/per-band configurable
 * beyond Kp/Ki/Kd themselves. Gain scheduling by temperature band (6A.4) is
 * unbuilt; this is a single band. */
/* Ambient substituted when not one thermocouple channel could report a valid
 * cold junction at firing start (every channel's SPI transfer failed, or every
 * one flagged CJRANGE). Feedforward is NOT disabled in that case, because the
 * cost of being wrong here is bounded and small: the hold term is
 * (T_sp - T_ambient)/K_dc, so an ambient that is off by 15 degC moves the
 * commanded duty by 15/K_dc, and K_dc for a kiln is hundreds of degC per unit
 * duty -- under 2% duty, which the integrator absorbs within a window or two.
 * Losing feedforward for the whole firing over a CJ sensor is the worse trade.
 * A plain indoor room; the substitution is logged at WARN so the operator can
 * see which number the firing actually ran on. */
#define FALLBACK_AMBIENT_C 20.0f

#define PID_D_FILTER_TAU_S 30.0f
#define PID_SETPOINT_WEIGHT_B 1.0f
#define PID_FUNCTIONAL_RANGE_C 25.0f

/* ---- packed history storage (see profile_executor.h's note on
 * profile_history_entry_t) ----------------------------------------------
 *
 * 2026-09-01 multi-zone history fix: desired_dc stays a single shared field
 * (target_c is one ramp across every zone in a run, TODO.md 6A.5) but
 * actual/duty/guard each widen to one slot per zone. Per-slot size:
 *   2 (elapsed_periods) + 2 (desired_dc) + 2*3 (actual_dc) + 1*3 (duty_pct)
 *   + 1*3 (guard) = 16 bytes at MAX31856_CHANNEL_COUNT==3, vs. 8 before --
 * NOT a naive 3x (24 bytes) because elapsed/desired are shared, not repeated
 * per zone. At HISTORY_MAX_SAMPLES==640 (profile_executor.h's own 2026-09-02
 * "display-only, right-sized to the two real consumers" comment) that's
 * 10240 bytes (10KB) -- down from 46080 bytes (45KB) at the old 2880-sample/
 * 24h sizing, which itself would have been a real problem sitting in .bss
 * next to a board that has been found running with single-digit KB of
 * internal DRAM free (see s_exec_state_t.history's own comment below) -- so
 * this buffer is heap_caps_malloc'd from PSRAM instead of declared inline
 * either way, which nets the board a WIN on internal DRAM (the old 23KB
 * inline array is gone from .bss) even though this struct itself grew. */
#define HISTORY_ZONE_COUNT MAX31856_CHANNEL_COUNT

typedef struct {
    uint16_t elapsed_periods;
    int16_t  desired_dc;                     /* deci-degC; shared setpoint across zones */
    int16_t  actual_dc[HISTORY_ZONE_COUNT];   /* deci-degC per zone */
    uint8_t  duty_pct[HISTORY_ZONE_COUNT];    /* per zone */
    uint8_t  guard[HISTORY_ZONE_COUNT];       /* per zone thermal_guard_trip_t */
    /* 2026-09-02 (opus review of 3f9b1a9/2a8ff7e): which zones THIS sample's
     * run had active, captured at sample time rather than read back from the
     * executor's CURRENT run later. Before this field, callers reached for
     * s_exec.profile.zone_mask (or the dashboard's lastExecStatus.zone_mask)
     * to know which of actual_dc/duty_pct/guard above are real vs. the
     * inactive-zone filler -- correct only for the slot sampled during the
     * run that is still active. A finished/idle executor zeroes zone_mask
     * (profile_executor_status.c), so every historical row read back that
     * way after a run ended, or read back while a DIFFERENT run is active,
     * silently disagreed with its own data (whole-kiln average duty NaN'd
     * out or averaged one run's samples against another run's mask). One
     * byte/slot; see HISTORY_MAX_SAMPLES's own sizing comment for the total
     * cost this adds to the ring. */
    uint8_t  zone_mask;
} history_slot_t;

#define HISTORY_TEMP_INVALID INT16_MIN
#define HISTORY_DUTY_INVALID 0xFFu

/* ---- per-zone/global executor runtime state (unchanged from the
 * original single file -- see s_exec_state_t's own doc comment) ---- */

typedef struct {
    bool     active;   /* this zone is in the current run's zone_mask */

    pid_cfg_t pid_cfg;
    pid_state_t pid_state;

    /* ZONE_CONTROL_MODE_PID_FUZZY only (PID_EXPANSION_PLAN.md Phase 3
     * hazard 3): the effective Ki pid_fuzzy_adjust() produced on the last
     * tick that actually ran it, so this tick can detect a change and rescale
     * pid_state.integral before calling pid_update_terms() -- otherwise a
     * fuzzy-driven Ki move steps the I term's contribution (i_term =
     * ki*integral) discontinuously, every tick, since the rule table can
     * legitimately re-fire a different cell tick to tick. 0 means "not yet
     * seeded" (mode just switched to fuzzy, or a fresh pid_reset()) -- the
     * first fuzzy tick then does no rescale, matching what pid_reset()
     * already does for the plain-PID path (cold start, no bump to avoid). */
    float fuzzy_prev_effective_ki;

    /* TODO.md 6A.2 feedforward: this zone's identified FOPDT plant model, as
     * autotune left it in zone config (zones_config_get_model()). Cached at
     * run start and refreshed by reload_zone_config() like every other zone
     * setting, so a model accepted from an autotune that finished DURING this
     * firing starts being used on the next tick rather than the next run.
     *
     * ff_enabled is the single "is this model usable" verdict, computed once
     * per read instead of re-validated every tick: a zone that has never been
     * autotuned has no model at all, and 6A.2 is explicit that feedforward is
     * "default on once autotune has run, off before that". Driving a kiln from
     * a fabricated K_dc is worse than driving it from feedback alone -- the
     * error goes straight into the duty, and unlike the integrator nothing
     * ever corrects it. */
    float ff_k_dc;   /* degC of steady-state rise per unit duty */
    float ff_tau_s;  /* plant time constant, seconds */
    /* Dead time, seconds -- identified alongside k_dc/tau_s but not consumed
     * by the coupled hold/climb solves themselves (those only need k_dc/tau).
     * Kept here for the terminal ease-off taper (PID_EXPANSION_PLAN.md
     * sec 3.1, sim_calibration.md sec 5): the taper's window is sized off
     * THIS zone's own dead time, never a hand constant, so a short-dead-time
     * zone eases later and over a shorter absolute window than a long one.
     * See zone_taper_climb_rate()'s doc comment. */
    float ff_dead_time_s;
    bool  ff_enabled;

    /* Coupled-hold solve diagnostics (defect: steady-state hold used to
     * divide by this zone's own diagonal gain alone -- see
     * solve_hold_for_zone()'s doc comment). Written by zone_feedforward()
     * every call via s_exec.zones[zi] (not through the const z pointer
     * zone_feedforward() itself is handed -- see that write site's comment),
     * read by profile_executor_get_status() so an infeasible or degraded
     * solve is visible off-board, not just in the log. */
    bool  ff_hold_used_matrix;  /* true = last hold term came from the coupled solve; false = the
                                  * legacy per-zone diagonal fallback (unqualified zone, no coupled
                                  * neighbours, or a singular/ill-conditioned matrix) */
    bool  ff_hold_infeasible;   /* true = the last coupled solve needed to clamp at least one zone's
                                  * duty UP to 1.0 -- more heat than that zone can physically
                                  * deliver was requested. A negative raw component is never what
                                  * sets this (see solve_hold_for_zone()'s "b < 0 case" doc comment
                                  * -- needing less than zero duty is always achievable). Only
                                  * meaningful alongside ff_hold_used_matrix == true. */
    uint8_t ff_hold_reason;     /* coupling_solve_reason_t, widened to a plain uint8_t so this
                                 * header doesn't have to forward-declare that profile_executor.c-
                                 * local enum -- 0 (COUPLING_SOLVE_OK) iff ff_hold_used_matrix. */

    /* Coupled-CLIMB solve diagnostics -- same shape and meaning as the three
     * ff_hold_* fields immediately above, for the coupled climb term added
     * alongside the coupled hold (the climb term used to be the uncoupled
     * per-zone `rate*tau/k_dc` formula unconditionally; see
     * zone_coupling_solve_climb()'s doc comment). There is deliberately no
     * ff_climb_membership_changed/ff_climb_membership_change_count pair: the
     * hold and climb solves share the same qualifying-neighbour criteria, so
     * a membership edge always shows up in ff_membership_changed/
     * ff_membership_change_count already (zone_feedforward() ORs both
     * solves' edges into that one flag) -- a second counter would just double
     * count the same event. */
    bool  ff_climb_used_matrix;
    bool  ff_climb_infeasible;
    uint8_t ff_climb_reason;
    bool  ff_membership_changed; /* true = the set of zones in zi's coupled system (or whether zi
                                  * qualifies at all) differs from the immediately preceding call --
                                  * pid_family_zone_tick() re-seeds the PID integral on this edge
                                  * (Opus review, blocker 2) so a membership flip cannot step
                                  * commanded duty; see that call site's doc comment. */
    uint32_t ff_membership_change_count; /* running total of ff_membership_changed edges this run
                                          * (Opus review round 3, item 3) -- a fast-growing count means
                                          * a flapping interlock/neighbour is repeatedly forcing the
                                          * bumpless reseed, which leaves integral action effectively
                                          * off for this zone (a safe but silent failure mode: sustained
                                          * FF+P offset instead of a repeated duty step). Never reset
                                          * mid-run -- see the write site's doc comment for the
                                          * rate-limited log this also drives. */
    heater_output_cfg_t heater_cfg;
    heater_output_state_t heater_state;
    thermal_guard_cfg_t guard_cfg;
    thermal_guard_state_t guard_state;
    zone_control_mode_t control_mode;

    /* The relay mask this zone owned as of the last config read. apply_relay()
     * deliberately re-reads the live mask on every call, so nothing else needs
     * a cached copy -- but the mid-run reload (TODO.md 6A.7) does: when the
     * operator re-assigns relays, the ones that must be de-energized are the
     * ones the zone owned a moment ago, and the live config can no longer name
     * them. Without this, an edit could strand energized relays under a mask no
     * zone controls any more, where no guard would ever turn them off. */
    uint8_t relay_mask;

    bool  relay_commanded_on;
    float duty;
    pid_terms_t last_pid_terms;

    /* Cross-zone coupling (PID_EXPANSION_PLAN.md 2c/Phase 3b): low-pass
     * filtered copy of actual_c, updated once per control tick (not once per
     * zone_feedforward() call -- see that update site's comment) at this
     * zone's OWN d_filter_tau_s, the same tau/alpha formula pid.c uses for
     * its D term. Read by every OTHER zone's zone_feedforward() as the
     * neighbour temperature for the disturbance term, so thermocouple noise
     * on this zone does not land straight in a neighbour's duty. */
    float coupling_filtered_c;
    bool  coupling_filter_init;

    /* TODO.md 6A.2's cooling-limited diagnostic: seconds duty has
     * continuously read 0 while still PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C
     * above target, in PID mode. Reset to 0 the instant either condition
     * breaks -- this is a debounce against a normal brief overshoot, not an
     * accumulating total. */
    float cooling_limited_hold_s;
    bool  cooling_limited;

    /* TODO.md 6A.5 load-staggering cap: ms of on-time this zone wanted but
     * was denied because zones_config_get_max_simultaneous_relays() was
     * exceeded this tick. Paid back as a duty boost on this zone's next
     * PID-mode window (see apply_load_cap()/duty boost in the control
     * loop) -- "deferred, not dropped." Bang-bang zones accumulate this
     * too but it's never paid back (no window to pay it into); documented
     * limitation, see the control loop's bangbang case. */
    float deferred_on_ms;

    float actual_c;    /* calibration-corrected; NAN if invalid */
    bool  actual_valid;

    /* Contact-cycle accounting (TODO.md 6A.1): heater_output counts relay
     * transitions per zone in RAM; this is how much of that count has
     * already been handed to relay_cycles.c, so each tick only reports the
     * delta. Kept here rather than in heater_output.c because that module is
     * pure and owns no persistence. */
    uint32_t cycles_reported;

    bool     faulted;         /* this zone's own per-zone guard trip */
    bool     per_zone_blocked; /* true if this run set relay_authority_set_zone_blocked() for it */
    /* Last relay_authority_zone_blocked() answer for this zone, refreshed
     * every tick that wanted heat. Reported over GET /api/profile_exec and
     * logged on the edge, because a run that is blocked from its very first
     * tick used to produce no evidence anywhere: apply_relay()'s "forced off"
     * warning only fires when a relay was ALREADY on, so a firing that never
     * energized anything sat at duty 0.0, faulted=false, state "running",
     * with a target climbing convincingly for as long as anyone watched it.
     * Observed on the bench doing exactly that for two minutes. */
    bool     heat_blocked;
    uint32_t heat_blocked_sources;
    char     fault_reason[96];
    thermal_guard_trip_t fault_guard;

    /* TODO.md 6A.7's max_ramp_c_per_hr re-check (see reload_zone_config()):
     * latches once this zone's current segment has newly become infeasible
     * against a lowered ceiling, so the WARN logs once per occurrence
     * rather than every reload tick while it stays true. */
    bool     max_ramp_warned;

    /* PID_EXPANSION_PLAN.md Phase 7a: live tracking-quality accumulator for
     * this zone across the current run -- see profile_exec_firing_stats_t's
     * doc comment (profile_executor.h) for the field-by-field rules this
     * fills in. Raw running sums here (err_sum, iae included as a running
     * sum too since it's already an integral); mean_error_c/iae_normalized
     * etc. are DERIVED, computed once by firing_stats_snapshot() rather than
     * divided every tick. Zeroed by profile_executor_run()'s
     * memset(s_exec.zones, ...). */
    float    fs_err_sum;             /* sum of (actual - target) over valid samples */
    float    fs_iae_raw_sum;         /* running integral(|error|)dt */
    float    fs_max_overshoot_c;
    uint32_t fs_max_overshoot_elapsed_s;
    uint8_t  fs_max_overshoot_segment;
    float    fs_max_undershoot_c;
    uint32_t fs_max_undershoot_elapsed_s;
    uint8_t  fs_max_undershoot_segment;
    float    fs_ramp_abs_err_sum;
    uint32_t fs_ramp_sample_count;
    float    fs_ramp_err_max_c;
    float    fs_dwell_abs_err_sum;
    uint32_t fs_dwell_sample_count;
    float    fs_dwell_err_max_c;
    uint32_t fs_sample_count;        /* valid samples */
    uint32_t fs_excluded_sample_count; /* actual_valid == false samples */
    uint32_t fs_duration_s;          /* wall-clock seconds accumulated over */

    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: ramp assist's sustained-lag
     * detection, ALWAYS updated regardless of ramp_assist_cfg_enabled()
     * (the owner wants to see raw lag behaviour during PID testing with
     * assist off). See profile_executor_ramp_assist.c's ramp_assist_zone_
     * lag_tick() for the update rule; this is deliberately a SEPARATE
     * accumulator from the existing s_exec.ramp_lock_held/lagging_mask
     * (7.1's already-built instantaneous per-tick signal) -- lag_held_s is
     * how long THIS zone has been continuously on the wrong side of that
     * signal, which the instantaneous bit alone cannot answer. */
    float    lag_held_s;          /* continuous seconds this zone has been lagging; 0 when not */
    bool     lag_sustained;       /* lag_held_s >= EXEC_SUSTAINED_LAG_S -- the reportable condition */
    float    lag_start_actual_c;  /* actual_c snapshot at the tick lag_held_s left 0, for the
                                   * achieved-rate arithmetic reported alongside lag_sustained */

    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit. Heat-work-weighted
     * accumulator -- banked ONLY while this zone is lagging (the same
     * instantaneous s_exec.ramp_lock_lagging_mask bit lag_held_s above
     * watches) AND actual_c has already entered the half-cone-step band
     * below the CURRENT ramp segment's own target_c (cone_table_band_
     * bottom_c()), mirroring tools/PcTools/src/kilnctrl/ramp_assist.py's
     * z.credit_s accrual (~line 301) exactly: `credit += weight * dt`.
     * ALWAYS updated regardless of ramp_assist_cfg_enabled() -- see this
     * zone's lag_held_s comment just above for why (owner wants visibility
     * with assist off); see profile_executor.c's dwelling-transition code
     * for why SPENDING this is gated even though banking it is not.
     * Reset to 0.0f every time a dwell is entered (spent once, whether or
     * not the spend was actually applied to the dwell timer -- same
     * "z.credit_s = 0.0" unconditional reset ramp_assist.py's DwellStep
     * branch uses regardless of its own apply_dwell_credit flag). */
    float    dwell_credit_s;
    /* REMOVED (2026-09-03, opus review of commit 5312e14, "DEFECT 2"): this
     * field used to be a second "audit" accumulator, computed at the same
     * gate as dwell_credit_s above via its own separate cone_table_heat_
     * work_weight() call and its own separate `+=`. It was deleted rather
     * than kept, deliberately: both `+=` statements shared the same weight
     * function, the same band, the same target and the same accrual gate,
     * so it could only ever catch a copy-paste slip confined to ONE of the
     * two duplicate lines -- it was blind by construction to a wrong Ea, a
     * wrong band width, a wrong weight function, a wrong dt or a wrong
     * in-band predicate, i.e. every real shape DEFECT 1 (the 2x-too-large
     * credit) could take. PID_EXPANSION_PLAN.md sec 7.3.3 had described it
     * as "the audit-catches-a-scaled-regression property" for exactly one
     * mutation (an unweighted accrual line) that happened to touch only the
     * non-audit `+=` -- true of that one mutation, not of the property in
     * general, and indistinguishable from real protection without reading
     * this comment. Real protection against DEFECT 1 now comes from
     * test_profile_executor_prestart.c's independently hand-computed
     * weight pins (Arrhenius formula worked out by hand against the
     * documented Ea/R constants, NOT by calling cone_table_heat_work_
     * weight() and recording what it returns) -- see
     * test_dwell_credit_tick_accrues_while_lagging_in_band()'s comment. */
} zone_runtime_t;

/* TODO relay/IO segments: per-segment runtime tracking, one slot per
 * profile_segment_t index. See s_exec_state_t.io_segs's own doc comment for
 * why this has to be its own array rather than folded into segment_index. */
typedef struct {
    bool active;      /* this segment has been started (io_seg_start()) and not yet finished
                        * (io_seg_finish()) -- false for every ZONE_RAMP segment always */
    bool is_relay;    /* true = kiln relay 1-4 (relay_authority-gated, sweep_unowned_relays()-
                        * visible); false = general-purpose IO_1..IO_7 (kiln_io_set_io(), no
                        * ownership/sweep concept -- see kiln_io.h) */
    uint8_t target;   /* relay 1-4 or IO_1..7 index, already validated at save time
                        * (profiles_http.c's validate_io_segment()) and re-validated at run
                        * start (relay_io_target_is_zone_owned() below) */
    bool blocking;    /* copy of profile_segment_t.io_blocking, decoded once at start so
                        * io_segs_tick() doesn't need the profile segment back */
    bool state_on;    /* what this segment commanded */
    bool leave_on_at_end; /* copy of profile_segment_t.io_leave_on_at_end -- see
                            * io_seg_finish()'s doc comment for exactly when this is honored */
    float remaining_s;    /* counts down from dwell_min*60 while active && !blocking;
                            * meaningless for a blocking segment, which is finished by the
                            * segment-stepping block itself, not by io_segs_tick() */
} io_seg_runtime_t;

typedef struct {
    kiln_io_t *io;
    MAX31856BusClass *thermo_bus;
    SafetyLinkClass *safety;

    /* Guards every field below -- held only for a read/update, never across
     * a relay/thermo/safety call, same discipline as safety_link.h's
     * state_lock. */
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    TaskHandle_t watchdog_task;
    TickType_t   last_tick_tick; /* updated every control-task iteration, regardless of run state -- guard 9 reads this */

    profile_exec_state_t state;
    profile_t profile; /* copy taken at profile_executor_run(); the saved
                        * original in profiles_http.c can change under us
                        * without disturbing an in-progress run */
    uint8_t profile_id;

    zone_runtime_t zones[MAX31856_CHANNEL_COUNT];

    uint8_t segment_index;
    bool dwelling;
    float target_c;              /* shared setpoint, stepped incrementally each tick */
    uint32_t segment_elapsed_s;  /* shared; only advances while ramp-lock is satisfied */
    TickType_t prev_control_tick; /* for the *measured* dt_s pid.c/heater_output.c want */

    /* The rate the profile is COMMANDING the setpoint to move at, degC/s,
     * signed, recomputed by the segment-stepping block each tick and 0
     * whenever the setpoint is not being ramped (dwelling, ramp-lock holding,
     * or a step segment with no rate). Feeds the (dT_sp/dt)*tau/K_dc half of
     * TODO.md 6A.2's feedforward.
     *
     * Taken from the segment's commanded ramp rather than differentiated from
     * target_c tick to tick: the commanded rate is exact and noise-free, while
     * a tick-to-tick difference of target_c divides a ~0.03 degC step (100
     * degC/hr at 1 Hz) by a *measured* dt_s that jitters with I2C timing --
     * that quotient is mostly timing noise, and it would be multiplied by tau
     * (hundreds of seconds) before reaching the duty. */
    float target_rate_c_per_s;

    /* Ambient reference for the hold half of the feedforward term, captured
     * ONCE at firing start from the MAX31856's cold-junction sensor (TODO.md
     * 6A.2's "at firing start, not a constant"). Not re-sampled: the cold
     * junction sits on the board and warms up as the board and the room do, so
     * re-reading it would walk the hold term downward over a firing -- a slow
     * drift in commanded duty with no physical cause, exactly the artefact
     * feedforward exists to remove. What the term wants is the temperature the
     * kiln decays toward, and the best single measurement of that is the room
     * before the elements have put any heat into it. */
    float ambient_c;
    bool  ambient_from_cj; /* false = FALLBACK_AMBIENT_C was substituted, see run() */

    /* profile_exec_status_t.run_start_c/total_elapsed_s -- see that header
     * for what these feed. run_start_c is baseline_target_c from run(),
     * captured once and never touched again by the control loop.
     * total_elapsed_s is incremented alongside dt_s below, every RUNNING
     * tick regardless of ramp-lock, and simply not touched while PAUSED. */
    float run_start_c;
    uint32_t total_elapsed_s;

    /* PID_EXPANSION_PLAN.md Phase 7a: run-wide setpoint span for normalized
     * IAE (target_c is shared across every active zone, so the span is
     * computed once here rather than duplicated per zone). NAN until the
     * first RUNNING tick sets both from that tick's target_c. */
    float fs_target_min_c;
    float fs_target_max_c;
    uint32_t run_started_unix_s; /* time(NULL) at profile_executor_run(); 0 if unsynced */
    /* True once this run's firing stats have been persisted (or a persist
     * was attempted) -- guards against writing NVS twice for the same run
     * (once at the DONE/FAULTED tick transition, again if the operator then
     * calls halt() to dismiss it) and against ever writing for a run that
     * never produced a single tick of data. Reset false at profile_executor_
     * run(). */
    bool fs_persisted;

    bool ramp_lock_held;
    uint8_t ramp_lock_lagging_mask;

    /* PID_EXPANSION_PLAN.md sec 7.2: auto-stretch. Sec 7.1's ramp-lock
     * (already built) guarantees every ramp endpoint is eventually reached
     * on its own, by holding target_c/segment_elapsed_s still while a zone
     * is outside EXEC_RAMP_LOCK_BAND_C -- that guarantee is unconditional
     * and this feature does not touch it for a short/normal hold. What sec
     * 7.2 actually adds (profile_executor.c's segment-stepping code, gated
     * on ramp_assist_cfg_enabled()): once a lagging zone's lag_sustained
     * becomes true (the lock has held long enough that it would otherwise
     * hold indefinitely), target_c stops sitting fully frozen and instead
     * creeps forward at the slowest sustained-lagging zone's own
     * demonstrated achievable rate (ramp_assist_zone_achievable_rate_c_per_s(),
     * profile_executor_ramp_assist.c) rather than the commanded seg->
     * ramp_c_per_hr it cannot keep up with -- "extend the schedule to the
     * achievable rate" rather than stop-and-wait. It still only ever moves
     * target_c TOWARD seg->target_c, never past it (same "reached" clamp
     * the normal ramp path already used), so the hard-refusal-above-max-
     * temp guarantee (profile_executor_run.c's start-time check) and the
     * next segment's own target/shape (read fresh once this one reaches
     * seg->target_c and hands off to dwelling) are both untouched by this --
     * only the wall-clock time this segment takes changes. These two fields
     * only RECORD how much wall-clock time the lock has cost, in seconds
     * (during a stretch OR a strict hold), so that cost is a deliberate,
     * reported figure rather than an invisible side effect. Accumulated
     * only while ramp_assist_cfg_enabled() is true (see profile_executor_
     * ramp_assist.c's ramp_assist_stretch_tick()) -- with the flag off, the
     * lock still holds the setpoint exactly as it always has (7.1's
     * guarantee is unconditional), this just stops counting it as "assist".
     * Zeroed by profile_executor_run() like the rest of this struct's
     * per-run state. */
    float stretch_by_segment_s[PROFILE_MAX_SEGMENTS];
    float stretch_total_s;

    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit -- last spend actually
     * applied to a dwell's timer, in seconds. 0.0f for the whole life of a
     * run that never lagged near a target, AND (load-bearing) 0.0f for the
     * whole life of a run started with ramp_assist_cfg_enabled() false --
     * per-zone dwell_credit_s (zone_runtime_t) still banks and is still
     * reset at every dwell entry with the flag off (reporting stays live),
     * this field specifically only reflects what was actually SPENT against
     * segment_elapsed_s/dwell_min, which is the one piece gated on the flag.
     * See profile_executor.c's dwelling-transition code (the "if
     * (s_exec.target_c == seg->target_c)" branch) for where this is set,
     * and ramp_assist_dwell_credit_spend() (profile_executor_ramp_assist.c)
     * for the arithmetic -- min() across every active, non-faulted zone's
     * own dwell_credit_s, because unlike ramp_assist.py's per-zone
     * simulated dwell timers, this executor has exactly ONE shared
     * segment_elapsed_s/dwelling pair across every zone (see this struct's
     * doc comment further up): applying the smallest zone's earned credit
     * is the conservative choice -- no zone is ever credited for heat work
     * it did not itself accrue. Zeroed by profile_executor_run() like the
     * rest of this struct's per-run state.
     *
     * EXTENDED (2026-09-03, sec 7.6, bounded in-dwell credit): this field is
     * now specifically the ENTRY portion of a dwell's total spend -- the
     * one-time snapshot taken exactly as before. It no longer alone decides
     * how long the dwell is shortened by; profile_executor.c's dwelling-
     * transition code combines it with live in-dwell top-up (ramp_assist_
     * dwell_credit_peek_min_s()) via ramp_assist_dwell_credit_total_spend_s(),
     * bounded by dwell_credit_cap_s below. This field itself is still only
     * ever written once per dwell, at entry -- do not start writing it every
     * tick, or the "last spend actually applied" reporting semantics this
     * comment describes break. */
    float dwell_credit_applied_s;
    /* PID_EXPANSION_PLAN.md sec 7.6: fixed cap on THIS dwell's total credit
     * spend (entry + in-dwell top-up combined), computed once at dwell entry
     * as nominal_dwell_s * EXEC_DWELL_CREDIT_MAX_FRACTION and never changed
     * again until the next dwell entry recomputes it. See EXEC_DWELL_CREDIT_
     * MAX_FRACTION's own doc comment for why that fraction is safe. Zeroed
     * by profile_executor_run() like the rest of this struct's per-run
     * state. */
    float dwell_credit_cap_s;

    /* zones_config_generation() as of the last time this run read zone
     * settings (TODO.md 6A.7, "config reload while running"). Comparing one
     * counter per tick is what keeps the reload free on the ~always-taken
     * path: an unchanged generation means not a single getter, and therefore
     * no NVS-backed struct walk, runs in the control loop. Polling rather
     * than a callback from zones_http.c on purpose -- a callback would apply
     * an edit from the HTTP task's context, mid-tick, between this task's
     * decide and apply passes; polling makes every config change land at one
     * known point in the tick, with the executor's own lock already held. */
    uint32_t config_generation;

    /* Every relay bit this run has ever been in a position to command, ORed
     * together and never cleared until the next run starts (TODO.md 6A.7's
     * unowned-relay sweep). This is what keeps the sweep from being a
     * board-wide "anything not currently owned gets opened" rule: relays 1-4
     * are also reachable from the dashboard's manual /api/relay and the UART
     * bridge's SET_RELAY, neither of which consults the executor, and a relay
     * this run never touched is somebody else's to hold closed. The sweep only
     * ever opens contacts this run itself put in play and can no longer name --
     * see sweep_unowned_relays(). */
    uint8_t claimed_relay_mask;

    /* TODO relay/IO segments (owner's request, see profiles_http.h's
     * profile_seg_kind_t doc comment): independent per-segment tracking
     * alongside the single segment_index/dwelling ramp-machine above.
     * "Alongside" is the operative word -- a non-blocking segment is applied
     * and the shared schedule advances PAST it on the same tick (see the
     * segment-stepping block's PROFILE_SEG_KIND_RELAY_IO branch), so by the
     * time its own hold time is still counting down, segment_index no longer
     * points at it at all. This array is the only place that knows such a
     * segment is still live. Indexed by segment number (0..segment_count-1),
     * reset to all-inactive at the start of every profile_executor_run(). */
    io_seg_runtime_t io_segs[PROFILE_MAX_SEGMENTS];

    /* Which GLOBAL fault this run asserted, if any -- so halt() clears
     * exactly that and nothing another caller may have separately asserted. */
    uint32_t global_fault_source; /* 0 = none asserted by this run */
    char     fault_reason[96];
    thermal_guard_trip_t fault_guard;

    /* Warm-start (PROFILES.md, owner request 2026-08-30) -- mirrors
     * profile_exec_status_t's fields of the same name, see that header for
     * what they mean. Set once in profile_executor_run(), read only by
     * profile_executor_get_status(). */
    bool     warm_started;
    char     warm_start_reason[128];
    uint8_t  warm_start_replayed_segments[PROFILE_MAX_SEGMENTS];
    uint8_t  warm_start_replayed_count;

    /* History ring buffer (TODO.md section 0 / 6A.9), now multi-zone -- see
     * profile_executor.h's doc comment on profile_history_entry_t and
     * history_slot_t's own comment just above for the sizing/PSRAM
     * reasoning.
     *
     * Heap-allocated from PSRAM (MALLOC_CAP_SPIRAM), NOT an inline array:
     * this board has been found running with single-digit-KB internal DRAM
     * free (project_esp_internal_dram_exhaustion.md) and PSRAM sits mostly
     * idle (8MB, ~8MB free in a typical GET /api/status snapshot), so this
     * is exactly the "place the buffer in PSRAM" case. Allocated lazily on
     * the first profile_executor_run() (history_buf_ensure_alloc(), profile_
     * executor_run.c) rather than at profile_executor_start() -- a board
     * that never fires a profile never needs the allocation at all. NULL
     * until that first allocation, and if heap_caps_malloc() ever fails
     * (logged at ERROR, once) -- every reader/writer must check before
     * touching it; a run still proceeds without history rather than fault,
     * since losing the graph is not a safety issue. Never freed: history
     * needs to survive from one run to the next (IDLE/DONE/FAULTED states
     * still serve the last run's buffer) for the life of the board. */
    history_slot_t *history;
    uint16_t history_count;
    uint16_t history_head;
    TickType_t history_run_start_tick;
    TickType_t history_last_sample_tick;
} s_exec_state_t;

extern s_exec_state_t s_exec;

/* ---- firing-history NVS blob and reboot-breadcrumb snapshot buffer
 * (moved from profile_executor_firing_stats.c's own region since both
 * types are also used directly by profile_executor.c call sites) ---- */

typedef struct {
    uint8_t count; /* 0..PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH */
    profile_firing_run_record_t runs[PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH]; /* runs[0] = newest */
} profile_firing_history_blob_t;

typedef struct {
    run_state_snapshot_t snap;
    char name[PROFILE_NAME_MAX_LEN + 1];
    char reason[sizeof(s_exec.fault_reason)];
} run_snapshot_buf_t;


/* ---- feedforward / cross-zone coupling (profile_executor_feedforward.c) --- */
bool zone_load_model(uint8_t zi);
void coupling_filter_tick(zone_runtime_t *zn, bool actual_valid_now, float dt_s);
uint8_t count_qualifying_coupling_neighbors(uint8_t zi);
float zone_feedforward(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float rate_c_per_s,
                       float *out_hold);
void seed_bumpless_with_ff(zone_runtime_t *z, uint8_t zi, float u_desired);
float zone_taper_climb_rate(const zone_runtime_t *z, float target_c, float rate_c_per_s,
                            float segment_target_c);

/* ---- firing-statistics accounting / reboot breadcrumb
 * (profile_executor_firing_stats.c) ---------------------------------------- */
void firing_stats_zone_tick(zone_runtime_t *z, float target_c, bool dwelling, uint32_t elapsed_s,
                            uint8_t segment_index, float dt_s);
void firing_stats_snapshot(const zone_runtime_t *z, float setpoint_span_c,
                           profile_exec_firing_stats_t *out);
bool firing_stats_load(uint8_t profile_id, profile_firing_history_blob_t *out);
void firing_stats_persist(const profile_firing_run_record_t *rec);
bool firing_stats_maybe_finalize(profile_firing_run_record_t *out_rec);
void capture_run_snapshot(run_snapshot_buf_t *b);

/* ---- relay/IO segment machinery + guard escalation
 * (profile_executor_relay_io.c) --------------------------------------------- */
void apply_relay(uint8_t zi, bool want_on);
float profile_executor_guard_commanded_duty(bool heat_blocked, float intended_duty);
float profile_executor_guard_sanity_rate(float configured_rate_c_per_min, float target_rate_c_per_s);
void force_zone_relay_off(uint8_t zi);
void force_all_relays_off(void);
void release_profile_relay_claim(void);
bool relay_io_target_is_zone_owned(uint8_t relay_1_4, uint8_t *out_zone_index);
void io_seg_start(uint8_t idx, const profile_segment_t *seg);
void io_seg_finish(uint8_t idx, bool honor_leave_on);
void io_segs_force_all_off(bool honor_leave_on);
void io_segs_tick(float dt_s);
bool escalate_guard_trip(uint8_t zi, thermal_guard_trip_t reason, const char *detail);
void guard9_assert_stale_tick_fault(void);
void clear_this_runs_faults(void);
void force_relay_mask_off(uint8_t zi, uint8_t mask);
void sweep_unowned_relays(void);

/* ---- mid-run per-zone config reload (profile_executor_config_reload.c) --- */
bool reload_zone_config(uint8_t zi);

/* ---- PID-family tick + fuzzy-gain/bumpless-transfer plumbing
 * (profile_executor_pid_tick.c) --------------------------------------------- */
float exec_threshold(uint8_t zone_index, int which);
#define EXEC_BANGBANG_HYSTERESIS_C(zi) exec_threshold((zi), 0)
#define EXEC_COOLING_MARGIN_C(zi)      exec_threshold((zi), 1)
#define EXEC_COOLING_HOLD_S(zi)        exec_threshold((zi), 2)
#define EXEC_RAMP_LOCK_BAND_C(zi)      exec_threshold((zi), 3)
float pid_family_zone_tick(zone_runtime_t *z, uint8_t zi, const pid_cfg_t *cfg,
                           bool sensor_ok_zi, float dt_s, uint32_t dt_ms,
                           bool *out_want_relay_on);
void pid_fuzzy_prepare_gains(zone_runtime_t *z, uint8_t zi, pid_cfg_t *out_cfg);

/* ---- ramp assist: sustained-lag detection + auto-stretch instrumentation
 * (profile_executor_ramp_assist.c) -- PID_EXPANSION_PLAN.md sec 7.1/7.2/7.4.
 * See that file's own doc comment for the honest new-behaviour-vs-
 * instrumentation split. */
/* How long a zone must sit continuously outside EXEC_RAMP_LOCK_BAND_C
 * before its lag is "sustained" rather than a brief, unremarkable hold --
 * ramp-lock (7.1) can legitimately flick on for a tick or two during normal
 * PID settling (thermocouple noise crossing the 3C band, a load-cap-
 * deferred window landing on an unlucky tick), and that is not news. 30s
 * is 30 control ticks at the 1Hz rate (PROFILE_EXECUTOR_TICK_MS) -- long
 * enough that noise/settling on the scale this repo's own zone models
 * report (tau_s in the hundreds of seconds, see zone_runtime_t.ff_tau_s'
 * doc comment) cannot cross it by accident, short enough that an operator
 * watching a live firing sees the warning well before a ramp segment (which
 * can run for hours) is meaningfully behind. Not yet per-zone/per-profile
 * configurable, same status as PROFILE_EXECUTOR_RAMP_LOCK_BAND_C. */
#define EXEC_SUSTAINED_LAG_S 30.0f
void ramp_assist_zone_lag_tick(zone_runtime_t *z, bool lagging_now, float dt_s);
void ramp_assist_stretch_tick(s_exec_state_t *ex, uint8_t segment_index, bool assist_enabled,
                              bool ramping_now, bool lock_held_now, float dt_s);

/* PID_EXPANSION_PLAN.md sec 7.2: auto-stretch's rate computation -- the
 * caller (profile_executor.c's segment-stepping code) uses this in place of
 * seg->ramp_c_per_hr for the tick's target_c advance when it returns >= 0.
 * Returns the sentinel -1.0f (meaning: no stretch this tick, fall back to
 * sec 7.1's existing strict freeze-and-wait) when assist_enabled is false or
 * no currently-lagging zone (lagging_mask) has lag_sustained yet -- a short
 * hold below EXEC_SUSTAINED_LAG_S is normal PID settling, not the "would
 * otherwise hold indefinitely" case this function exists for. Otherwise
 * returns the MINIMUM, across every active/non-faulted/lagging/sustained
 * zone, of (actual_c - lag_start_actual_c) / lag_held_s -- that zone's own
 * average climb since its lag began, clamped to >= 0 so a momentarily
 * falling zone can never run target_c backward. See
 * profile_executor_ramp_assist.c's own doc comment above this function for
 * the full rationale, including why the hard-refusal-above-max-temp
 * guarantee needs no check here. */
float ramp_assist_stretch_rate_c_per_s(s_exec_state_t *ex, uint8_t lagging_mask, bool assist_enabled);

/* PID_EXPANSION_PLAN.md sec 7.3: dwell credit. ALWAYS accrues (not gated on
 * assist_enabled -- see zone_runtime_t.dwell_credit_s's own doc comment);
 * `segment_target_c` is the CURRENT ramp segment's own final target (i.e.
 * profile_segment_t.target_c, NOT the moving s_exec.target_c a ramp is
 * still interpolating toward -- mirrors ramp_assist.py's zone_active_
 * target_c() using step.target_c, not z.commanded_c). No-ops (banks
 * nothing) when `ramping_now` or `behind_schedule_now` is false, or when
 * cone_table reports segment_target_c out of its covered range.
 *
 * `behind_schedule_now` is DELIBERATELY NOT the ramp-lock's `lagging_now`
 * (sec 7.1's `(target_c - actual_c) > EXEC_RAMP_LOCK_BAND_C`, i.e.
 * s_exec.ramp_lock_lagging_mask) -- pass a separate "actual_c < the moving
 * s_exec.target_c" boolean instead, computed at the call site. The two must
 * stay distinguishable: the lock's wide band decides when the schedule
 * freezes (and still must, unconditionally -- auto-stretch, the warning
 * surfaces and the event log all correctly key off the 25C lock and are
 * untouched by this parameter). This credit gate answers a narrower
 * question -- "is the zone behind at all" -- and is already scoped tight by
 * `in_band` (half a cone step below segment_target_c) plus heat-work
 * weighting, so it does not need the lock's wide band on top. Reusing
 * lagging_now here made credit and in_band mutually exclusive (a
 * lock-lagging zone during a ramp is always >25C below segment_target_c,
 * while in_band requires within half a cone step, under 25C almost
 * everywhere in the Orton table) -- credit was measured at exactly zero at
 * bisque/cone 6/cone 10 at every mass loading before this was split out.
 * See profile_executor_ramp_assist.c's own doc comment above this
 * function's definition for the full rationale. */
void ramp_assist_dwell_credit_tick(zone_runtime_t *z, bool ramping_now, bool behind_schedule_now,
                                   float segment_target_c, float dt_s);

/* PID_EXPANSION_PLAN.md sec 7.3 extension (2026-09-03, "extend dwell-credit
 * accrual past the nominal ramp end"): the boolean the caller passes to
 * ramp_assist_dwell_credit_tick() as `ramping_now` above. Deliberately takes
 * ONLY `seg_kind` -- no dwelling flag -- because the owner's decision was to
 * keep banking credit for as long as the CURRENT segment is a ZONE_RAMP,
 * whether or not the shared schedule has already flipped s_exec.dwelling to
 * true for it. The gap this closes: the ramp step ends (dwelling begins) the
 * moment a lagging zone is back within the WIDE 25C ramp-lock band
 * (EXEC_RAMP_LOCK_BAND_C), but credit itself only accrues within the much
 * NARROWER half-cone-step in_band test inside ramp_assist_dwell_credit_tick()
 * -- so under heavy thermal mass a zone routinely un-locks (ending the ramp)
 * while still outside the credit band, and only crosses into it after
 * dwelling has already started. Gating credit on `!dwelling` made that
 * catch-up window uncreditable; measured result was exactly 0.0s of credit
 * at 2x/4x mass for bisque/cone 6 and at 4x for cone 10. Once actual_c
 * reaches segment_target_c, ramp_assist_dwell_credit_tick()'s own in_band
 * test (`actual_c < segment_target_c`) stops accrual on its own -- no
 * dwelling check is needed here to bound it.
 *
 * THE HAZARD (do not remove this gate without re-reading it): credit banked
 * while s_exec.dwelling is already true for THIS segment must never be
 * allowed to shorten THIS SAME dwell -- that would let time spent dwelling
 * (while still in-band) shrink the very timer it is being measured against,
 * a circular, self-shortening loop. This function does not create that risk
 * by itself (it only decides whether to accrue, never what a dwell's
 * threshold is), but the caller's invariant it depends on is: dwell_credit_
 * applied_s is captured EXACTLY ONCE, at the tick a dwell is entered (via
 * ramp_assist_dwell_credit_spend()'s return value), and every later tick's
 * threshold check reuses that same frozen float -- never a live re-read of
 * any zone's dwell_credit_s. See profile_executor.c's dwelling-transition
 * comment (the `s_exec.dwell_credit_applied_s = ramp_assist_dwell_credit_
 * spend(...)` call) and test_dwell_credit_spend_snapshot_is_frozen_against_
 * later_accrual (test_profile_executor_prestart.c) for the pinned proof:
 * credit accrued after spend() runs is visible in dwell_credit_s but the
 * ALREADY-RETURNED spend value never changes. */
static inline bool ramp_assist_credit_should_accrue(profile_seg_kind_t seg_kind)
{
    return seg_kind == PROFILE_SEG_KIND_ZONE_RAMP;
}

/* Computes the seconds to actually shorten a fresh dwell's timer by, given
 * every active/non-faulted zone's currently-banked dwell_credit_s, and
 * resets EVERY active zone's dwell_credit_s to
 * 0.0f as a side effect -- "spent once", whether or not the spend this call
 * returns is ever applied (ramp_assist.py's DwellStep branch does the same
 * unconditional `z.credit_s = 0.0` regardless of apply_dwell_credit). When
 * `assist_enabled` is false the return value is always 0.0f (nothing is
 * ever subtracted from segment_elapsed_s/dwell_min with the flag off --
 * this is the one gated half of dwell credit, see ramp_assist_cfg.h), but
 * the reset-to-0 side effect still happens either way, matching the
 * reference implementation exactly. The return value is the MINIMUM of
 * every active, non-faulted zone's own banked credit (never a zone's own
 * value alone) because this executor has exactly one shared segment_
 * elapsed_s/dwelling pair across all zones, unlike ramp_assist.py's
 * independent per-zone dwell timers -- see s_exec_state_t.dwell_credit_
 * applied_s's doc comment for why min() is the conservative choice. Always
 * clamped to [0, nominal_dwell_s] -- never a negative dwell. A run with no
 * active zones returns 0.0f. */
float ramp_assist_dwell_credit_spend(s_exec_state_t *ex, float nominal_dwell_s,
                                     bool assist_enabled);

/* PID_EXPANSION_PLAN.md sec 7.6: BOUNDED in-dwell dwell credit. The owner's
 * decision (2026-09-03) is to let credit accrued DURING a dwell shorten that
 * SAME dwell -- previously impossible, because profile_executor.c froze the
 * spend snapshot at dwell entry (commit 0402ecb) specifically so a dwell
 * could not shorten itself; that freeze is what made the terminal dwell's
 * banked credit (424s at bisque 4x mass) unspendable, since a terminal dwell
 * has no "next" dwell to apply it to. Relaxing the freeze reintroduces
 * exactly the hazard 0402ecb existed to prevent (unbounded self-shortening),
 * so two bounds are mandatory and are the deliverable, not the feature:
 *
 *   1. EXEC_DWELL_CREDIT_MAX_FRACTION (below) caps the TOTAL reduction (entry
 *      snapshot + in-dwell top-up combined) at a fixed fraction of the
 *      nominal dwell -- no amount of banked heat-work credit, however large,
 *      can cut a dwell by more than that fraction. This is what keeps "N
 *      minutes of timer, not a soak" true even with the freeze relaxed: at
 *      least (1 - EXEC_DWELL_CREDIT_MAX_FRACTION) of the nominal dwell is
 *      always honored on the wall clock, so a dwell can never degrade into
 *      "leave the instant actual_c arrives".
 *   2. ramp_assist_dwell_target_reached() (below) must be true before ANY
 *      credit-shortened exit -- a dwell can end early only once the zone has
 *      actually reached target_c, never before, regardless of how much
 *      credit is banked. Combined with the cap, self-shortening cannot run
 *      away: the earliest a credited exit can happen is bounded below by
 *      nominal_dwell_s * (1 - EXEC_DWELL_CREDIT_MAX_FRACTION), and it can
 *      NEVER happen before the physical target is reached no matter how that
 *      floor is computed.
 *
 * profile_executor.c's dwelling-transition code combines these as:
 *   ready = (elapsed >= nominal_dwell_s) ||
 *           (elapsed >= credited_threshold_s && target_reached)
 * so the pre-existing unconditional-nominal-elapsed fallback is untouched --
 * a dwell with no credit (assist off, or a run that never lagged) is exactly
 * as bit-identical as before; credit only ever gives an EARLY exit option,
 * never removes the guaranteed one. */

/* Fraction of nominal_dwell_s the TOTAL bounded-credit reduction may never
 * exceed, for any dwell, at any mass loading, however much credit is banked.
 * 0.5f (50%): chosen so a dwell always keeps at least half its planned
 * wall-clock length -- generous enough to matter at the load levels measured
 * (424s banked against a 600s bisque terminal dwell, i.e. ~70% of nominal,
 * is a case this cap is deliberately expected to bind on, per the "cap
 * actually binds" test), while small enough that "N minutes of timer" still
 * reads as a timed hold and not a bare "wait for temperature" soak -- the
 * owner's explicit "must not turn it into a soak" requirement. A named
 * constant, not a magic number, so a future change to the fraction is a
 * one-line, reviewable decision. */
#define EXEC_DWELL_CREDIT_MAX_FRACTION 0.5f

/* Read-only peek at the minimum currently-banked dwell_credit_s across every
 * active, non-faulted zone -- same conservative min() direction ramp_assist_
 * dwell_credit_spend() uses, but WITHOUT resetting anything (unlike spend(),
 * this may be called every tick of a dwell to see how much MORE credit has
 * accrued since entry). Returns 0.0f if no zone is active. */
float ramp_assist_dwell_credit_peek_min_s(const s_exec_state_t *ex);

/* Total seconds this dwell's timer should be considered shortened by, right
 * now: entry_applied_s (the frozen snapshot ramp_assist_dwell_credit_spend()
 * returned at dwell entry) plus however much MORE credit has accrued live
 * since then (ramp_assist_dwell_credit_peek_min_s()), clamped to cap_s
 * (EXEC_DWELL_CREDIT_MAX_FRACTION * nominal_dwell_s, computed once by the
 * caller at dwell entry) -- bound 1 above. Never negative, never exceeds
 * cap_s regardless of how large entry_applied_s or the live peek are. */
float ramp_assist_dwell_credit_total_spend_s(const s_exec_state_t *ex, float entry_applied_s,
                                             float cap_s);

/* Bound 2 above: true only when every active, non-faulted zone's actual_c
 * has reached (>=) the shared s_exec.target_c (which already equals seg->
 * target_c throughout a dwell -- profile_executor.c sets it once at dwell
 * entry and never moves it again until the segment advances). A zone whose
 * actual_valid is false counts as NOT reached (an invalid reading is never
 * grounds to end a dwell early) -- same conservative direction as the credit
 * accrual and lag-detection gates elsewhere in this file. A run with no
 * active zones returns true (nothing to wait on). */
bool ramp_assist_dwell_target_reached(const s_exec_state_t *ex);

/* ---- history ring buffer unpack (profile_executor.c; history_pack()/the
 * pack-temp/unpack-temp helpers stay static there, only used by the same
 * file's history-sampling site) ---------------------------------------- */
void history_unpack(const history_slot_t *slot, profile_history_entry_t *out);

/* ---- run() feasibility/warm-start helpers (profile_executor_start.c) ----- */
bool profile_zones_have_ceiling(const profile_t *p, uint8_t *out_missing_zone);

typedef struct {
    bool     warm_started;
    uint8_t  entry_segment_index;
    bool     entry_dwelling;
    float    entry_target_c;
    uint32_t entry_segment_elapsed_s;
} profile_warm_start_plan_t;

profile_warm_start_plan_t profile_executor_plan_warm_start(const profile_t *p, float current_c);

/* ---- control task entry points (profile_executor.c), needed by
 * profile_executor_start.c's xTaskCreatePinnedToCore() calls ------------- */
void executor_task_entry(void *arg);
void watchdog_task_entry(void *arg);

#endif /* PROFILE_EXECUTOR_INTERNAL_H */
