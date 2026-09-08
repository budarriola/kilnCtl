// on_off_trigger_decide -- the pure decision core of docs/ON_OFF_ZONE_PLAN.md
// sections 3 (trigger model) and 4 (quasi-dwell), pulled out the same way
// link_watchdog_decide.h/.c pulls the PC-link watchdog's decision out of
// uart_bridge.c: no FreeRTOS, no kiln_io, no locks, no profile_executor.c
// globals -- a pure function of (state, cfg, input) -> verdict, so real
// vectors can drive it from a host test.
//
// SCOPE. This module decides ON/OFF for one on/off zone on one tick. It does
// NOT: read a thermocouple, evaluate a guard, walk s_exec, or write a relay
// -- profile_executor.c (a later plan step, #8) owns turning the verdict
// into an apply_relay() call. Nothing here is wired into the executor yet;
// this is intentionally dead code from the board's point of view until that
// step lands, exactly like plan steps 4/7 describe.
//
// QUASI_DWELL IS FEATURE-LOCAL. docs/ON_OFF_ZONE_PLAN.md sec 4 is explicit
// that `quasi_dwell` and its two timers must never be fed back into
// s_exec.dwelling, firing stats, or dwell credit -- this project has four
// documented "reset one side of a pair" bugs already (uart_bridge.c's
// msg_index, SimFW's s_ring_next_seq, fault_sched.c's s_seed,
// safety_link_frames.c's trip_last_seq) and feeding quasi_dwell into the
// executor's own dwell state would be a fifth: the executor would gain a
// second, independently-timed notion of "am I dwelling" with no single
// owning function keeping the two in sync. This module's state struct
// (on_off_trigger_state_t) is therefore private to one on/off zone's trigger
// evaluation and is never read by, or derived from, profile_executor's own
// s_exec.dwelling/segment_elapsed_s.
#ifndef ON_OFF_TRIGGER_DECIDE_H
#define ON_OFF_TRIGGER_DECIDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// docs/ON_OFF_ZONE_PLAN.md sec 4's two thresholds. 120 s entry = 4x
// EXEC_SUSTAINED_LAG_S (profile_executor_internal.h); 30 s exit is
// deliberately 4x FASTER than entry -- see that section's rationale for why
// the asymmetry is load-bearing, not a typo.
#define ON_OFF_QUASI_DWELL_ENTER_S 120.0f
#define ON_OFF_QUASI_DWELL_EXIT_S  30.0f

// Plan sec 3's phase/direction axes, bit masks so a rule can name more than
// one value ("RAMP or DWELL", "HEATING or COOLING") -- matches
// profile_on_off_rule_t.phase_mask/direction_mask's documented bit layout.
typedef enum {
    ON_OFF_PHASE_RAMP  = 1u << 0,
    ON_OFF_PHASE_DWELL = 1u << 1,
} on_off_phase_bit_t;

typedef enum {
    ON_OFF_DIR_HEATING = 1u << 0, /* target rising */
    ON_OFF_DIR_COOLING = 1u << 1, /* target falling */
    ON_OFF_DIR_FLAT    = 1u << 2, /* target unchanged tick-to-tick */
} on_off_direction_bit_t;

typedef enum {
    ON_OFF_TEMP_CMP_NONE  = 0, /* axis absent -- tautology, drops out of the AND */
    ON_OFF_TEMP_CMP_ABOVE = 1,
    ON_OFF_TEMP_CMP_BELOW = 2,
} on_off_temp_cmp_t;

// One (profile, segment, zone) rule -- plan sec 3's profile_on_off_rule_t,
// minus zone_index/segment_index (the caller already knows which zone/
// segment it is evaluating; those two fields exist in the plan's storage
// struct purely to key a sparse array and carry no meaning inside one
// tick's decision). temp_ref_zone resolution (temp_source == 2, "another
// zone's TC") is the caller's job -- by the time this struct is filled in,
// temp_measurement_c already IS whichever channel the rule names.
typedef struct {
    bool     enable;             /* false = no rule for this segment/zone -- precedence level 6 */
    uint8_t  phase_mask;         /* on_off_phase_bit_t bits the rule requires; 0 = any phase (tautology) */
    uint8_t  direction_mask;     /* on_off_direction_bit_t bits the rule requires; 0 = any direction */
    on_off_temp_cmp_t temp_cmp;  /* NONE = axis absent */
    float    temp_threshold_c;
    uint16_t time_start_s;       /* offset into the segment the rule becomes eligible */
    uint16_t time_stop_s;        /* 0 = to end of segment */
    bool     invert;             /* negate the AND of every configured axis */
} on_off_trigger_rule_t;

// Everything the decision needs for ONE tick of ONE on/off zone. Every
// field has a real production assignment site once this module is wired up
// (tracked by check_on_off_trigger_input_producers.ps1, added alongside
// this file) -- no field here may be left as a compound-literal implicit
// zero on the only path that matters.
typedef struct {
    // --- Precedence 1: fail-safe override (safety trip / FAULTED / halt /
    // abort / relay_authority_zone_blocked()) -- plan sec 3 level 1, sec 5's
    // fail-safe table. The caller ORs together every source that means
    // "this zone must go to its fail-safe state right now"; this module
    // does not need to know which one fired, only that one did.
    bool failsafe_override;
    bool failsafe_state_on;      /* zone_cfg_t.failsafe_state: false=OFF (default), true=ON */

    // --- Precedence 2: guard 5/6 trip on this zone (plan sec 3 level 2).
    bool guard_5_6_tripped;

    // --- Precedence 3: run state (plan sec 3 level 3).
    bool run_running;            /* PROFILE_EXEC_RUNNING */
    bool run_paused;             /* PROFILE_EXEC_PAUSED */
    bool failsafe_on_pause;      /* zone_cfg_t-derived: true = go fail-safe on PAUSE too */

    // --- Precedence 4: minimum on/off dwell (plan sec 3's hysteresis
    // section, "Minimum on/off time"). 0 substituted with the plan's 30 s
    // default by the caller, same "0 means not configured" convention
    // thermal_guard_cfg_t already uses -- this module does not itself
    // substitute a default, so a test can prove 0-means-off-holds behavior
    // explicitly rather than relying on an implicit default hiding it.
    uint16_t min_on_s;
    uint16_t min_off_s;

    // --- Precedence 5: the segment rule, plus the resolved per-tick facts
    // it is evaluated against (temperature hysteresis is applied HERE, not
    // inside the rule, because hysteresis needs the current commanded
    // state, which is state->commanded_on, not something a caller can
    // precompute once).
    on_off_trigger_rule_t rule;
    bool     current_phase_is_dwell;  /* effective_dwell = dwelling || quasi_dwell; false = ramp */
    uint8_t  current_direction;       /* one on_off_direction_bit_t value (exactly one bit set) */
    float    temp_measurement_c;      /* resolved reading for rule.temp_cmp, if not NONE */
    float    hyst_c;                  /* zone_cfg_t.hyst_c; 0 substituted with 2.0f default by the caller */
    float    segment_elapsed_s;       /* offset into the segment, for rule.time_start_s/time_stop_s */

    // --- Quasi-dwell inputs (plan sec 4). Read every tick regardless of
    // current_phase_is_dwell's source -- the caller computes
    // current_phase_is_dwell as (s_exec.dwelling || state->quasi_dwell)
    // using the SAME state->quasi_dwell this tick is about to (maybe)
    // update, i.e. quasi_dwell's classification always lags its own
    // trigger by exactly one tick, same as any other latch.
    bool    ramp_lock_held;           /* s_exec.ramp_lock_held */
    bool    stretched_this_tick;      /* sec 5's auto-stretch gate: ramp_lock_held && !stretched_this_tick */
    uint8_t segment_index;            /* for quasi_dwell's hard reset on segment change */

    float dt_s;
} on_off_trigger_input_t;

// Feature-local per-zone state, carried tick to tick. NEVER read by, or
// written into, profile_executor's own s_exec -- see this header's top
// comment. One instance per on/off zone, owned by whatever module ends up
// calling this (not yet decided -- plan step 8).
typedef struct {
    // Quasi-dwell classifier (plan sec 4). lock_true_s/lock_false_s are the
    // two continuous-duration timers the enter/exit thresholds compare
    // against; only one of them is ever nonzero at a time (see .c for why).
    bool  quasi_dwell;
    float lock_true_s;
    float lock_false_s;

    // Precedence-4 hold tracking: how long the CURRENT commanded_on value
    // has been held, and what it is. commanded_on is this module's own
    // belief about the relay -- the actual relay write (plan step 8) is a
    // separate concern.
    bool  commanded_on;
    float held_s;

    // For quasi_dwell's hard reset "on segment change" -- the only field in
    // this struct that must persist across a segment boundary in order to
    // DETECT one; everything else in the struct already resets at that
    // boundary via the reset itself.
    uint8_t last_segment_index;
    bool    have_last_segment_index; /* false until the first tick ever seen */
} on_off_trigger_state_t;

// Resets a state to its start-of-run value: quasi_dwell false, both timers
// zero, commanded_on false (fail-safe-shaped: a zone that has never ticked
// has never been commanded on), held_s zero, segment tracking cleared. Call
// at profile_executor_run() (a fresh firing) and at resume-after-power-cycle
// (plan sec 5: "Resume starts every on/off device in its fail-safe state and
// quasi_dwell = false") -- never merely on a segment change, which is
// on_off_trigger_decide()'s own job (partial reset, not this one).
void on_off_trigger_state_reset(on_off_trigger_state_t *state);

// Evaluates one tick for one on/off zone. Mutates *state (quasi_dwell
// timers/flag, commanded_on, held_s, last_segment_index) and returns the
// commanded relay state (true = ON) -- identical to what state->commanded_on
// now holds, returned directly so a caller that only wants the verdict does
// not need to reach into the state struct.
bool on_off_trigger_decide(on_off_trigger_state_t *state, const on_off_trigger_input_t *in);

#ifdef __cplusplus
}
#endif

#endif // ON_OFF_TRIGGER_DECIDE_H
