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
#include <stddef.h> /* offsetof() -- profile_firing_history_blob_t layout asserts below */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "heater_output.h"
#include "profile_executor_live_pickup.h" /* profile_live_pickup_result_t -- live_edit_last_refusal field below */
#include "run_state.h"
#include "zone_coupling_solve.h"
#include "autotune_engine.h"
#include "on_off_trigger_decide.h"
#include "pid_fuzzy_confidence.h"

/* ---- shared log tag ------------------------------------------------------ */
extern const char *PE_TAG;

/* ============================================================================
 * ROADMAP.md M15 "Mode-state sprawl" -- the legal-state table
 * ============================================================================
 * >=5 independent enums/booleans describe executor mode:
 *
 *   1. s_exec.state              profile_exec_state_t   (profile_executor.h)
 *                                 IDLE / RUNNING / PAUSED / DONE / FAULTED
 *   2. s_exec.dwelling            bool                   (this file)
 *   3. s_exec.ramp_lock_held      bool                   (this file)
 *   4. autotune per-zone state    autotune_engine_state_t (autotune_engine.h)
 *                                 IDLE / SETTLING / STEPPING / RELAY_APPROACH /
 *                                 RELAY_CYCLING / DONE / ABORTED, plus
 *                                 `no_setpoint` derived from `method` (STEP
 *                                 has none, RELAY has relay_setpoint_c)
 *   5. per-zone flags             zone_runtime_t          (this file)
 *                                 active, faulted, heat_blocked,
 *                                 cooling_limited, lag_sustained
 *
 * A forced single enum was REJECTED (some exclusions below are load-bearing:
 * e.g. dwelling surviving a PAUSE is what makes resume-mid-dwell correct, so
 * collapsing dwelling into s_exec.state would either lose that information or
 * multiply the state count instead of shrinking it). Two bug classes have
 * already come from illegal-but-representable combinations of these fields
 * slipping past review (project_autotune_feeds_fake_setpoint.md,
 * project_dwell_credit_unreachable.md) -- this table is the one place a
 * future feature that adds another mode flag has to read and update before
 * shipping. exec_mode_state_check() below is a runtime assertion of the
 * ILLEGAL rows; nothing checks the LEGAL rows are reachable (a table entry
 * marked "legal" is a claim about the code's own invariants, not a promise
 * that a particular combination in it is ever hit in practice).
 *
 * ---- LEGAL combinations (non-exhaustive; the ones that recur across this
 * ---- module's own comments, called out because a first reading would
 * ---- guess them illegal) --------------------------------------------------
 *
 *  state=RUNNING, dwelling=false, ramp_lock_held=true
 *      "ramp-lock stall without dwelling" -- a lagging zone freezes
 *      target_c/segment_elapsed_s (sec 7.1) *before* the segment can ever
 *      reach seg->target_c and flip dwelling true. The stall is what keeps
 *      the ramp segment from silently completing on a zone that never
 *      climbed. Loses legality only once every zone is back in-band and the
 *      segment-stepping block's own reached-target check fires.
 *
 *  state=RUNNING, dwelling=true, ramp_lock_held=true
 *      A zone can start lagging again mid-dwell (thermal mass sagging back
 *      below EXEC_RAMP_LOCK_BAND_C after the ramp step ended) -- ramp_lock_
 *      held is recomputed every tick off the CURRENT lock_ok, independent of
 *      dwelling; nothing in profile_executor.c clears it on the dwelling
 *      transition. Cosmetically odd (the schedule isn't advancing either
 *      way during a dwell) but not double-counted: sec 7.2 auto-stretch and
 *      sec 7.3 dwell-credit's "ramping_now"/"credit_ramping_now" gates both
 *      key off dwelling/seg_kind, never off ramp_lock_held directly, so this
 *      combination changes nothing about what the tick actually does.
 *
 *  state=PAUSED, dwelling=true (or ramp_lock_held=true)
 *      profile_executor_pause() freezes the shared schedule without
 *      resetting either field (profile_executor_status.c) -- a firing
 *      paused mid-dwell resumes mid-dwell, not at the dwell's start. This is
 *      the specific exclusion the rejected single-enum design would have
 *      had to either lose or re-encode as its own extra state.
 *
 *  autotune state=SETTLING, no_setpoint=true
 *      SETTLING only occurs on the STEP method (RELAY has no SETTLING
 *      phase, see autotune_engine_state_t), and no_setpoint is `method !=
 *      AUTOTUNE_METHOD_RELAY` (autotune_engine.c) -- STEP has no real
 *      setpoint at ANY of its states, so this pairing holds for the whole
 *      SETTLING/STEPPING lifetime of a step run, not just as a momentary
 *      transition value. It exists so thermal_guard.c's guard 4 (drift-at-
 *      setpoint) does not fire against a setpoint that was never real.
 *
 *  zone: active=true, faulted=false, heat_blocked=true
 *      relay_authority_zone_blocked()/a safety-link outage can block a
 *      healthy, still-participating zone; z->duty is still computed and
 *      reported (profile_executor_guard_commanded_duty() zeroes only the
 *      value FED TO THE GUARDS, not z->duty itself) so the dashboard shows
 *      what the zone WANTS while heat_blocked explains why it isn't getting
 *      it. Distinct from faulted, which is this zone's OWN guard latch.
 *
 *  zone: control_mode=PID, cooling_limited=true
 *      Legal, and the only mode this can ever be true in (see the next
 *      table row) -- duty has read 0 for >= the debounce hold while still
 *      PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C above target: normal for a
 *      kiln coming down off a big overshoot, nothing to escalate.
 *
 * ---- ILLEGAL combinations (what exec_mode_state_check() asserts against)--
 *
 *  1. zone active in a RUNNING/PAUSED profile run AND that SAME zone's
 *     autotune state is one of {SETTLING, STEPPING, RELAY_APPROACH,
 *     RELAY_CYCLING} (i.e. autotune is actively driving heat on it).
 *     Reason: two independent control loops would be writing duty/relay
 *     commands to the same zone. profile_executor_run() refuses to start
 *     against an actively-autotuning zone and autotune_engine.c refuses to
 *     start against a profile_executor_zone_is_active() zone (profile_
 *     executor.h's own doc comment on profile_executor_run(), "zones must
 *     be autotuned one at a time" -- enforced from both directions). DONE/
 *     ABORTED/IDLE are not "actively driving" (DONE holds the last recorded
 *     duty only until accept/abort; IDLE/ABORTED command nothing) so those
 *     three do NOT trip this rule even against an active zone.
 *
 *  2. control_mode != ZONE_CONTROL_MODE_PID (and != PID_FUZZY, which shares
 *     the PID tick body) AND cooling_limited == true.
 *     Reason: cooling_limited is explicitly documented as "a PID-mode-only
 *     diagnostic" (zone_runtime_t.cooling_limited's own comment) and both
 *     the BANGBANG and OFF branches of the control-mode switch unconditionally
 *     zero cooling_limited_hold_s/cooling_limited every tick they run --
 *     seeing it true outside PID/PID_FUZZY means a mode switch failed to
 *     clear stale state from before the switch.
 *
 *  3. zone faulted == true AND (heater relay actually commanded on this
 *     tick, i.e. z->relay_commanded_on == true after apply_relay()).
 *     Reason: a faulted zone's guard latch exists specifically to stop
 *     commanding heat to it; apply_relay()/force_zone_relay_off() are the
 *     only writers of relay_commanded_on and every faulted-zone path in
 *     profile_executor.c skips the zone (`if (!active || faulted) continue`)
 *     before ever reaching the switch that could set want_relay_on true.
 *
 *  4. s_exec.state == PROFILE_EXEC_IDLE AND any zone active == true.
 *     Reason: `active` means "in the current run's zone_mask"
 *     (zone_runtime_t.active's own comment) -- profile_executor_run()
 *     (profile_executor_run.c, the zone_mask loop that sets z->active = true)
 *     is the only writer that ever sets it true, and it always also sets
 *     state to RUNNING in the same locked section. **Correction, audit
 *     2026-09-24 (docs/audits/profile_executor_panic_2026-09-24.md):** despite
 *     this rule's original claim, no DONE/FAULTED/halt path actually clears
 *     a zone's `active` back to false -- grep finds no writer of `active =
 *     false` on a zone_runtime_t anywhere in this module, so after
 *     profile_executor_halt() sets IDLE every zone of the halted run still
 *     reads active == true -- the data this rule describes as illegal does
 *     exist, until the next run. It cannot reach the assert: the one
 *     asserting call site (the control tick) is reached only on a tick that
 *     found state == RUNNING at the top (every other state `continue`s
 *     first), and no code inside that locked tick body writes IDLE (the only
 *     IDLE writers are profile_executor_start() and halt(), each under its
 *     own s_exec.lock take). profile_executor_run() then memset()s
 *     s_exec.zones before setting the new run's zone_mask bits, so no stale
 *     `active` survives into the next RUNNING tick. Known gap, left for a
 *     follow-up: rule 4 is unenforced data hygiene today, not a live assert
 *     hazard.
 *
 *  5. s_exec.dwelling == true AND s_exec.state NOT IN {RUNNING, PAUSED}.
 *     Reason: dwelling is only ever set true inside the RUNNING control
 *     loop's segment-stepping block and only ever read back (never reset)
 *     across a PAUSED interval -- see the LEGAL row above. **Correction,
 *     audit 2026-09-24:** this rule's original text claimed "every path that
 *     leaves RUNNING/PAUSED for IDLE/DONE/FAULTED resets the whole
 *     s_exec_state_t including dwelling" -- that was false. Before the same
 *     audit's fix, `dwelling` was cleared only at profile_executor_run()'s
 *     own start (profile_executor_run.c:412) and at a segment advance
 *     (profile_executor.c, the ZONE_RAMP dwell-done branch); every FAULTED/
 *     DONE transition inside the RUNNING tick (escalate_guard_trip()'s three
 *     branches in profile_executor_relay_io.c, the watchdog's guard-9 FAULT
 *     action, and the RELAY_IO/ZONE_RAMP DONE branches in profile_executor.c)
 *     set state directly and left dwelling untouched, which is exactly the
 *     sequence that reached this rule's assert on 2026-09-24 (a guard tripped
 *     mid-dwell, FAULTED was set, dwelling stayed true, the same tick's
 *     exec_mode_state_check() call caught it). All six sites now go through
 *     exec_enter_terminal_state() (profile_executor_relay_io.c), the one
 *     function that sets state AND clears dwelling/ramp_lock_held together;
 *     any future FAULTED/DONE transition must use it too, not assign
 *     s_exec.state directly, or this rule's guarantee breaks again. Of the
 *     six, only escalate_guard_trip()'s three could actually reach the
 *     assert (they run inside the RUNNING tick, before its check); the two
 *     DONE branches `continue` before the check and the watchdog's FAULT
 *     runs in another task, so those were stale data, not a panic path.
 *     profile_executor_halt()'s IDLE transition still leaves dwelling as it
 *     was -- unreachable by the assert for the same reason rule 4's gap is
 *     (see above), and get_status() does not report dwelling while IDLE.
 *
 *  6. autotune per-zone state has no_setpoint == true AND method ==
 *     AUTOTUNE_METHOD_RELAY.
 *     Reason: no_setpoint is defined as `method != AUTOTUNE_METHOD_RELAY`
 *     (autotune_engine.c's `.no_setpoint = (s_at.method != AUTOTUNE_METHOD_
 *     RELAY)`) -- RELAY always has a genuine relay_setpoint_c to check drift
 *     against (see the comment immediately above that assignment). The two
 *     fields are a single boolean's worth of information written from one
 *     expression; seeing them disagree means a caller constructed a
 *     thermal_guard_input_t by hand instead of through that one call site.
 *
 * Rules 1/4/5 are the ones a host test drives directly (s_exec is plain,
 * host-writable struct state); rules 2/3/6 are exercised the same way by
 * writing the same fields the real control loop would have written, since
 * exec_mode_state_check() only ever reads state, never re-derives it from
 * I/O. See exec_mode_state_check()'s own doc comment (near the bottom of
 * this file) for the exact violation-string wording each rule produces. */

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

    /* ADAPTIVE_FUZZY_EVALUATION_PLAN.md sec 3, N3: the in-firing error
     * zero-crossing oscillation backstop's per-zone state. Reset at every
     * firing start (profile_executor_run.c, alongside pid_reset()/
     * fuzzy_prev_effective_ki above) -- NOT persisted across firings, unlike
     * the confidence counter (adaptive_tune_zone_t::fuzzy_confidence_c),
     * because a limit cycle is a property of THIS firing's control loop, not
     * a durable statement about the plant model. See pid_fuzzy_confidence.h
     * for the module this drives. */
    pid_fuzzy_oscillation_state_t fuzzy_osc;

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
    /* ROADMAP.md M15 B4: the rest of the duty pipeline -- see
     * zone_duty_breakdown_t's doc comment (profile_executor.h) for the
     * field list and profile_executor_feedforward.c/
     * profile_executor_pid_tick.c for the write sites. Copied verbatim into
     * profile_exec_zone_status_t by profile_executor_get_status() alongside
     * last_pid_terms's own fields. */
    zone_duty_breakdown_t duty_breakdown;

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

    /* PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md
     * option (b): this zone's OWN commanded setpoint, rate-limited toward
     * the shared s_exec.target_c by zone_cfg_t::approach_rate_cap_c_per_hr
     * (0 = uncapped). Computed once per tick, in profile_executor.c, right
     * before the per-zone control-mode pass -- see that update site's own
     * comment for the exact clamp arithmetic. This is the value the
     * feedforward/PID terms (profile_executor_pid_tick.c) and thermal_
     * guard_input_t.setpoint_c are fed FROM, instead of s_exec.target_c
     * directly, so a capped zone's guard sees what it is actually being
     * asked to do this tick rather than the group's eventual destination
     * (PER_ZONE_TARGET_DESIGN_STUDY.md section 2.4's "fake setpoint" bug
     * class, project_autotune_feeds_fake_setpoint.md).
     *
     * UNCAPPED (the default, and every zone before this field existed):
     * effective_target_c is set to s_exec.target_c on every tick with no
     * rate limit at all, so it is always EXACTLY s_exec.target_c -- every
     * consumer of this field behaves bit-for-bit as if it still read
     * s_exec.target_c directly.
     *
     * Deliberately NOT consulted by ramp-lock (profile_executor.c's
     * lock_ok loop, still actual_c vs the shared s_exec.target_c) or
     * segment-advance (still s_exec.target_c == seg->target_c) -- see this
     * field's own zone_cfg_t::approach_rate_cap_c_per_hr comment for the
     * full list of what this option deliberately leaves untouched.
     *
     * NOT actually NAN in practice: profile_executor_run.c:728 seeds this
     * to baseline_target_c (a real segment/warm-start temperature, never
     * NAN) before the zone ever ticks, and this struct's own memset default
     * is a finite 0.0f -- nothing in this codebase ever writes NAN here, so
     * the `!isfinite(effective_target_c)` snap-from-unseeded branch in
     * profile_executor.c (~line 863) is unreachable in current builds. It
     * is kept as a defensive fallback (a future caller that skips the
     * run.c seed step would otherwise climb a freshly started, capped zone
     * from 0 degC before the cap could engage usefully), not because NAN is
     * an actual sentinel value produced anywhere today. */
    float effective_target_c;

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

    /* docs/ON_OFF_ZONE_PLAN.md sec 3/4 -- on_off_trigger_decide.h's
     * feature-local per-zone state (quasi_dwell timers, commanded_on, hold
     * timer). Deliberately NOT read by, or derived from, any other field in
     * this struct (see that header's top comment on why quasi_dwell must
     * stay isolated). Meaningful only for a ZONE_TYPE_ON_OFF zone; a HEATER
     * zone's copy is reset at run start and never ticked. Report-only for
     * now (plan step 7): profile_executor.c computes a verdict here every
     * tick but does not act on it -- no relay is written from this field or
     * from anything derived from it. */
    on_off_trigger_state_t on_off_trigger_state;

    /* Plan step 8 (docs/ON_OFF_ZONE_PLAN.md sec 3/6): ACTUATION-layer
     * min_on_s/min_off_s enforcement, deliberately separate state from
     * on_off_trigger_state.commanded_on/held_s above. Requirement: "a
     * decision-core bug cannot chatter a physical relay" -- if
     * on_off_trigger_decide() ever mis-evaluates the hold (a bug in that
     * module), this second, independent timer at the point that actually
     * calls apply_relay() still bounds the physical relay's switching rate.
     * Bypassed only for a failsafe/guard-5-6/run-not-RUNNING transition
     * (on_off_apply_gate()'s own doc comment) -- safety is never delayed by
     * a hold timer, same rule the decision core itself follows. Reset at
     * run start / resume alongside on_off_trigger_state (profile_executor_
     * run.c) -- never merely on a segment change. */
    bool  on_off_actuated_on;
    float on_off_actuated_held_s;

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
    /* docs/audits/iter_tune_decision_2026-09-07.md: the one field iter_tune.h's
     * INTEGRATION POINT comment calls out as "not currently captured
     * anywhere" -- this zone's own actual_c at its first accumulated
     * (actual_valid) tick of the run, i.e. the same instant fs_sample_count
     * goes 0->1 (see firing_stats_zone_tick()'s write site). Zeroed by
     * profile_executor_run()'s memset(s_exec.zones, ...) like every other
     * fs_* field above -- a zone that never accumulates a valid sample this
     * run (all-excluded or zero-duration) reports 0.0f here, same
     * degenerate-default convention firing_stats_snapshot() already uses
     * for a never-ticked span. Prep-only: nothing reads this yet
     * (iter_tune_process_firing() is deliberately not wired in by this
     * change -- see the audit doc's recommendation). */
    float    fs_start_temp_c;
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

    /* live_profile_generation() as of the last time this run polled the
     * live-edit working slot (docs/LIVE_PROFILE_EDIT_PLAN.md pass 1, section
     * 3) -- same one-counter-per-tick poll shape as config_generation just
     * above, so an unedited run pays nothing beyond the comparison. Zeroed by
     * profile_executor_run() like the rest of this struct's per-run state;
     * an edit fork()ed before this run started is picked up on its very
     * first tick because live_profile_generation() already differs from 0. */
    uint32_t live_edit_generation;

    /* MEDIUM-3 (review): the last DEFINITIVE live-edit refusal this run has
     * seen -- window or HARD-validate (never "not applicable"/malloc/not-
     * running, none of which are definitive, see profile_live_pickup_
     * should_advance_generation()'s doc comment). Pass 1 has no HTTP route
     * to surface this (a bare ESP_LOGW was the only trace before this
     * fix); pass 2's planned GET /api/profile/live is expected to read it.
     * Cleared back to !valid at the START of every run (profile_executor_
     * run(), matching every other per-run field in this struct) so a stale
     * refusal from a PREVIOUS firing can never be reported against this
     * one. */
    struct {
        bool     valid;
        uint32_t generation;              /* the live_profile_generation() value refused */
        profile_live_pickup_result_t result;
        char     err_msg[128];
    } live_edit_last_refusal;

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

/* VERSIONLESS HAZARD (docs/audits/firing_history_blob_versioning_2026-09-07.md):
 * unlike zones_cfg_t (ZONES_CFG_VERSION + per-version decode in
 * zones_config_json.c/zones_config_store.c), this blob carries no version
 * byte, no CRC, and no migration path. firing_stats_load()'s only defence
 * is a raw hal_kv_get_blob() length check (`len != sizeof(*out)`) -- any
 * field added, removed, reordered, or resized in
 * profile_exec_firing_stats_t / profile_firing_zone_record_t /
 * profile_firing_run_record_t changes sizeof(profile_firing_history_blob_t)
 * and makes every existing on-flash "fs_<id>" key fail that check. That is
 * NOT a loud rejection: firing_stats_load() logs one ESP_LOGW and returns an
 * all-zero blob, and firing_stats_persist() calls it ignoring the bool
 * return ("empty blob on any failure -- still safe to prepend into"), so a
 * layout change silently DISCARDS every profile's persisted firing-history
 * ring on the next firing after a flash, with no operator-visible failure
 * beyond a log line. These asserts pin today's layout byte-for-byte so a
 * future edit here is a deliberate, visible build break instead of a silent
 * on-flash format change -- see the doc above for the extensibility options
 * this does NOT itself decide between. Values captured 2026-09-07 with a
 * standalone MSVC layout replica (same discipline as zone_cfg_v21_t's own
 * frozen-snapshot asserts in zones_config_json.h), not guessed. */
_Static_assert(sizeof(profile_exec_firing_stats_t) == 64,
               "profile_exec_firing_stats_t changed size -- see the VERSIONLESS HAZARD comment above");
_Static_assert(sizeof(profile_firing_zone_record_t) == 80,
               "profile_firing_zone_record_t changed size -- see the VERSIONLESS HAZARD comment above");
_Static_assert(sizeof(profile_firing_run_record_t) == 272,
               "profile_firing_run_record_t changed size -- see the VERSIONLESS HAZARD comment above");
_Static_assert(sizeof(profile_firing_history_blob_t) == 1364,
               "profile_firing_history_blob_t changed size -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, profile_id) == 0,
               "profile_firing_run_record_t::profile_id moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, profile_name) == 1,
               "profile_firing_run_record_t::profile_name moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, run_started_unix_s) == 20,
               "profile_firing_run_record_t::run_started_unix_s moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, duration_s) == 24,
               "profile_firing_run_record_t::duration_s moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, zone_mask) == 28,
               "profile_firing_run_record_t::zone_mask moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_run_record_t, zones) == 32,
               "profile_firing_run_record_t::zones moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_history_blob_t, count) == 0,
               "profile_firing_history_blob_t::count moved -- see the VERSIONLESS HAZARD comment above");
_Static_assert(offsetof(profile_firing_history_blob_t, runs) == 4,
               "profile_firing_history_blob_t::runs moved -- see the VERSIONLESS HAZARD comment above");

/* Tail-append migration hook (docs/audits/firing_history_blob_versioning_
 * 2026-09-07.md, option (b)): the ONE prior on-disk size firing_stats_load()
 * knows how to migrate forward, by zero-filling the tail rather than
 * discarding. Today this equals sizeof(profile_firing_history_blob_t)
 * exactly -- there is only one layout that has ever shipped, so the
 * migration path is unreachable right now, which is the point: this pass
 * adds the mechanism with zero behavioural change. The NEXT field addition
 * (e.g. start_temp_c, per this doc's section 1) must leave this constant at
 * 1364 while sizeof(profile_firing_history_blob_t) grows -- that is what
 * makes an old-size on-disk blob recognizable as "version 1" instead of
 * being silently discarded by firing_stats_load(). If a SECOND size-changing
 * edit lands before the first is flashed everywhere, this single-prior-
 * version scheme becomes ambiguous (see the audit's (b) risk paragraph) --
 * that is the point at which a real version byte (option (a)) is due, not a
 * second frozen constant here. */
#define PROFILE_FIRING_HISTORY_BLOB_SIZE_V1 1364u

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
float zone_taper_climb_rate(const zone_runtime_t *z, uint8_t zi, float target_c, float rate_c_per_s,
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

/* Last-run-started RAM cache (PROFILE_SLOTS_100_PLAN.md review LOW, "list
 * perf") -- see profile_executor_firing_stats.c's own section comment for
 * the full design. firing_stats_persist()/firing_stats_erase() (this same
 * file) are the only writers; profile_executor_status.c's
 * profile_executor_last_run_started_unix_s() is the only reader, consulting
 * the cache first and calling firing_stats_cache_store() itself on a miss
 * after doing the real (uncached) firing_stats_load()-based lookup. */
bool firing_stats_cache_lookup(uint8_t profile_id, uint32_t *out_started_unix_s);
void firing_stats_cache_store(uint8_t profile_id, uint32_t started_unix_s);

/* ---- relay/IO segment machinery + guard escalation
 * (profile_executor_relay_io.c) --------------------------------------------- */
void apply_relay(uint8_t zi, bool want_on);
float profile_executor_guard_commanded_duty(bool heat_blocked, float intended_duty);
float profile_executor_guard_sanity_rate(float configured_rate_c_per_min, float target_rate_c_per_s);
/* This zone's OWN commanded setpoint rate, to be passed to profile_executor_
 * guard_sanity_rate() in place of the shared s_exec.target_rate_c_per_s --
 * see that function's definition (profile_executor_relay_io.c) for why the
 * per-zone approach-rate cap made the shared rate the wrong input, and why
 * an uncapped zone (cap_c_per_hr == 0) is bit-identical to before. */
float profile_executor_guard_zone_ramp_rate(float shared_rate_c_per_s, float cap_c_per_hr,
                                            bool still_approaching);
/* Plan step 8 -- ACTUATION-layer min_on_s/min_off_s enforcement for an
 * on/off zone, independent of on_off_trigger_decide()'s own hold timer (see
 * on_off_actuated_on/on_off_actuated_held_s's doc comment, zone_runtime_t).
 * Pure scalar function, same shape as this file's other small decision
 * helpers, so a host test can drive it directly with no zone_runtime_t/
 * s_exec in scope.
 *
 * *actuated_on / *held_s are the caller's persistent actuation-layer state,
 * mutated in place. decided_on is on_off_trigger_decide()'s verdict for this
 * tick. bypass_hold must be true exactly when the decision was driven by a
 * safety-relevant precedence level (fail-safe override, guard 5/6 trip, or
 * run not RUNNING) -- plan sec 3: the hold "sits... below the safety levels
 * so safety is never delayed by it," and this second, independent gate must
 * honor the same rule or it would reintroduce the very chatter/delay this
 * layer exists to bound out of the level it is supposed to be defending.
 * min_on_s/min_off_s of 0 hold nothing (matches on_off_trigger_decide()'s
 * own 0-means-not-configured convention -- the caller substitutes the plan's
 * 30s default before calling, same as it does for that module).
 *
 * Returns the actuated value (== *actuated_on after the call). */
bool profile_executor_on_off_actuation_gate(bool *actuated_on, float *held_s, bool decided_on,
                                            bool bypass_hold, uint16_t min_on_s, uint16_t min_off_s,
                                            float dt_s);

/* docs/ON_OFF_ZONE_PLAN.md sec 6: whether an on/off zone's already-gated ON
 * verdict must be suppressed by max_simultaneous_relays THIS tick.
 * relays_on_count is how many relays the tick has already committed to ON
 * before this zone is considered -- the caller seeds it from the (already
 * cap-adjusted) heater decisions and grows it as on/off zones are granted,
 * which is what makes on/off zones the cap's last, unclaimed slots: a
 * heater's own claim was already final by the time any on/off zone is
 * looked at. cap == 0 is unlimited (never denies). Pure predicate, no
 * side effect, no deferral bookkeeping -- plan sec 6: "their denial is
 * logged rather than deferred." */
bool profile_executor_on_off_cap_denies(uint8_t relays_on_count, uint8_t cap);

/* docs/ON_OFF_ZONE_PLAN.md sec 3/4/6/8 -- the ENTIRE per-zone, per-tick
 * on/off decision chain as one production function: on_off_trigger_decide()
 * (precedence, hysteresis, quasi-dwell) -> profile_executor_on_off_
 * actuation_gate() (independent actuation-layer min_on_s/min_off_s hold,
 * requirement 4) -> profile_executor_on_off_cap_denies() (max_simultaneous_
 * relays, on/off zones suppressed last). Factored out of profile_executor.c's
 * tick loop for the same reason profile_executor_guard_commanded_duty() and
 * this file's other small decision helpers were: direct host-test call-
 * ability against the REAL production chain, not a hand-written mirror of
 * it -- the tick loop's own "no seam to call just one tick" limitation
 * (test_ramp_lock_onesided.c's header comment) applies to the FreeRTOS task
 * loop's shape, not to what runs inside one zone's iteration of it, and
 * everything genuinely decision-shaped for an on/off zone now lives here
 * instead of inline in that loop.
 *
 * bypass_hold must be true exactly when *in describes a safety-relevant
 * transition (fail-safe override / guard 5-6 trip / run not RUNNING) --
 * same value on_off_trigger_decide() itself uses internally to decide
 * whether ITS OWN hold applies; the caller computes it once and passes it
 * to both, which is why it is a parameter here rather than re-derived.
 * relays_on_count/cap are this tick's already-tallied "how many relays are
 * ON before this zone is considered" and the configured cap (0=unlimited).
 *
 * Mutates *decide_state (on_off_trigger_decide()'s own state) and
 * *actuated_on / *actuated_held_s (the actuation-layer hold state) in place,
 * exactly as the two functions it calls would if invoked separately. */
typedef struct {
    bool actuated_on;  /* final verdict this tick -- what apply_relay() should be called with */
    bool cap_denied;   /* true if the cap suppressed an otherwise-ON verdict (for the caller's log line) */
} on_off_zone_tick_result_t;

on_off_zone_tick_result_t profile_executor_on_off_zone_tick(
    on_off_trigger_state_t *decide_state, bool *actuated_on, float *actuated_held_s,
    const on_off_trigger_input_t *in, bool bypass_hold, uint8_t relays_on_count, uint8_t cap);
/* UART trace for the on/off-zone bench-readiness decision -- see the
 * definition in profile_executor_relay_io.c for the full volume budget and
 * edge-trigger reasoning, and docs/audits/on_off_zone_bench_readiness_
 * 2026-09-08.md for the reading guide. */
void profile_executor_on_off_log_transition(uint8_t zi, const on_off_trigger_input_t *in,
                                             bool prev_decided_on, bool decided_on,
                                             bool prev_actuated_on, bool actuated_on,
                                             float held_prior_s, float held_s,
                                             uint16_t min_on_s, uint16_t min_off_s, bool bypass_hold,
                                             bool cap_denied);
void force_zone_relay_off(uint8_t zi);
void force_all_relays_off(void);
void release_profile_relay_claim(void);
bool relay_io_target_is_zone_owned(uint8_t relay_1_4, uint8_t *out_zone_index);
void io_seg_start(uint8_t idx, const profile_segment_t *seg);
void io_seg_finish(uint8_t idx, bool honor_leave_on);
void io_segs_force_all_off(bool honor_leave_on);
void io_segs_tick(float dt_s);
/* Sets s_exec.state and clears the per-run scratch flags (dwelling,
 * ramp_lock_held) that only mean anything inside RUNNING/PAUSED -- see the
 * definition in profile_executor_relay_io.c for the full rationale. Use this
 * at every transition to FAULTED or DONE instead of assigning s_exec.state
 * directly; the caller still owns fault_reason/fault_guard/relay-off/claim-
 * release, which vary per site. Must be called with s_exec.lock held. */
void exec_enter_terminal_state(profile_exec_state_t st);
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

/* PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option
 * (b), defined in profile_executor_pid_tick.c: resolves this zone's own
 * commanded setpoint for this tick -- s_exec.target_c when uncapped
 * (approach_rate_cap_c_per_hr reads 0, the default), z->effective_target_c
 * when capped. See that definition's own doc comment for why this
 * re-checks the cap itself rather than trusting z->effective_target_c
 * unconditionally (every existing host test that builds a zone_runtime_t by
 * hand and calls pid_family_zone_tick()/zone_feedforward() directly, never
 * running profile_executor.c's own per-tick cap-update loop, still gets
 * exactly today's behaviour with no test changes needed). Used by
 * profile_executor.c's BANGBANG branch and thermal_guard_input_t.setpoint_c
 * feed, and internally by pid_family_zone_tick()/pid_fuzzy_prepare_gains(). */
float zone_commanded_setpoint_c(const zone_runtime_t *z, uint8_t zi);
/* harvest_freeze (docs/audits/simc_sole_gain_writer_2026-09-14.md, Option B):
 * true while THIS zone is inside a dwell whose duty/rise observation
 * adaptive_tune.c may harvest for its K_dc fit -- forces strength_pct to 0
 * for the tick (the same bit-for-bit-base-gains path strength_pct==0/no-
 * model already take), so fuzzy cannot bias WHICH dwells settle cleanly
 * enough to be harvested (frame-independence is pointwise, not sample-wise
 * -- see that audit doc's section 1.3). Caller-computed (s_exec.dwelling &&
 * adaptive_tune_get_enabled(zi), profile_executor.c) rather than looked up
 * in here, same "no globals reached into" discipline pid_fuzzy.c's own
 * header comment states for this function's sibling. Fuzzy stays fully
 * active on ramps/approaches and on any zone that is not opted into
 * adaptive tuning -- this is a freeze of the harvest window only, not of
 * the firing. */
void pid_fuzzy_prepare_gains(zone_runtime_t *z, uint8_t zi, bool harvest_freeze, float dt_s, pid_cfg_t *out_cfg);

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

/* docs/ON_OFF_ZONE_PLAN.md plan step 5, sec 3 -- looks up the stored
 * profile_on_off_rule_t (if any) for (zone_index, segment_index) in `p` and
 * translates it into on_off_trigger_decide.h's on_off_trigger_rule_t, the
 * exact shape that module's `.rule` input field expects. Pure function
 * (profile_t in, verdict-shaped struct out; no s_exec, no I/O) so it is
 * host-test-callable directly against a hand-built profile_t, same as
 * profile_zones_have_ceiling()/profile_executor_plan_warm_start() above --
 * profile_executor.c's per-tick wiring (control/profile_executor.c) is the
 * only production caller.
 *
 * No match, or a match with enable == 0 (an author disabled a rule without
 * deleting it), both return a rule with .enable == false -- indistinguishable
 * from "no rule for this segment" (plan sec 3 precedence level 6), which is
 * intentional: a disabled rule and an absent one command the identical
 * verdict. At most one stored rule is expected to match a given
 * (segment_index, zone_index) pair -- profiles_http.c's
 * validate_on_off_rules() enforces this is the only way a profile can be
 * saved -- so the first match found is returned without scanning for a
 * (structurally impossible, if validation held) second one.
 *
 * temp_source == 2 ("named zone's TC") and == 3 ("executor setpoint") are
 * reserved encoding (profiles_types.h) not yet resolved by any caller as of
 * this step -- a rule using either is returned with temp_cmp forced to
 * ON_OFF_TEMP_CMP_NONE (the axis drops out of on_off_trigger_decide()'s AND
 * as a tautology) rather than evaluated against the wrong reading. Only
 * temp_source == 1 (this zone's own thermocouple, the only reading a
 * profile_t-only function can resolve without a live zone read) passes its
 * temp_cmp through. */
on_off_trigger_rule_t profile_resolve_on_off_rule(const profile_t *p, uint8_t zone_index, uint8_t segment_index);

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

/* ---- mode-state consistency check (ROADMAP.md M15 "Mode-state sprawl";
 * profile_executor.c) -- see this header's big table comment above for what
 * each rule means and why. Must be called with s_exec.lock already held
 * (same discipline as everything else that reads s_exec directly); it takes
 * no lock of its own and does no I/O.
 *
 * Checks every state combination the table marks ILLEGAL and returns the
 * count of violations found this call (0 = consistent) -- never aborts or
 * asserts itself, so a host test can call it directly against a hand-built
 * s_exec (including a deliberately illegal one, to prove the check has
 * teeth) without taking the whole test binary down. When
 * out_first_violation is non-NULL and at least one violation was found, it
 * is filled with a short, stable, human-readable description of the FIRST
 * one (rule order matches the table's numbering); every violation is also
 * ESP_LOGE'd unconditionally (host build and target alike), whether or not
 * the caller asserts on the return value.
 *
 * The ONE real control-loop call site (profile_executor.c's tick, end of
 * the locked section) additionally does:
 *     assert(exec_mode_state_check(buf, sizeof(buf)) == 0 && buf);
 * making that call site the actual "debug-buildable consistency assertion"
 * the ROADMAP item asks for. This repo has no existing runtime-assert
 * convention to plug into on target -- no `assert()`/`configASSERT()` call
 * site exists anywhere else in App/drivers, only compile-time
 * `_Static_assert` (verified by grep across App/drivers before writing
 * this) -- so plain libc `assert()` is used directly rather than inventing
 * a new macro. On the host-test build this is live (build_host_tests.ps1
 * defines no NDEBUG, so `assert()` aborts the test binary on a violation --
 * exactly what "assert it fires" means for host tests: they call
 * exec_mode_state_check() directly instead of going through the real tick,
 * so they observe the nonzero return/message with TEST_CHECK rather than
 * triggering that abort). On an ESP-IDF target build, whether `assert()`
 * compiles to a real abort depends on the Release optimization-assertion
 * Kconfig setting, same as every other libc `assert()` in this toolchain --
 * this function does not change or override that; if that setting resolves
 * to a no-op on this board's shipped config, this call site degrades to
 * "still ESP_LOGE'd, no board-side abort" for exactly the reason given in
 * the ROADMAP item: "if no convention exists, host-test-only is
 * acceptable." */
uint32_t exec_mode_state_check(char *out_first_violation, size_t out_cap);

#endif /* PROFILE_EXECUTOR_INTERNAL_H */
