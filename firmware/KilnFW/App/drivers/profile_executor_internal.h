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
 * profile_history_entry_t) ---------------------------------------------- */
typedef struct {
    uint16_t elapsed_periods;
    int16_t  actual_dc;   /* deci-degC */
    int16_t  desired_dc;  /* deci-degC */
    uint8_t  duty_pct;
    uint8_t  guard;
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

    /* History ring buffer (TODO.md section 0 / 6A.9) -- single
     * representative zone, see profile_executor.h's doc comment. */
    history_slot_t history[HISTORY_MAX_SAMPLES];
    uint16_t history_count;
    uint16_t history_head;
    TickType_t history_run_start_tick;
    TickType_t history_last_sample_tick;
    uint8_t history_zone; /* lowest-indexed active zone this run */
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
