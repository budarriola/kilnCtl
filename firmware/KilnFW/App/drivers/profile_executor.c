#include "profile_executor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "autotune_engine.h"
#include "heat_enable.h"
#include "heater_output.h"
#include "kiln_io_owner.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- heat_interlock.h's own doc comment */
#include "pid.h"
#include "pid_fuzzy.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "safety_trip_words.h" /* safety_fault_source_words() -- ROADMAP.md M13, decode the
                                 * fault-source mask for the operator instead of a bare hex value */
#include "sim_backend.h"
#include "stack_margin.h" /* stack_margin_register() -- Opus review round 3, item 5: a real
                           * uxTaskGetStackHighWaterMark() reading for this task and its watchdog,
                           * readable over the link's GET_STACK_MARGIN (uart_bridge.c) without a
                           * debugger, before this fix's ~250-300B estimated peak stack addition
                           * (solve_hold_for_zone()/gauss_solve_partial_pivot()) is trusted on
                           * hardware. */
#include "thermo_combine.h"
#include "zone_coupling_solve.h"
#include "zones_http.h"

static const char *TAG = "profile_executor";

/* Pre-start warnings from the POLLED readers below, throttled to one line
 * each per boot.
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
#define LOG_PRESTART_ONCE(msg)                                                                       do {                                                                                                  static bool s_warned_once = false;                                                                if (!s_warned_once) {                                                                                 s_warned_once = true;                                                                             ESP_LOGW(TAG, msg " (further occurrences this boot are suppressed)");                         }                                                                                              } while (0)

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

/* Guard 9: control-tick liveness (TODO.md 6A.3/6A.7). A second, independent
 * task -- "a control loop cannot be its own watchdog" -- checks that the
 * control task's last-tick timestamp keeps moving and force-drops relays if
 * it doesn't. */
#define WATCHDOG_CHECK_PERIOD_MS 2000u
#define WATCHDOG_TICK_DEAD_MS 10000u

/* Packed history storage (see profile_executor.h's note on
 * profile_history_entry_t). 8 bytes instead of 20, which took this buffer
 * from 57.6 KB of .bss to 23 KB -- the difference between a board that can
 * start its tasks and serve HTTP and the one found on 2026-08-12 booting
 * with 7 KB of free heap.
 *
 * Resolution, and why each is enough for a *trend line over a firing*:
 *   elapsed  units of HISTORY_SAMPLE_PERIOD_S (30 s), u16 -> 22 days
 *   temps    0.1 degC, i16 -> +/-3276.7 degC, well past any thermocouple's
 *            range; HISTORY_TEMP_INVALID encodes NaN
 *   duty     whole percent, u8; 255 encodes "no value"
 * The graph this feeds is a multi-hour trend, not a scope trace. */
typedef struct {
    uint16_t elapsed_periods;
    int16_t  actual_dc;   /* deci-degC */
    int16_t  desired_dc;  /* deci-degC */
    uint8_t  duty_pct;
    uint8_t  guard;
} history_slot_t;

#define HISTORY_TEMP_INVALID INT16_MIN
#define HISTORY_DUTY_INVALID 0xFFu

static int16_t history_pack_temp(float c)
{
    if (isnan(c)) {
        return HISTORY_TEMP_INVALID;
    }
    float dc = c * 10.0f;
    if (dc > 32767.0f) dc = 32767.0f;
    if (dc < -32767.0f) dc = -32767.0f; /* -32768 is the NaN sentinel, don't collide with it */
    return (int16_t)(dc >= 0.0f ? dc + 0.5f : dc - 0.5f);
}

static float history_unpack_temp(int16_t dc)
{
    return (dc == HISTORY_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
}

static void history_pack(history_slot_t *slot, uint32_t elapsed_s, float actual_c, float desired_c, float duty,
                         uint8_t guard)
{
    uint32_t periods = elapsed_s / HISTORY_SAMPLE_PERIOD_S;
    slot->elapsed_periods = (periods > UINT16_MAX) ? UINT16_MAX : (uint16_t)periods;
    slot->actual_dc = history_pack_temp(actual_c);
    slot->desired_dc = history_pack_temp(desired_c);
    if (isnan(duty)) {
        slot->duty_pct = HISTORY_DUTY_INVALID;
    } else {
        float pct = duty * 100.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        slot->duty_pct = (uint8_t)(pct + 0.5f);
    }
    slot->guard = guard;
}

static void history_unpack(const history_slot_t *slot, profile_history_entry_t *out)
{
    out->elapsed_s = (uint32_t)slot->elapsed_periods * HISTORY_SAMPLE_PERIOD_S;
    out->actual_c = history_unpack_temp(slot->actual_dc);
    out->desired_c = history_unpack_temp(slot->desired_dc);
    out->duty = (slot->duty_pct == HISTORY_DUTY_INVALID) ? NAN : (float)slot->duty_pct / 100.0f;
    out->guard = slot->guard;
}

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

static s_exec_state_t s_exec;

/* ---- small helpers -------------------------------------------------------- */

static uint32_t ticks_to_s(TickType_t ticks)
{
    return (uint32_t)(ticks / configTICK_RATE_HZ);
}

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)ticks * (1000u / configTICK_RATE_HZ);
}

/* ---- feedforward from the identified plant model (TODO.md 6A.2) ------------
 *
 *   u_ff = (T_sp - T_ambient)/K_dc + (dT_sp/dt)*tau/K_dc
 *
 * The first term is the duty the kiln needs just to HOLD the current setpoint
 * against its own losses; the second is the extra needed to CLIMB at the
 * commanded rate (energy going into the thermal mass rather than out through
 * the walls). With a plant whose dead time is tens of seconds, feedback alone
 * always trails a ramp by a roughly fixed offset -- the integrator can only
 * build that offset by first being wrong for long enough. Feedforward supplies
 * it from the model instead, and leaves the PID correcting only the model's
 * error, which is what puts the actual curve ON the desired curve.
 *
 * It is an INPUT to pid_update_terms(), never a bypass of it: the sum is
 * clamped there along with P+I+D, and every relay it can lead to still goes
 * through heater_output, the load cap, the guards and
 * relay_authority_zone_blocked() unchanged. Nothing here can command heat that
 * the rest of the tick would have refused. */

/* Reads zone zi's model out of zone config and decides whether it can be used
 * at all. Returns true if the verdict CHANGED (so callers can log an operator-
 * visible transition). The validation is deliberately total rather than
 * defensive-in-places: K_dc appears in a denominator in both terms, so a zero,
 * negative or non-finite model must turn feedforward OFF for the zone, not
 * produce an infinity that the clamp downstream would render as 100% duty --
 * a plausible-looking number arrived at by dividing by nothing. A negative
 * K_dc is likewise rejected rather than used: it asserts that adding duty
 * cools the kiln, which means the fit is wrong, not that the kiln is strange.
 * Must be called with s_exec.lock held. */
static bool zone_load_model(uint8_t zi)
{
    zone_runtime_t *z = &s_exec.zones[zi];
    float k_dc = 0.0f, tau_s = 0.0f, dead_time_s = 0.0f;

    /* zones_http.h's contract: false means "cannot answer", and 0 in any of
     * the three outputs means "no model has been identified for this zone" --
     * the expected state of a zone that has never been autotuned, not an
     * error. Either way there is nothing to compute with. */
    bool have = zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s) &&
                isfinite(k_dc) && isfinite(tau_s) && isfinite(dead_time_s) &&
                k_dc > 0.0f && tau_s > 0.0f && dead_time_s > 0.0f;

    bool was_enabled = z->ff_enabled;
    float was_k = z->ff_k_dc, was_tau = z->ff_tau_s;

    z->ff_enabled = have;
    z->ff_k_dc = have ? k_dc : 0.0f;
    z->ff_tau_s = have ? tau_s : 0.0f;

    return (z->ff_enabled != was_enabled) || (z->ff_k_dc != was_k) || (z->ff_tau_s != was_tau);
}

/* Cross-zone coupling/feedforward math (Gaussian elimination, the coupled
 * steady-state hold solve, the neighbour-side low-pass filter tick, and the
 * neighbour-qualification predicate) now lives in zone_coupling_solve.c/.h --
 * split out for host-testability the same way dashboard_json.c and
 * zones_config_json.c were (see zone_coupling_solve.h's own doc comment for
 * why). The four thin wrappers below exist only because that module's
 * functions take plain scalars/a small zone_coupling_neighbor_t array
 * instead of this file's private zone_runtime_t* -- every call site below
 * this point keeps calling coupling_filter_tick()/
 * zone_qualifies_as_coupling_neighbor()/count_qualifying_coupling_neighbors()/
 * solve_hold_for_zone() by the same names and signatures as before the
 * split; only where these four are DEFINED changed. The cache and
 * membership-signature arrays (s_coupling_hold_cache[]/
 * s_coupling_prev_membership_sig[]) stay right here, unchanged, and are
 * threaded into zone_coupling_solve_hold() by reference to this zi's own
 * slot -- see that function's doc comment. */

static void coupling_filter_tick(zone_runtime_t *zn, bool actual_valid_now, float dt_s)
{
    zone_coupling_filter_tick(&zn->coupling_filtered_c, &zn->coupling_filter_init, zn->actual_c,
                              actual_valid_now, zn->pid_cfg.d_filter_tau_s, dt_s);
}

static bool zone_qualifies_as_coupling_neighbor(const zone_runtime_t *zn)
{
    return zone_coupling_qualifies_as_neighbor(zn->active, zn->actual_valid, zn->actual_c, zn->ff_enabled,
                                               zn->ff_k_dc, zn->control_mode, zn->faulted, zn->heat_blocked);
}

/* Builds the per-zone {qualifies, ff_k_dc} array zone_coupling_solve.c's
 * neighbour-facing functions take, straight from s_exec.zones[] -- every
 * OTHER zone's own fields are always read from s_exec.zones[], both before
 * and after this split (see solve_hold_for_zone()'s wrapper below for the
 * one exception: zi's OWN entry, which callers pass separately since `z` is
 * not guaranteed to BE s_exec.zones[zi]). Must be called with s_exec.lock
 * held (reads s_exec.zones[]). */
static void build_coupling_neighbor_array(zone_coupling_neighbor_t out[MAX31856_CHANNEL_COUNT])
{
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        out[j].qualifies = zone_qualifies_as_coupling_neighbor(&s_exec.zones[j]);
        out[j].ff_k_dc = s_exec.zones[j].ff_k_dc;
    }
}

static uint8_t count_qualifying_coupling_neighbors(uint8_t zi)
{
    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    build_coupling_neighbor_array(zones);
    return zone_coupling_count_qualifying_neighbors(zi, zones, MAX31856_CHANNEL_COUNT);
}

static zone_coupling_hold_cache_t s_coupling_hold_cache[MAX31856_CHANNEL_COUNT];
static uint16_t s_coupling_prev_membership_sig[MAX31856_CHANNEL_COUNT];

static float solve_hold_for_zone(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float ambient_c,
                                  bool *out_used_matrix, bool *out_infeasible,
                                  coupling_solve_reason_t *out_reason, bool *out_membership_changed)
{
    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    build_coupling_neighbor_array(zones);
    bool z_qualifies = zone_qualifies_as_coupling_neighbor(z);
    zone_coupling_hold_cache_t *cache_row = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_hold_cache[zi] : &s_coupling_hold_cache[0];
    uint16_t *prev_sig = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_prev_membership_sig[zi] : &s_coupling_prev_membership_sig[0];
    return zone_coupling_solve_hold(z_qualifies, z->ff_k_dc, zi, zones, MAX31856_CHANNEL_COUNT, setpoint_c,
                                    ambient_c, out_used_matrix, out_infeasible, out_reason,
                                    out_membership_changed, cache_row, prev_sig);
}


/* The feedforward duty for one zone, already clamped to [0,1]. Returns exactly
 * 0.0f -- i.e. the behaviour of every firing before this existed -- for a zone
 * with no usable model.
 *
 * The SUM is clamped, not each term: on a cooling ramp the second term is
 * legitimately negative and is supposed to reduce the hold duty below what a
 * steady hold would need. Clamping the terms separately would throw that away
 * and hold the kiln up through a controlled cool. */
static float zone_feedforward(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float rate_c_per_s)
{
    if (!z->ff_enabled) {
        return 0.0f;
    }
    bool hold_used_matrix = false, hold_infeasible = false, hold_membership_changed = false;
    coupling_solve_reason_t hold_reason = COUPLING_SOLVE_OK;
    float hold = solve_hold_for_zone(z, zi, setpoint_c, s_exec.ambient_c, &hold_used_matrix, &hold_infeasible,
                                     &hold_reason, &hold_membership_changed);
    float climb = (rate_c_per_s * z->ff_tau_s) / z->ff_k_dc;
    float u_ff = hold + climb;

    /* Diagnostics written to s_exec.zones[zi], not through `z` -- z is const
     * here, and (see solve_hold_for_zone()'s doc comment) is not guaranteed
     * to BE s_exec.zones[zi] in the first place, only its production-code
     * value. s_exec.zones[zi] is always the real per-zone runtime record
     * that profile_executor_get_status() reports off-board (Opus review,
     * blocker 3: wired through get_status()/dashboard_http.c's JSON, the
     * same path heat_blocked already uses -- this comment's claim is now
     * actually true, not aspirational). Logged once per TRANSITION only
     * (apply_relay()'s own heat_blocked logging uses the same discipline) --
     * this runs every tick. ff_membership_changed is read (and cleared by
     * being re-derived) every tick by pid_family_zone_tick() immediately
     * after this call, to decide whether to re-seed -- see that call site. */
    if (zi < MAX31856_CHANNEL_COUNT) {
        s_exec.zones[zi].ff_hold_used_matrix = hold_used_matrix;
        s_exec.zones[zi].ff_hold_infeasible = hold_infeasible;
        s_exec.zones[zi].ff_hold_reason = (uint8_t)hold_reason;
        s_exec.zones[zi].ff_membership_changed = hold_membership_changed;
        static bool s_fallback_active[MAX31856_CHANNEL_COUNT];
        static bool s_infeasible_active[MAX31856_CHANNEL_COUNT];
        bool fell_back = !hold_used_matrix;
        if (fell_back != s_fallback_active[zi]) {
            s_fallback_active[zi] = fell_back;
            if (fell_back) {
                ESP_LOGW(TAG, "zone %u coupled feedforward hold fell back to the legacy per-zone "
                              "diagonal (unqualified zone, no qualifying coupled neighbours, or a "
                              "singular/ill-conditioned coupling matrix)",
                         zi);
            } else {
                ESP_LOGI(TAG, "zone %u coupled feedforward hold resumed using the solved coupling matrix", zi);
            }
        }
        if (hold_infeasible != s_infeasible_active[zi]) {
            s_infeasible_active[zi] = hold_infeasible;
            if (hold_infeasible) {
                ESP_LOGW(TAG, "zone %u coupled feedforward solve required clamping a zone's duty to "
                              "[0,1] -- the commanded setpoint combination is not achievable as specified",
                         zi);
            } else {
                ESP_LOGI(TAG, "zone %u coupled feedforward solve back within [0,1] without clamping", zi);
            }
        }

        /* Opus review round 3, item 3: the membership-transition reseed
         * (pid_family_zone_tick(), on hold_membership_changed) makes a
         * chattering membership a SAFE failure mode (a sustained FF+P
         * offset while the integrator effectively stops correcting,
         * instead of a repeated 6-10 point duty step) but it is a silent
         * one -- heat_blocked alone is refreshed every tick by
         * apply_relay(), so a flapping interlock can drive
         * seed_bumpless_with_ff() every single tick with nothing to notice
         * it happening. Counted here (once per genuine membership change,
         * the same edge the reseed itself reacts to) and surfaced in status
         * (get_status()/dashboard_http.c's JSON, same path as the flags
         * just above) so a chattering membership is visible off-board
         * rather than only inferable from a sustained control-error offset
         * nobody thought to attribute to this. Logged at a RATE-LIMITED
         * cadence, not once per occurrence -- the first change (every
         * commissioned kiln sees a few of these across a run, e.g. autotune
         * finishing on a neighbour) and then every 10th after that, so a
         * genuinely chattering interlock produces a growing but bounded
         * trickle of log lines instead of flooding the UART bridge the way
         * this file's own LOG_PRESTART_ONCE doc comment describes that
         * queue dropping lines under. */
        if (hold_membership_changed) {
            s_exec.zones[zi].ff_membership_change_count++;
            uint32_t count = s_exec.zones[zi].ff_membership_change_count;
            if (count == 1 || count % 10 == 0) {
                ESP_LOGW(TAG, "zone %u coupled feedforward membership changed (count=%lu this run) -- "
                              "PID integral re-seeded to hold commanded duty steady; a fast-growing "
                              "count means a flapping interlock/coupled-neighbour state is leaving "
                              "integral action effectively off for this zone",
                         zi, (unsigned long)count);
            }
        }
    }

    /* PID_EXPANSION_PLAN.md section 2c / Phase 3b: additive cross-zone
     * coupling contribution, added here so it stays inside the isfinite/
     * clamp below rather than escaping it.
     *
     * coupling_coeff[j] (zones_http.h) is zone zi's measured steady-state
     * response in raw degC PER UNIT DUTY AT ZONE j'S HEATER -- the exact
     * same convention as ff_k_dc/model_k_dc, deliberately not a
     * dimensionless ratio. That means a plain `-c_ij*(T_j-sp_j)` is not a
     * duty: it mixes (degC_i/duty_j) with degC_j into degC_i*degC_j/duty_j,
     * which silently rescales the whole term by whatever K_dc happens to be.
     * The dimensionally correct form is the standard measured-disturbance
     * feedforward [W6]/[10]:
     *
     *     u_ff_d = -(Gd / Gu) * d
     *
     * with the manipulated-variable gain Gu = this zone's own
     * d(T_i)/d(duty_i) = z->ff_k_dc, and the disturbance gain Gd =
     * d(T_i)/d(T_j) -- a dimensionless degC_i-per-degC_j ratio, NOT
     * coupling_coeff[j] itself. Gd is recovered by dividing coupling_coeff[j]
     * (degC_i per duty_j) by neighbour j's OWN steady-state gain k_dc_j
     * (degC_j per duty_j); the duty_j unit cancels, leaving degC_i/degC_j:
     *
     *     Gd_ij = coupling_coeff[j] / k_dc_j
     *     term  = -(Gd_ij / z->ff_k_dc) * (T_j - setpoint_j)
     *           = -(coupling_coeff[j] / (k_dc_j * z->ff_k_dc)) * (T_j - setpoint_j)
     *
     * A neighbour running hot (T_j > setpoint_j) subtracts duty from this
     * zone; a neighbour running cold adds it; a neighbour on target changes
     * nothing. The zone-wide setpoint is shared (s_exec.target_c, see its
     * own doc comment), so neighbour j's setpoint is the very same
     * setpoint_c already passed in for zone zi -- no separate lookup needed.
     *
     * Skipped, contributing exactly 0 and never NaN: the diagonal (j==zi),
     * any coupling_coeff[j] that is 0/non-finite (0 is coupling_coeff's own
     * "unmeasured" default -- with every row 0 this loop changes nothing,
     * which is finding 1/the required zero-coefficient parity), any
     * neighbour with no identified k_dc_j of its own (Gd_ij is not
     * computable without it, so "no model for that neighbour" degrades to
     * the same "contribute 0" default as "no coupling measured"), and any
     * neighbour zone_qualifies_as_coupling_neighbor() rejects -- inactive,
     * no valid reading, not currently under PID-family closed-loop control
     * on this shared setpoint, faulted, or blocked by
     * relay_authority_zone_blocked(). That last group is a post-review
     * fix (was just active/reading-valid before): a neighbour sitting at
     * ZONE_CONTROL_MODE_OFF or authority-blocked can be arbitrarily far
     * from setpoint for reasons that have nothing to do with its own
     * heater, so attributing that gap to "this zone's duty is doing
     * something" was simply wrong -- see zone_qualifies_as_coupling_neighbor()'s
     * doc comment for the concrete failure this was producing. The
     * deviation itself is also low-pass filtered and bounded before it's
     * scaled -- see the two comments inside the loop below. */
    float coupling_row[MAX31856_CHANNEL_COUNT];
    if (zones_config_get_coupling(zi, coupling_row)) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (j == zi) continue;
            float c_ij = coupling_row[j];
            if (!isfinite(c_ij) || c_ij == 0.0f) continue;
            const zone_runtime_t *zn = &s_exec.zones[j];
            if (!zone_qualifies_as_coupling_neighbor(zn)) continue;
            float gd_ij = c_ij / zn->ff_k_dc;

            /* Neighbour deviation: the LOW-PASS FILTERED reading (see the
             * coupling_filtered_c update site), not the raw one -- finding
             * 3, keeps thermocouple noise out of this zone's duty. Bounded
             * to +/- zn->pid_cfg.pid_range_c (finding 2) before it is
             * scaled: that is the exact threshold pid.c itself already uses
             * to decide a zone's error is too large for the linear PID/FF
             * model to mean anything (pid_range_c gate in
             * pid_update_terms(), full on/off outside it) -- reusing it
             * here means a deviation this term is allowed to react to
             * linearly is never larger than one pid.c itself would still
             * be doing linear control on. Without this a neighbour idle at
             * OFF used to pass everything else and reach a 600C deviation
             * (the reviewer's scenario) uncapped; with a genuinely
             * qualifying neighbour (PID family, not faulted, not blocked)
             * a multi-hundred-degree gap should not occur in the first
             * place, so this bound is a backstop against a bad reading or
             * a slow-to-settle transient, not the primary defense -- that
             * is finding 1's control_mode/faulted/heat_blocked gate above.
             *
             * coupling_filter_init is only set once the main control loop
             * has actually run at least one tick for the neighbour (the
             * filter update site above). A caller that reaches
             * zone_feedforward() before that (the host tests below, which
             * poke actual_c directly and call this function with no tick
             * loop around it) falls back to the raw actual_c -- the same
             * "snap to the first sample" the filter itself does on its own
             * first update, so this is not a second, divergent behaviour,
             * just the filter's own cold-start case reached a different
             * way. */
            float neighbour_temp = zn->coupling_filter_init ? zn->coupling_filtered_c : zn->actual_c;
            float dev = neighbour_temp - setpoint_c;
            float bound = zn->pid_cfg.pid_range_c;
            if (bound > 0.0f) {
                if (dev > bound) dev = bound;
                else if (dev < -bound) dev = -bound;
            }
            u_ff -= (gd_ij / z->ff_k_dc) * dev;
        }
    }

    if (!isfinite(u_ff)) {
        return 0.0f; /* belt-and-braces: a non-finite setpoint can only come
                      * from a corrupted profile, but it must not become duty */
    }
    if (u_ff < 0.0f) u_ff = 0.0f;
    if (u_ff > 1.0f) u_ff = 1.0f;
    return u_ff;
}

/* Seeds the PID integral so the very next tick (which computes
 * P + I + D + ff) reproduces u_desired -- pid_seed_bumpless() itself now
 * subtracts ff_u before solving for the integral (TODO.md 6A.2's "move the
 * feedforward subtraction into pid_seed_bumpless()"), so this wrapper only
 * has to compute the feedforward term the next tick will use and hand it
 * over alongside u_desired.
 *
 * When u_ff alone already exceeds u_desired the shortfall cannot be expressed
 * -- the integral floor is 0, since a negative one violates the anti-windup
 * clamp on the next tick -- so the zone comes back at its feedforward duty.
 * That is the honest outcome rather than a defect: the model's estimate of
 * what the current setpoint costs to hold is exactly what the operator asked
 * to resume onto. Must be called with s_exec.lock held.
 *
 * zi must be z's own index in s_exec.zones[] -- zone_feedforward() needs it
 * both to look up z's own coupling row (PID_EXPANSION_PLAN.md section 2c)
 * and to skip that row's own diagonal. This is the same zone_feedforward()
 * called from the per-tick control loop (pid_family_zone_tick()), on the
 * same s_exec.target_c/target_rate_c_per_s -- deliberately identical inputs,
 * so a zone reseeded here is bumpless against exactly the feedforward the
 * very next tick will compute, coupling term included. If the two callers
 * ever diverge on what they pass, bump transfer breaks.
 *
 * Residual bump-transfer gap the reviewer flagged: "identical inputs" only
 * covers target_c/target_rate_c_per_s and zi -- it does NOT mean a
 * neighbour's contribution is frozen between this seed and the next real
 * tick. coupling_filtered_c (this zone's neighbour-side low-pass, see its
 * update site) is bumpless against itself -- both calls read whatever the
 * filter's current value is -- but that value keeps moving between ticks
 * exactly like any filtered signal does. If a neighbour's thermocouple
 * reading is recovering (e.g. it just came back in range) between this seed
 * and the next tick, the filtered deviation used here and the one used a
 * tick later can differ, and this zone's duty steps by
 * (gd_ij/z->ff_k_dc) * delta_dev -- the reviewer's example (gd/k =
 * 0.0159/degC, a 30C recovery = a 48% step) is against the raw,
 * pre-filter deviation. The d_filter_tau_s low-pass added here
 * (finding 3) softens this materially by construction: a low-pass turns a
 * step into an exponential approach over ~tau, so the SAME 30C recovery
 * spreads across many ticks instead of landing on whichever single tick
 * happened to seed. It does not eliminate the residual: a real
 * discontinuity in the neighbour's temperature (sensor recovering, not
 * just filtered noise) still passes through, delayed and attenuated rather
 * than blocked. No further machinery is added for this -- the same
 * anti-windup clamp that already bounds every other seed's error is the
 * backstop, and the filter is doing exactly what pid.c's own D filter does
 * for the analogous problem on the local sensor. */
static void seed_bumpless_with_ff(zone_runtime_t *z, uint8_t zi, float u_desired)
{
    float u_ff = zone_feedforward(z, zi, s_exec.target_c, s_exec.target_rate_c_per_s);
    pid_seed_bumpless(&z->pid_state, &z->pid_cfg, s_exec.target_c, z->actual_c, u_desired, u_ff);
}

/* Turns zone zi's relay(s) on/off as one group -- see profile_executor.h.
 * Gated by relay_authority_zone_blocked() (global fault OR that zone's own
 * per-zone block, TODO.md 6A.6) for the ON direction only. Must be called
 * with s_exec.lock held. */
static void apply_relay(uint8_t zi, bool want_on)
{
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(zi, &mask) || mask == 0) {
        s_exec.zones[zi].relay_commanded_on = false;
        return;
    }

    /* Claimed the moment this run can name the mask at all, in BOTH
     * directions and before the authority gate -- not only when a relay is
     * actually energized. A commanded-off write that silently failed leaves a
     * coil closed just as effectively as a commanded-on one, and by the time
     * the sweep is looking for strays it has no way to reconstruct which
     * masks this run once addressed. Over-claiming costs nothing (the sweep
     * intersects with what kiln_io believes is still closed); under-claiming
     * is the whole failure this exists to catch. */
    s_exec.claimed_relay_mask |= mask;

    /* Evaluated EVERY tick now, unconditionally -- NOT only when want_on is
     * true, which is what this function did until a review caught the
     * regression it caused (2026-09-XX PWM/progress-window fix, defect 1).
     * z->heat_blocked is the single source of truth profile_executor_
     * guard_commanded_duty() reads to decide whether the guards see zero or
     * the intended duty; if it were only refreshed on ticks that wanted
     * heat, a duty commanded partway through a PWM window (want_on=true on
     * the on-pulse, false on the off-pulse) would leave heat_blocked stale
     * on every off-pulse -- true from the last on-pulse's real evaluation,
     * but READ by the guard-duty helper as "this tick is genuinely
     * blocked", so a zone with a real, contiguous block ends up reporting
     * ALTERNATING zero/nonzero to the guards at the PWM period instead of a
     * genuine contiguous zero. That broke guard 3 (welded relay) during an
     * authority block specifically -- guard 3 needs a contiguous duty<=0
     * run, which it had before this whole fix (apply_relay() always forced
     * relay_commanded_on=false while blocked) and lost when heat_blocked's
     * staleness was introduced. Evaluating unconditionally restores that: a
     * genuinely blocked zone now reads heat_blocked=true on EVERY tick,
     * want_on or not, so profile_executor_guard_commanded_duty() sees a
     * true contiguous zero again. */
    uint32_t sources = 0;
    bool blocked = relay_authority_zone_blocked(s_exec.safety, zi, &sources);
    bool edge = (blocked != s_exec.zones[zi].heat_blocked) ||
                (blocked && sources != s_exec.zones[zi].heat_blocked_sources);
    if (edge) {
        if (blocked && want_on) {
            /* The common, most-actionable case: this tick actually wanted
             * heat and got refused. */
            ESP_LOGW(TAG, "zone %u WANTS HEAT BUT IS BLOCKED: sources 0x%02X -- no relay will "
                          "close and the run will otherwise look normal",
                     zi, (unsigned)sources);
        } else if (blocked) {
            /* Block state changed on a tick that did not itself want heat
             * (e.g. a PWM off-pulse, or a zone between demands) -- still
             * worth a line, just without claiming this exact tick wanted
             * heat. */
            ESP_LOGW(TAG, "zone %u's heat is blocked: sources 0x%02X", zi, (unsigned)sources);
        } else {
            ESP_LOGI(TAG, "zone %u heat no longer blocked", zi);
        }
    }
    s_exec.zones[zi].heat_blocked = blocked;
    s_exec.zones[zi].heat_blocked_sources = blocked ? sources : 0u;
    if (want_on && blocked) {
        want_on = false;
    }

    if (s_exec.io) {
        /* AUTHORIZED, not the manual gate -- see kiln_io_owner.h's top
         * comment. relay_authority_zone_blocked() just above already
         * applied this run's own gate; kiln_io_owner just serializes the
         * actual write against uart_bridge.c/dashboard_set_relay() (2026-08-19,
         * TODO.md 10.14 Phase 1). */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(mask, want_on ? mask : 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "kiln_io_owner_command_set_relay_mask_authorized failed: %s -- relay "
                          "state for zone %u is unknown",
                     esp_err_to_name(err), zi);
        }
    }
    /* No-op unless CONFIG_KILNCTL_SIM_PLANT. Fed the POST-gate decision, not
     * what control wanted, so the simulated kiln heats only when a real one
     * would have. */
    sim_backend_note_zone_relay(zi, want_on);
    s_exec.zones[zi].relay_commanded_on = want_on;
}

/* What guards 1/2/3/7 (thermal_guard.c) should be told this zone's
 * commanded_duty is, for THIS tick -- factored out of the apply-relays-and-
 * guards loop below purely so it is directly unit-testable (same reasoning
 * as this file's other small pure-decision helpers). See that loop's own
 * comment, right above where this is called, for the full defect this
 * replaces and the load-cap reasoning behind the choice below.
 *
 * REVISED (review defect 1): the first version of this function took an
 * additional want_relay_on_this_tick parameter and only zeroed intended_duty
 * when (want_relay_on_this_tick && heat_blocked). That gate was WRONG, and a
 * regression against the pre-fix code: want_relay_on_this_tick is itself the
 * POST-PWM instantaneous relay decision (heater_output_duty()'s return, see
 * the caller), so under a genuine authority block at a partial intended
 * duty it alternated true/false at the PWM period right along with the
 * relay -- true on an on-pulse (heat_blocked freshly true that tick,
 * correctly zeroing), false on an off-pulse (the gate itself false, so the
 * function returned intended_duty UNZEROED even though the zone was, in
 * fact, still blocked that whole time). Guard 3 (welded relay) needs a
 * CONTIGUOUS duty<=0 run to arm; this alternation broke it during exactly
 * the scenario guard 3 exists for -- a welded/shorted contact discovered
 * while heat is authority-blocked. The pre-fix code never had this bug
 * (apply_relay() always forced relay_commanded_on=false while blocked, a
 * genuine contiguous zero) -- the bug was introduced by this function's own
 * first version, not inherited.
 *
 * Root cause: heat_blocked is now refreshed by apply_relay() on EVERY tick
 * (see that function's own comment on this exact change), so it is no
 * longer only "fresh when want_on was true" -- there is nothing left for a
 * want_relay_on_this_tick gate to usefully condition, and gating on it was
 * actively wrong whenever heat_blocked correctly stayed true across a
 * PWM off-pulse. Dropped entirely: this function now trusts heat_blocked
 * unconditionally, which is correct precisely because the caller keeps it
 * unconditionally fresh. */
static float profile_executor_guard_commanded_duty(bool heat_blocked, float intended_duty)
{
    return heat_blocked ? 0.0f : intended_duty;
}

/* Guard 1's own effective rate requirement for THIS tick -- factored out of
 * the apply-relays-and-guards loop for the same reason profile_executor_
 * guard_commanded_duty() was (direct unit-testability), see that loop's own
 * comment on review defect 2 for the full "a fixed absolute rate has no
 * knowledge of the commanded ramp" reasoning.
 *
 * configured_rate_c_per_min is the zone's own guard_cfg.sanity_rate_c_per_min
 * (0 meaning "not configured" -- substituted with thermal_guard.c's own
 * 0.5 C/min default here so the min() below always compares two real
 * numbers, mirroring effective_f()'s convention in that file without
 * needing to export it). target_rate_c_per_s is s_exec.target_rate_c_per_s,
 * exactly 0.0f during a dwell/step-segment/ramp-lock tick (see that field's
 * own comment) -- deliberately left AT the configured rate in that case
 * (returned unchanged), not capped to zero, so a lagging DWELL still has to
 * catch up at the zone's full configured rate; the cap only ever narrows
 * the requirement while a ramp is genuinely still moving. */
static float profile_executor_guard_sanity_rate(float configured_rate_c_per_min, float target_rate_c_per_s)
{
    float configured = (configured_rate_c_per_min > 0.0f) ? configured_rate_c_per_min : 0.5f;
    float ramp_rate_c_per_min = fabsf(target_rate_c_per_s) * 60.0f;
    if (ramp_rate_c_per_min > 0.0f && ramp_rate_c_per_min < configured) {
        return ramp_rate_c_per_min;
    }
    return configured;
}

/* Must be called with s_exec.lock held. */
static void force_zone_relay_off(uint8_t zi)
{
    heater_output_force_off(&s_exec.zones[zi].heater_state);
    apply_relay(zi, false);
    s_exec.zones[zi].duty = 0.0f;
}

/* Must be called with s_exec.lock held. */
static void force_all_relays_off(void)
{
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        return; /* nothing loaded -- nothing to turn off */
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (s_exec.zones[zi].active) {
            force_zone_relay_off(zi);
        }
    }
}

/* Hands every relay this run ever claimed (claimed_relay_mask, see
 * apply_relay()'s comment) back to RELAY_OWNER_NONE. profile_executor_halt()
 * already did this on its own exit path; this is the SAME release, called
 * from every OTHER path that leaves RUNNING/PAUSED for a state that is not
 * "still an in-progress run" -- normal completion (DONE), a guard trip
 * (FAULTED, all three escalate_guard_trip() branches that set it), and the
 * watchdog's own forced FAULTED transition. Before this existed, only
 * halt() released the claim, so a run that finished on its own or faulted
 * kept every relay it touched tagged RELAY_OWNER_PROFILE until an operator
 * explicitly dismissed it -- confirmed on the bench: a guard-1 fault ended a
 * firing and relay 1 was still refused to /api/relay, the LCD Temperature
 * page and the UART bridge as "owned by a running profile" with no profile
 * running. relay_authority_release_mask() is a plain overwrite (see
 * relay_authority.c), so calling this and then having halt() call it again
 * later (an operator dismissing the same FAULTED/DONE run) is harmless --
 * releasing an already-released mask changes nothing.
 *
 * Deliberately NOT called from profile_executor_pause(): a paused run is
 * still a run in progress by TODO.md section 0's own reasoning (it hands the
 * claim to RELAY_OWNER_MANUAL instead of releasing it) -- releasing here
 * would let a manual command fight a firing that is one profile_executor_
 * resume() away from driving those same relays again. Must be called with
 * s_exec.lock held. */
static void release_profile_relay_claim(void)
{
    relay_authority_release_mask(s_exec.claimed_relay_mask);
    /* The shared heat claim (relay_authority.h) taken atomically right
     * before this run's s_exec.state was set to RUNNING -- see
     * profile_executor_run()'s own comment at that call site. Safe to call
     * unconditionally: relay_authority_heat_zone_claim_end() is a no-op if
     * this run never actually held it (refused before reaching that point).
     * Every path that leaves RUNNING/PAUSED for good funnels through this
     * function except profile_executor_halt(), which releases it directly
     * alongside its own relay_authority_release_mask() call for the same
     * reason it doesn't call this whole function (see halt()'s comment). */
    relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
    /* ...and give K4 back too. Every terminal transition out of RUNNING/
     * PAUSED except profile_executor_halt() (which does it inline, alongside
     * its own duplicate of the two calls above, for the reason its comment
     * gives) funnels through here -- normal completion, all three
     * escalate_guard_trip() branches, and the watchdog's forced FAULTED
     * transition -- so this is the single release point for the firing's
     * heat-enable request. Called AFTER force_all_relays_off()/
     * force_zone_relay_off() at every one of those sites: dropping the zone
     * relay is never gated on, or delayed by, giving K4 back
     * (heat_enable.h's ordering rule). Idempotent, so calling it on a path
     * that never acquired costs nothing. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
}

/* ---- TODO relay/IO segments (owner's request, profiles_http.h's
 * profile_seg_kind_t doc comment) ---------------------------------------
 *
 * These four functions are the executor-side half of the feature: applying
 * a segment's command, and force-releasing it on every path that leaves
 * RUNNING. All four must be called with s_exec.lock held, same as every
 * other s_exec-touching static in this file. */

/* Second, independent zone-ownership check (the storage-side one is
 * profiles_http.c's profile_relay_is_zone_owned(), run at save time) -- a
 * relay can be reassigned to a zone AFTER a profile was saved, same
 * reload-time hazard zones_http.c's relay_mask comment and rules_task.c's
 * compute_heater_relay_mask() both already document for the rule engine,
 * and the exact reason the ramp-ceiling feasibility check just above this
 * function's call site is ALSO re-run at start rather than trusted from
 * save time. relay_1_4 is 1-based. */
static bool relay_io_target_is_zone_owned(uint8_t relay_1_4, uint8_t *out_zone_index)
{
    uint8_t bit = (uint8_t)(1u << (relay_1_4 - 1u));
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (zones_config_get_relay_mask(zi, &zone_mask) && (zone_mask & bit) != 0) {
            if (out_zone_index) *out_zone_index = zi;
            return true;
        }
    }
    return false;
}

/* Applies segment `idx`'s command once and starts tracking it. Called the
 * first tick segment_index reaches a RELAY_IO segment (see the
 * segment-stepping block) -- never re-applied on later ticks while the same
 * segment is still current, so a manual override of a NON-BLOCKING segment's
 * relay in between is possible but is also exactly what relay_authority's
 * ownership claim below exists to prevent for the blocking/relay case. */
static void io_seg_start(uint8_t idx, const profile_segment_t *seg)
{
    io_seg_runtime_t *r = &s_exec.io_segs[idx];
    memset(r, 0, sizeof(*r));
    r->active = true;
    r->is_relay = (seg->io_target >= PROFILE_IO_TARGET_RELAY_BASE) &&
                  (seg->io_target < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
    r->target = seg->io_target;
    r->blocking = seg->io_blocking != 0;
    r->state_on = seg->io_state != 0;
    r->leave_on_at_end = seg->io_leave_on_at_end != 0;
    r->remaining_s = (float)(seg->dwell_min * 60u);

    if (r->is_relay) {
        uint8_t bit = (uint8_t)(1u << (r->target - PROFILE_IO_TARGET_RELAY_BASE));
        /* Same claim-before-write discipline apply_relay() uses: claimed the
         * moment this run can name the bit, in both directions, so the sweep
         * below can always account for it even if the write itself fails. */
        s_exec.claimed_relay_mask |= bit;
        relay_authority_claim_mask(bit, RELAY_OWNER_PROFILE);
        if (s_exec.io) {
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(bit, r->state_on ? bit : 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "relay/IO segment %u: relay %u write failed: %s -- state is unknown",
                         idx + 1, r->target, esp_err_to_name(err));
            }
        }
    } else if (s_exec.io) {
        esp_err_t err = kiln_io_owner_command_set_io(r->target - PROFILE_IO_TARGET_IO_BASE + 1u, r->state_on);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "relay/IO segment %u: IO_%u write failed: %s -- state is unknown",
                     idx + 1, r->target - PROFILE_IO_TARGET_IO_BASE + 1u, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "relay/IO segment %u: %s %u %s (%s, %lus hold)", idx + 1,
             r->is_relay ? "relay" : "IO_", r->is_relay ? r->target : (uint8_t)(r->target - PROFILE_IO_TARGET_IO_BASE + 1u),
             r->state_on ? "ON" : "OFF", r->blocking ? "blocking" : "non-blocking",
             (unsigned long)seg->dwell_min * 60u);
}

/* Ends segment `idx`'s command -- either commanding it off, or (only when
 * honor_leave_on is true AND the segment itself asked for it via
 * leave_on_at_end) leaving it exactly as last commanded and handing
 * ownership back to NONE so it becomes an ordinary, manually-reachable
 * relay/IO from this point on, same as if an operator had always owned it.
 *
 * honor_leave_on is true ONLY on the clean DONE path (see the
 * segment-stepping block and force_all_relays_off()'s caller in the main
 * tick loop). It is deliberately FALSE on every other path that can call
 * this -- profile_executor_halt(), a global or per-zone guard trip
 * escalating to FAULTED, and the guard-9/watchdog stale-tick and safety-trip
 * force-offs -- because those are all abnormal-stop paths where the safe
 * default (relay actually goes off) must win over a per-segment convenience
 * preference, regardless of what the segment asked for. Only a clean,
 * intentional "the schedule finished exactly as planned" end honors the
 * owner's flag; every other ending is treated the same as the flag's own
 * default (off). A natural mid-run timeout (io_segs_tick() below) also
 * always passes false: the segment finished on its own, which is not "the
 * profile ended while it was still running" at all. */
static void io_seg_finish(uint8_t idx, bool honor_leave_on)
{
    io_seg_runtime_t *r = &s_exec.io_segs[idx];
    if (!r->active) {
        return;
    }
    bool leave_on = honor_leave_on && r->leave_on_at_end && r->state_on;

    if (r->is_relay) {
        uint8_t bit = (uint8_t)(1u << (r->target - PROFILE_IO_TARGET_RELAY_BASE));
        if (!leave_on && s_exec.io) {
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(bit, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "relay/IO segment %u: force-off of relay %u failed: %s -- sweep_unowned_relays() "
                              "will keep retrying",
                         idx + 1, r->target, esp_err_to_name(err));
            }
        }
        /* Either way this run is done naming this bit: on the off path
         * nothing more needs it; on the leave-on path an unowned energized
         * relay is intentional (the owner's explicit opt-in) and must NOT be
         * reported by sweep_unowned_relays() as a stray -- see that
         * function's own doc comment on claimed_relay_mask. Releasing the
         * relay_authority claim in both cases means the relay is reachable
         * from /api/relay and the UART bridge again either way, exactly as
         * if no profile had ever touched it. */
        s_exec.claimed_relay_mask &= (uint8_t)~bit;
        relay_authority_release_mask(bit);
    } else if (!leave_on && s_exec.io) {
        esp_err_t err = kiln_io_owner_command_set_io(r->target - PROFILE_IO_TARGET_IO_BASE + 1u, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "relay/IO segment %u: force-off of IO_%u failed: %s", idx + 1,
                     r->target - PROFILE_IO_TARGET_IO_BASE + 1u, esp_err_to_name(err));
        }
    }
    if (leave_on) {
        ESP_LOGW(TAG, "relay/IO segment %u left ON at run end (leave_on_at_end) -- now unowned, reachable "
                      "manually",
                 idx + 1);
    }
    r->active = false;
}

/* Sweeps every segment this run has ever started -- the DONE/FAULTED/HALT/
 * stale-tick backstop, analogous to force_all_relays_off() for zone relays.
 * Safe to call every tick regardless of state: io_seg_finish() is a no-op
 * for a segment that is already inactive, so repeated calls (e.g. every tick
 * of a PAUSED or FAULTED run, or every tick after DONE) cost nothing once
 * the sweep has actually finished. */
static void io_segs_force_all_off(bool honor_leave_on)
{
    for (uint8_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        io_seg_finish(i, honor_leave_on);
    }
}

/* Ticks every ACTIVE, NON-BLOCKING segment's own hold timer, independent of
 * which ramp/dwell segment segment_index currently points at -- this is what
 * "runs alongside the next segment" actually means at 1Hz: the timer keeps
 * counting down no matter how many other segments the shared schedule moves
 * through while it does. A natural (in-run) expiry always force-offs
 * (honor_leave_on=false) -- see io_seg_finish()'s doc comment for why that is
 * correct and not merely the safe default. Must be called once per RUNNING
 * tick, with s_exec.lock held. */
static void io_segs_tick(float dt_s)
{
    for (uint8_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        io_seg_runtime_t *r = &s_exec.io_segs[i];
        if (!r->active || r->blocking) {
            continue; /* a blocking segment is finished by the segment-stepping block itself */
        }
        r->remaining_s -= dt_s;
        if (r->remaining_s <= 0.0f) {
            io_seg_finish(i, false);
        }
    }
}

/* Escalation policy (TODO.md 6A.6, "decide which -- see 6A.6"): guards whose
 * failure mode is severe/board-wide (a welded relay, an out-of-range
 * reading, an electrically faulted sensor) assert the GLOBAL fault source,
 * which blocks every zone via relay_authority_on_blocked() and faults the
 * WHOLE run, not just the tripping zone. Guards whose failure mode is
 * specific to one zone's physics only block that zone, via
 * relay_authority_zone_blocked()'s per-zone mask -- the run continues for
 * any other still-healthy active zone UNLESS
 * zones_config_get_continue_on_zone_trip() is false (the default,
 * TODO.md 6A.3's "abort the whole firing" policy) -- in that case a
 * per-zone trip also faults every other active zone, just without
 * asserting the board-wide safety-link fault the `global` branch does.
 * Returns true if this trip faulted the whole run (global trip, abort
 * policy, or the last active zone just faulted anyway), false if the run
 * continues. Must be called with s_exec.lock held. */
static bool escalate_guard_trip(uint8_t zi, thermal_guard_trip_t reason, const char *detail)
{
    bool global = (reason == THERMAL_GUARD_TRIP_RUNAWAY || reason == THERMAL_GUARD_TRIP_MAX_TEMP ||
                  reason == THERMAL_GUARD_TRIP_MIN_TEMP || reason == THERMAL_GUARD_TRIP_SENSOR_INVALID);
    uint32_t source = (reason == THERMAL_GUARD_TRIP_SENSOR_INVALID) ? SAFETY_FAULT_SRC_THERMO
                                                                    : SAFETY_FAULT_SRC_THERMAL_SANITY;

    if (global) {
        if (s_exec.safety) {
            esp_err_t err = safety_link_set_fault_source(s_exec.safety, source, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "safety_link_set_fault_source(0x%02X) failed: %s", (unsigned)source,
                         esp_err_to_name(err));
            }
        }
        s_exec.global_fault_source = source;
        for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
            if (!s_exec.zones[zi2].active) continue;
            s_exec.zones[zi2].faulted = true;
            strncpy(s_exec.zones[zi2].fault_reason, detail, sizeof(s_exec.zones[zi2].fault_reason) - 1);
            s_exec.zones[zi2].fault_reason[sizeof(s_exec.zones[zi2].fault_reason) - 1] = '\0';
            s_exec.zones[zi2].fault_guard = reason;
            force_zone_relay_off(zi2);
        }
        s_exec.state = PROFILE_EXEC_FAULTED;
        strncpy(s_exec.fault_reason, detail, sizeof(s_exec.fault_reason) - 1);
        s_exec.fault_reason[sizeof(s_exec.fault_reason) - 1] = '\0';
        s_exec.fault_guard = reason;
        /* Abnormal stop: force off regardless of any segment's
         * leave_on_at_end -- see io_seg_finish()'s doc comment for why a
         * guard trip never honors it. */
        io_segs_force_all_off(false);
        release_profile_relay_claim();
        ESP_LOGE(TAG, "GLOBAL thermal guard tripped on zone %u, whole run faulted: %s", zi, detail);
        return true;
    }

    relay_authority_set_zone_blocked(zi, true);
    s_exec.zones[zi].per_zone_blocked = true;
    s_exec.zones[zi].faulted = true;
    strncpy(s_exec.zones[zi].fault_reason, detail, sizeof(s_exec.zones[zi].fault_reason) - 1);
    s_exec.zones[zi].fault_reason[sizeof(s_exec.zones[zi].fault_reason) - 1] = '\0';
    s_exec.zones[zi].fault_guard = reason;
    force_zone_relay_off(zi);
    ESP_LOGE(TAG, "zone %u thermal guard tripped (per-zone): %s", zi, detail);

    /* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
     * firing" -- the remaining zones would keep dumping heat into a chamber
     * whose temperature is now partly unmeasured, and the ware is already
     * ruined; "continue" is the option that needs justifying, so it is the
     * one that requires an explicit opt-in
     * (zones_config_get_continue_on_zone_trip()). This does NOT assert the
     * board-wide SAFETY_FAULT_SRC_* bit the `global` branch above does --
     * the trip's cause is this zone's physics specifically, not a hardware
     * condition threatening every zone, so only the executor's run is
     * faulted, not the safety link. */
    if (!zones_config_get_continue_on_zone_trip()) {
        for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
            if (!s_exec.zones[zi2].active || s_exec.zones[zi2].faulted) continue;
            s_exec.zones[zi2].faulted = true;
            strncpy(s_exec.zones[zi2].fault_reason, detail, sizeof(s_exec.zones[zi2].fault_reason) - 1);
            s_exec.zones[zi2].fault_reason[sizeof(s_exec.zones[zi2].fault_reason) - 1] = '\0';
            s_exec.zones[zi2].fault_guard = reason;
            force_zone_relay_off(zi2);
        }
        s_exec.state = PROFILE_EXEC_FAULTED;
        snprintf(s_exec.fault_reason, sizeof(s_exec.fault_reason),
                "zone %u thermal guard tripped, whole firing aborted per policy: %s", zi, detail);
        s_exec.fault_guard = reason;
        io_segs_force_all_off(false); /* abnormal stop -- see the GLOBAL branch above */
        release_profile_relay_claim();
        ESP_LOGE(TAG, "zone %u per-zone trip abandoned the whole firing (continue_on_zone_trip is off)", zi);
        return true;
    }

    bool all_faulted = true;
    for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
        if (s_exec.zones[zi2].active && !s_exec.zones[zi2].faulted) {
            all_faulted = false;
            break;
        }
    }
    if (all_faulted) {
        s_exec.state = PROFILE_EXEC_FAULTED;
        snprintf(s_exec.fault_reason, sizeof(s_exec.fault_reason), "every active zone individually faulted; last: %s",
                detail);
        s_exec.fault_guard = reason;
        io_segs_force_all_off(false); /* abnormal stop -- see the GLOBAL branch above */
        release_profile_relay_claim();
        ESP_LOGE(TAG, "every active zone faulted -- whole run faulted");
        return true;
    }
    return false;
}

/* Guard 9's own fault-assertion step, factored out of watchdog_task_entry()'s
 * for(;;) loop body so a host test can call it directly -- the loop itself
 * cannot be, same "no seam without restructuring the module" limit as
 * executor_task_entry() (see test_profile_executor_prestart.c's relay-claim
 * test block comment), but this one function has no such limit: it is a
 * plain static function taking s_exec.lock as a precondition, exactly like
 * escalate_guard_trip() above.
 *
 * Audit 2026-08-27 item 2: this used to only call safety_link_set_fault_
 * source(..., true) and stop there -- nothing ever deasserted it, so a
 * single stale control-task tick left SAFETY_FAULT_SRC_APP latched
 * board-wide until reboot, blocking every relay-ON everywhere (including any
 * later, different fault, since relay_authority_on_blocked() only reports
 * "blocked", not which bit) -- the same class of bug escalate_guard_trip()'s
 * global branch already avoids via global_fault_source/clear_this_runs_
 * faults(). OR'd in, not assigned: an earlier global guard trip may already
 * be sitting in global_fault_source, and clear_this_runs_faults() clears the
 * whole mask in one safety_link_set_fault_source() call -- overwriting here
 * would silently drop that other source from ever being cleared.
 *
 * Must be called with s_exec.lock held. */
static void guard9_assert_stale_tick_fault(void)
{
    if (s_exec.safety) {
        safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, true);
    }
    s_exec.global_fault_source |= SAFETY_FAULT_SRC_APP;
}

/* Must be called with s_exec.lock held. */
static void clear_this_runs_faults(void)
{
    if (s_exec.global_fault_source != 0 && s_exec.safety) {
        esp_err_t err = safety_link_set_fault_source(s_exec.safety, s_exec.global_fault_source, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "clearing fault source 0x%02X failed: %s", (unsigned)s_exec.global_fault_source,
                     esp_err_to_name(err));
        }
    }
    s_exec.global_fault_source = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        if (s_exec.zones[zi].per_zone_blocked) {
            relay_authority_set_zone_blocked(zi, false);
            s_exec.zones[zi].per_zone_blocked = false;
        }
        thermal_guard_clear(&s_exec.zones[zi].guard_state);
        s_exec.zones[zi].faulted = false;
    }
}

/* ---- firing quality stats (PID_EXPANSION_PLAN.md Phase 7a/7a-2/7a-3) ------
 *
 * Set 2 from the plan: per-profile firing-accuracy history, distinct from
 * (and independent of) Set 1's per-zone tuning-quality figures, which this
 * task does not touch (autotune_engine.c/pid_autotune.c are off limits here
 * -- another agent owns that half).
 *
 * STORAGE: profiles_nvs (384 KB, see partitions.csv), namespace "fire_stats"
 * -- deliberately NOT kiln_nvs (64 KB, already carrying zones/rules/
 * relay_cycles/run_state) and NOT the "kiln_cfg" namespace profiles_http.c
 * itself uses in profiles_nvs, so a firing-stats read/write can never
 * collide with a profile-blob key. One key per profile_id
 * ("fs_<id>", <=8 chars, well under NVS's 15-char key limit even for a
 * 3-digit builtin id), holding a ring of the last
 * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH runs for THAT profile only -- see
 * profile_firing_history_blob_t below. Only profiles that have actually
 * fired allocate a key, so the realistic footprint is (profiles actually
 * run) * sizeof(profile_firing_history_blob_t), not
 * (PROFILES_MAX_COUNT + builtin count) * that -- see this module's
 * PID_EXPANSION_PLAN.md report for the exact byte arithmetic.
 *
 * WHICH TASK WRITES: the profile executor's own control task
 * (executor_task_entry(), INTERNAL-stacked -- see profile_executor_start()'s
 * xTaskCreatePinnedToCore() call and its doc comment a few hundred lines
 * below, ~3491-3506 in this file as of this pass) or profile_executor_halt()
 * (called from whichever task the operator's Stop request lands on, e.g.
 * dashboard_http.c's HTTP handler task or the UART bridge). NEITHER is
 * PSRAM-stacked: the control task is internal by the same 2026-08-22-crash
 * reasoning that already lets it call run_state_note()/relay_cycles_
 * maybe_persist() from this exact tick path, and every HTTP/bridge task in
 * this codebase already writes NVS routinely (settings, profiles, kiln
 * config) without routing through uart_bridge_ext_run_on_flash_worker() --
 * that worker exists for PSRAM-stacked tasks only (autotune_engine.c's),
 * which this write path never runs on. Written ONCE at run completion (the
 * DONE/FAULTED tick transition, or an operator halt() that ends a RUNNING/
 * PAUSED run early) -- never per tick, so the partition is never worn by
 * this feature. */

#define FIRING_STATS_NVS_PARTITION "profiles_nvs"
#define FIRING_STATS_NVS_NAMESPACE "fire_stats"

typedef struct {
    uint8_t count; /* 0..PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH */
    profile_firing_run_record_t runs[PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH]; /* runs[0] = newest */
} profile_firing_history_blob_t;

/* One tick's worth of firing-quality accumulation for one zone -- pure
 * (touches only *z and its own arguments, no s_exec, no lock, no I/O), so a
 * host test can drive a synthetic error sequence through it directly without
 * a real FreeRTOS task loop. See profile_exec_firing_stats_t's doc comment
 * (profile_executor.h) for the exclusion/edge-case rules this implements:
 *
 *   - actual_valid == false: counted in fs_excluded_sample_count, NOT folded
 *     into fs_err_sum/fs_iae_raw_sum as a zero error. duration still
 *     accrues (the zone was still "in the run" for that tick), but nothing
 *     else does.
 *   - error is SIGNED (actual - target): + = running hot, - = running cold.
 *   - overshoot tracks the largest positive error; undershoot tracks the
 *     largest positive MAGNITUDE of a negative error (both reported as
 *     non-negative numbers, see the struct's own comment).
 *   - ramp vs dwell is decided by the caller's `dwelling` flag for this
 *     tick, not re-derived here. */
static void firing_stats_zone_tick(zone_runtime_t *z, float target_c, bool dwelling, uint32_t elapsed_s,
                                    uint8_t segment_index, float dt_s)
{
    z->fs_duration_s += (uint32_t)(dt_s + 0.5f);
    if (!z->actual_valid) {
        z->fs_excluded_sample_count++;
        return;
    }
    float error = z->actual_c - target_c;
    z->fs_sample_count++;
    z->fs_err_sum += error;
    z->fs_iae_raw_sum += fabsf(error) * dt_s;
    if (error > z->fs_max_overshoot_c) {
        z->fs_max_overshoot_c = error;
        z->fs_max_overshoot_elapsed_s = elapsed_s;
        z->fs_max_overshoot_segment = segment_index;
    }
    if (-error > z->fs_max_undershoot_c) {
        z->fs_max_undershoot_c = -error;
        z->fs_max_undershoot_elapsed_s = elapsed_s;
        z->fs_max_undershoot_segment = segment_index;
    }
    float abs_error = fabsf(error);
    if (dwelling) {
        z->fs_dwell_abs_err_sum += abs_error;
        z->fs_dwell_sample_count++;
        if (abs_error > z->fs_dwell_err_max_c) z->fs_dwell_err_max_c = abs_error;
    } else {
        z->fs_ramp_abs_err_sum += abs_error;
        z->fs_ramp_sample_count++;
        if (abs_error > z->fs_ramp_err_max_c) z->fs_ramp_err_max_c = abs_error;
    }
}

/* Derives the reportable (mean/max/normalized) figures from a zone's raw
 * running sums. Pure -- takes no lock, touches no NVS -- so it's equally
 * usable for a live in-progress snapshot (profile_executor_get_status()) and
 * for the final record persisted at run end. duration_s/setpoint_span_c
 * come from the caller (duration is per-zone; span is run-wide, shared
 * across zones -- see s_exec_state_t.fs_target_min_c/fs_target_max_c). */
static void firing_stats_snapshot(const zone_runtime_t *z, float setpoint_span_c,
                                   profile_exec_firing_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sample_count = z->fs_sample_count;
    out->excluded_sample_count = z->fs_excluded_sample_count;
    out->duration_s = z->fs_duration_s;
    out->max_overshoot_c = z->fs_max_overshoot_c;
    out->max_overshoot_elapsed_s = z->fs_max_overshoot_elapsed_s;
    out->max_overshoot_segment = z->fs_max_overshoot_segment;
    out->max_undershoot_c = z->fs_max_undershoot_c;
    out->max_undershoot_elapsed_s = z->fs_max_undershoot_elapsed_s;
    out->max_undershoot_segment = z->fs_max_undershoot_segment;
    out->iae_raw_c_s = z->fs_iae_raw_sum;

    if (z->fs_sample_count > 0) {
        out->mean_error_c = z->fs_err_sum / (float)z->fs_sample_count;
    }
    if (z->fs_ramp_sample_count > 0) {
        out->ramp_err_mean_c = z->fs_ramp_abs_err_sum / (float)z->fs_ramp_sample_count;
    }
    out->ramp_err_max_c = z->fs_ramp_err_max_c;
    if (z->fs_dwell_sample_count > 0) {
        out->dwell_err_mean_c = z->fs_dwell_abs_err_sum / (float)z->fs_dwell_sample_count;
    }
    out->dwell_err_max_c = z->fs_dwell_err_max_c;

    /* Normalized IAE -- see profile_exec_firing_stats_t's doc comment for
     * the span-floor and zero-duration guards. Both guards make this 0
     * rather than NAN/inf for a degenerate run (no ticks yet, or a run that
     * somehow ended in the same tick it started) -- 0 reads as "nothing to
     * show," which is true, rather than propagating a NaN into whatever
     * later formats it. */
    float span = fmaxf(setpoint_span_c, PROFILE_EXECUTOR_FIRING_STATS_MIN_SPAN_C);
    if (out->duration_s > 0) {
        out->iae_normalized = out->iae_raw_c_s / ((float)out->duration_s * span);
    }
}

/* Must be called with s_exec.lock held. Fills rec from the run currently (or
 * just-finished) in s_exec -- profile id/name/zone_mask/duration, then each
 * active zone's derived stats snapshot plus the PID gains in force right
 * now, so a later comparison against an older ring entry can tell whether a
 * re-tune happened between them. */
static void firing_stats_build_record(profile_firing_run_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->profile_id = s_exec.profile_id;
    strncpy(rec->profile_name, s_exec.profile.name, sizeof(rec->profile_name) - 1);
    rec->run_started_unix_s = s_exec.run_started_unix_s;
    rec->duration_s = s_exec.total_elapsed_s;
    rec->zone_mask = s_exec.profile.zone_mask;

    float span = fabsf(s_exec.fs_target_max_c - s_exec.fs_target_min_c);
    if (isnan(s_exec.fs_target_min_c) || isnan(s_exec.fs_target_max_c)) {
        span = 0.0f; /* never ticked -- e.g. halted the instant it started */
    }

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &s_exec.zones[zi];
        profile_firing_zone_record_t *zr = &rec->zones[zi];
        zr->active = z->active;
        if (!z->active) continue;
        firing_stats_snapshot(z, span, &zr->stats);
        if (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) {
            zr->kp = z->pid_cfg.kp;
            zr->ki = z->pid_cfg.ki;
            zr->kd = z->pid_cfg.kd;
        }
    }
}

/* NVS I/O only -- no s_exec, no lock. Safe to call from any task/state.
 * Loads FIRING_STATS_NVS_NAMESPACE/"fs_<id>" from profiles_nvs; a missing
 * key (never fired) is reported as an empty (count == 0) blob, not an
 * error -- that's the normal, common case for most profiles. */
static bool firing_stats_load(uint8_t profile_id, profile_firing_history_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    char key[16];
    snprintf(key, sizeof(key), "fs_%u", (unsigned)profile_id);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(FIRING_STATS_NVS_PARTITION, FIRING_STATS_NVS_NAMESPACE,
                                             NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* ESP_ERR_NVS_NOT_FOUND here means the namespace itself has never
         * been written to (no profile has ever finished a firing yet) --
         * expected on a fresh board, not worth logging. */
        return (err == ESP_ERR_NVS_NOT_FOUND);
    }
    size_t len = sizeof(*out);
    err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return true; /* this profile has never fired -- not an error */
    }
    if (err != ESP_OK || len != sizeof(*out)) {
        ESP_LOGW(TAG, "firing_stats_load(%u) failed: %s (len %u/%u)", (unsigned)profile_id,
                 esp_err_to_name(err), (unsigned)len, (unsigned)sizeof(*out));
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

/* Persists rec as the newest entry for its own profile_id -- read-modify-
 * write against profiles_nvs, called from whichever task ended the run
 * (see this section's top comment for why that's always safe here). A
 * failure is logged and otherwise swallowed: losing one run's history is
 * not worth failing the run itself over, matching relay_cycles_flush()'s
 * and run_state_note()'s own non-fatal convention. */
static void firing_stats_persist(const profile_firing_run_record_t *rec)
{
    profile_firing_history_blob_t blob;
    firing_stats_load(rec->profile_id, &blob); /* empty blob on any failure -- still safe to prepend into */

    uint8_t keep = (blob.count < PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH)
                       ? blob.count
                       : (PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH - 1);
    if (keep > 0) {
        memmove(&blob.runs[1], &blob.runs[0], keep * sizeof(blob.runs[0]));
    }
    blob.runs[0] = *rec;
    blob.count = keep + 1;

    char key[16];
    snprintf(key, sizeof(key), "fs_%u", (unsigned)rec->profile_id);
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(FIRING_STATS_NVS_PARTITION, FIRING_STATS_NVS_NAMESPACE,
                                             NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firing_stats_persist(%u): nvs_open failed: %s", (unsigned)rec->profile_id,
                 esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, key, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firing_stats_persist(%u): write failed: %s", (unsigned)rec->profile_id,
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "firing stats persisted for profile %u (%s), %u/%u history entries",
                 (unsigned)rec->profile_id, rec->profile_name, (unsigned)blob.count,
                 (unsigned)PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH);
    }
    nvs_close(h);
}

/* Called from the tick loop's non-RUNNING branch (DONE/FAULTED) and from
 * profile_executor_halt() (an operator stop out of RUNNING/PAUSED, which
 * never passes through that tick-loop transition). s_exec.fs_persisted
 * makes a second call for the same run a no-op -- halt() dismissing an
 * already-DONE/FAULTED run must not persist twice. Must be called with
 * s_exec.lock held; the actual NVS write happens after the caller releases
 * the lock (matching capture_run_snapshot()'s split), via the record this
 * writes into *out_rec and the bool it returns. */
static bool firing_stats_maybe_finalize(profile_firing_run_record_t *out_rec)
{
    if (s_exec.fs_persisted || s_exec.state == PROFILE_EXEC_IDLE) {
        return false;
    }
    firing_stats_build_record(out_rec);
    s_exec.fs_persisted = true;
    return true;
}

/* ---- reboot breadcrumb (TODO.md 6A.3, "No auto-resume across reboot") ------
 *
 * The relays-come-up-off half of that bullet is kiln_io_init()'s latch
 * ordering and is untouched here. This is the other half: a persisted
 * description of what was running and how far it got, so that after a
 * brownout the operator can tell "I stopped it" from "the power went out".
 * run_state.c owns the flash side, including the write cadence; this file
 * only decides WHEN a transition is worth recording.
 *
 * Nothing in this file ever reads that record back. There is deliberately no
 * path from a stored record to profile_executor_run() -- see run_state.h. */

typedef struct {
    run_state_snapshot_t snap;
    char name[PROFILE_NAME_MAX_LEN + 1];
    char reason[sizeof(s_exec.fault_reason)];
} run_snapshot_buf_t;

/* Copies the live run into a caller-owned buffer so the NVS write can happen
 * without pinning s_exec.lock across it wherever that's practical. Must be
 * called with s_exec.lock held. */
static void capture_run_snapshot(run_snapshot_buf_t *b)
{
    memset(b, 0, sizeof(*b));
    strncpy(b->name, s_exec.profile.name, sizeof(b->name) - 1);
    strncpy(b->reason, s_exec.fault_reason, sizeof(b->reason) - 1);

    /* On the DONE path segment_index has already stepped one past the last
     * segment (that step is what ends the run). Clamp it, or the breadcrumb
     * reads "segment 6 / 5" -- a detail nobody would trust the rest of the
     * record after seeing. */
    uint8_t seg = s_exec.segment_index;
    if (s_exec.profile.segment_count > 0 && seg >= s_exec.profile.segment_count) {
        seg = (uint8_t)(s_exec.profile.segment_count - 1u);
    }

    b->snap.profile_id = s_exec.profile_id;
    b->snap.profile_name = b->name;
    b->snap.zone_mask = s_exec.profile.zone_mask;
    b->snap.segment_index = seg;
    b->snap.segment_count = s_exec.profile.segment_count;
    b->snap.dwelling = s_exec.dwelling;
    b->snap.target_c = s_exec.target_c;
    b->snap.segment_elapsed_s = s_exec.segment_elapsed_s;
    b->snap.fault_guard = (uint8_t)s_exec.fault_guard;
    b->snap.fault_reason = b->reason;
}

/* ---- config reload while running (TODO.md 6A.7) ------------------------------ */

/* De-energizes a specific mask instead of "whatever mask this zone owns now",
 * which is all apply_relay() can express -- it re-reads the live config every
 * call. The one case that needs the distinction is an operator re-assigning
 * relays mid-firing: the contacts that must open are the ones the zone owned
 * a moment ago, and by the time the reload notices, the config can no longer
 * name them. Skipping this would leave those relays latched closed under a
 * mask no zone controls any more -- nothing would ever command them off
 * again, not even a guard trip or halt(), since every one of those paths also
 * goes through the live mask. Must be called with s_exec.lock held. */
static void force_relay_mask_off(uint8_t zi, uint8_t mask)
{
    heater_output_force_off(&s_exec.zones[zi].heater_state);
    s_exec.claimed_relay_mask |= mask; /* see apply_relay() -- this is the one caller that can be handed a mask the live config no longer knows */
    if (s_exec.io && mask != 0) {
        /* AUTHORIZED -- same reasoning as apply_relay() above. */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(mask, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "zone %u: dropping superseded relay mask 0x%02X failed: %s -- "
                          "those contacts may still be closed and nothing owns them now",
                     zi, mask, esp_err_to_name(err));
        }
    }
    sim_backend_note_zone_relay(zi, false);
    s_exec.zones[zi].relay_commanded_on = false;
    s_exec.zones[zi].duty = 0.0f;
}

/* The backstop for every path above (TODO.md 6A.7's "unowned-relay sweep").
 *
 * Everything else in this file that can OPEN a relay -- apply_relay(),
 * force_zone_relay_off(), halt/pause, a guard trip, force_relay_mask_off() --
 * can only address contacts it can still *name*, and all but the last name
 * them through the live zone config. So there is a family of failures with no
 * recovery path at all: one kiln_io_set_relay_mask() that returns an error and
 * leaves a coil energized, or a mask edit whose force-off didn't land, and
 * those contacts are then closed under a mask no zone owns any more. Nothing
 * downstream ever looks at them again -- a later guard trip re-opens the NEW
 * mask, halt() re-opens the NEW mask, and the kiln keeps heating from a relay
 * the firmware has forgotten it commanded. Guard 9 does call
 * kiln_io_all_relays_off(), but only once the control task has stopped
 * ticking, which is a different fault entirely: here the task is alive and
 * confidently driving the wrong set of contacts.
 *
 * The three masks this intersects, and why each one is needed:
 *
 *   kiln_io_get_relay_shadow()   what the board believes is still CLOSED.
 *       kiln_io.c refuses to update this from a failed write and re-syncs it
 *       from the part instead (see kiln_io_set_relay_mask()), so it is honest
 *       about exactly the failure this function exists for -- and it is what
 *       keeps the common case free: once a stray is actually cleared the
 *       shadow drops the bit, so the steady state is a compare and no I2C
 *       traffic at all, which is what makes this affordable at 1 Hz.
 *
 *   claimed_relay_mask           what THIS run has ever commanded.
 *       Without it this would be "open every relay no zone currently owns",
 *       and relays 1-4 are not the executor's exclusive property: the
 *       dashboard's manual /api/relay and the UART bridge's SET_RELAY can
 *       both energize any relay during a firing, gated only by
 *       relay_authority_on_blocked(), with no notion of who else is driving.
 *       An operator holding a damper or a blower on through a manual relay
 *       would have it chattered off once a second by a "safety" feature. A
 *       relay this run never touched is not this run's to open.
 *
 *   ~owned                       who may legitimately hold one right now.
 *       Active zones of this run whatever their fault state -- a faulted
 *       zone's relays are still that zone's to command off, and its mask is
 *       still resolvable, so it is not stranded. Plus any zone autotune is
 *       running on: autotune_engine.c drives relays through its own
 *       apply_relay() and the two engines are only mutually exclusive
 *       PER ZONE (autotune_engine_run() refuses a zone this run is driving,
 *       profile_executor_run() refuses a zone autotune holds), so a step test
 *       on zone 2 alongside a firing on zones 0-1 is a supported combination
 *       and its contacts must survive this.
 *
 * A zone whose live mask can no longer be READ contributes nothing to owned,
 * which is deliberate and is the second half of reload_zone_config()'s
 * dropped-zone branch: that branch force-opens the cached mask, and if that
 * write failed, this is what keeps retrying it.
 *
 * Reaching a non-zero stray mask at all means an earlier force-off failed or
 * an edit stranded contacts, so it is an ERROR every time it happens rather
 * than once -- the log repeating at 1 Hz is proportionate to a coil that is
 * still closed and still refusing to open. Must be called with s_exec.lock
 * held; the autotune query below takes s_at.lock while we hold s_exec.lock,
 * which is safe only because autotune never does the reverse (its one call
 * into this module, profile_executor_zone_is_active(), is made before it
 * takes s_at.lock). */
static void sweep_unowned_relays(void)
{
    if (!s_exec.io) {
        return;
    }

    uint8_t owned = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active && !autotune_engine_is_active_on_zone(zi)) {
            continue;
        }
        uint8_t mask = 0;
        if (zones_config_get_relay_mask(zi, &mask)) {
            owned |= mask;
        }
    }

    uint8_t stray = (uint8_t)(kiln_io_get_relay_shadow(s_exec.io) & s_exec.claimed_relay_mask & (uint8_t)~owned);
    if (stray == 0) {
        return;
    }

    ESP_LOGE(TAG, "UNOWNED RELAY(S) 0x%02X still closed (TODO.md 6A.7 sweep): claimed 0x%02X by this run, "
                  "owned 0x%02X by an active zone or an autotune run -- an earlier force-off failed or a "
                  "mask edit stranded them; forcing off",
             stray, s_exec.claimed_relay_mask, owned);
    /* AUTHORIZED -- same reasoning as apply_relay() above. */
    esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(stray, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unowned-relay sweep could not open 0x%02X: %s -- those contacts are still closed and "
                      "the expander is not answering",
                 stray, esp_err_to_name(err));
    }
}

/* Re-reads zone zi's settings and folds them into the live run. Returns true
 * if anything actually moved (the caller only uses this for the summary
 * line). Called only from reload_config_if_changed(), with s_exec.lock held
 * and after this tick's readings have been stored, so the bumpless seed below
 * uses a measurement from this tick rather than the previous one.
 *
 * What is deliberately NOT touched here:
 *   - thermal_guard_state_t. A latched trip must survive a config edit, or
 *     "edit a threshold" becomes an undocumented way to clear a fault and
 *     re-energize a kiln that just tripped. TODO.md 6A.3 makes halt() the one
 *     explicit acknowledgement path, and a reload is not an acknowledgement.
 *   - heater_output_state_t (except where a mask/mode change forces the zone
 *     off outright). Resetting it would discard the min-on/min-off timers and
 *     the current window's accumulated on-time, i.e. an operator nudging
 *     window_ms could machine-gun a mechanical contactor -- the exact wear
 *     TODO.md 6A.1's min-on/min-off exists to prevent. The in-flight window
 *     finishes on the old timing; the next one uses the new. */
static bool reload_zone_config(uint8_t zi)
{
    zone_runtime_t *z = &s_exec.zones[zi];
    bool changed = false;

    /* Every getter here fails identically for a zone_index past thermo_count,
     * so the control-mode read doubles as "is this zone still configured at
     * all". A shrunk thermo_count must not silently drop a zone out of a
     * running firing -- the run's active set is what the profile was
     * validated against, and quietly editing it mid-flight would leave the
     * operator's dashboard and the physical kiln disagreeing about what is
     * being driven. Keep it active on its last-known settings, say so at
     * ERROR, and open its contacts: apply_relay() refuses to energize a zone
     * whose mask it can't read, but "refuses" there means "returns without
     * touching the hardware", which on its own would strand an already-closed
     * relay. */
    zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
    if (!zones_config_get_control_mode(zi, &mode)) {
        ESP_LOGE(TAG, "zone %u is active in this run but is no longer configured (thermo_count shrank?) -- "
                      "keeping it in the run on its last-known settings, forcing its relays off; "
                      "it cannot be re-energized while the config can't name its relays", zi);
        force_relay_mask_off(zi, z->relay_mask);
        return true;
    }

    /* Mask first, mode second: both may force the zone off, and doing the
     * mask change first means the mode change's force-off already acts on the
     * new mask instead of re-opening contacts that were just handed away. */
    uint8_t mask = 0;
    if (zones_config_get_relay_mask(zi, &mask) && mask != z->relay_mask) {
        ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u relay mask 0x%02X -> 0x%02X -- old mask forced off "
                      "before the new one is adopted", zi, z->relay_mask, mask);
        force_relay_mask_off(zi, z->relay_mask);
        z->relay_mask = mask;
        changed = true;
    }

    /* A mode change is a discontinuity in what the output even means (a duty
     * fraction vs. a hysteresis latch vs. nothing), so there is no meaningful
     * handover to attempt -- TODO.md 6A.2's bumpless rule is about carrying a
     * controller's own state across a tuning change, not about translating
     * between controllers. Drop the heat, start the new mode cold, and make
     * the operator's action loud in the log. */
    bool mode_changed = (mode != z->control_mode);
    if (mode_changed) {
        ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u control mode %d -> %d -- relays forced off and "
                      "the PID restarted cold; no handover is attempted between modes",
                 zi, (int)z->control_mode, (int)mode);
        force_zone_relay_off(zi);
        z->control_mode = mode;
        pid_reset(&z->pid_state);
        z->fuzzy_prev_effective_ki = 0.0f; /* no bump-transfer history to carry into a cold start */
        changed = true;
    }

    /* Tuning edits are the case TODO.md 6A.7 exists for: they must land
     * without restarting the firing. pid_seed_bumpless() solves the integral
     * that reproduces the duty this zone last commanded under the NEW gains,
     * so the element keeps doing what it was doing and the new tuning takes
     * over from there -- instead of the step change a cold integral would
     * produce halfway up a ramp. */
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    if (zones_config_get_pid(zi, &kp, &ki, &kd) &&
        (kp != z->pid_cfg.kp || ki != z->pid_cfg.ki || kd != z->pid_cfg.kd)) {
        ESP_LOGI(TAG, "zone %u PID gains reloaded mid-firing: kp %.4g->%.4g, ki %.4g->%.4g, kd %.4g->%.4g",
                 zi, (double)z->pid_cfg.kp, (double)kp, (double)z->pid_cfg.ki, (double)ki,
                 (double)z->pid_cfg.kd, (double)kd);
        z->pid_cfg.kp = kp;
        z->pid_cfg.ki = ki;
        z->pid_cfg.kd = kd;
        /* PID_FUZZY covered here too, not just plain PID: it shares the same
         * pid_state/pid_cfg base gains, so an operator's tuning edit needs
         * the identical bumpless handling either way. */
        if (!mode_changed &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY)) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, zi, z->duty);
                /* Reseeded off the BASE gains (u_desired's split assumes
                 * cfg.ki, not whatever cell the fuzzy layer last picked), so
                 * that's the "previous effective Ki" hazard-3's bump-transfer
                 * should compare next fuzzy tick against. */
                z->fuzzy_prev_effective_ki = ki;
            } else {
                /* Seeding off a fabricated measurement would bake this tick's
                 * bad reading into the integral and keep driving from it long
                 * after the sensor recovers. A cold restart costs one
                 * transient; a poisoned integral costs the rest of the run. */
                pid_reset(&z->pid_state);
                z->fuzzy_prev_effective_ki = 0.0f;
            }
        }
        changed = true;
    }

    /* The identified plant model, re-read on the same path and for the same
     * reason as the gains above -- autotune writes both through zones_http at
     * one acceptance point, so an autotune that finishes DURING a firing (on a
     * zone this run isn't driving, which is the only way it can run at all)
     * must be able to switch this zone's feedforward on without the operator
     * restarting a multi-hour firing.
     *
     * And it needs the bumpless treatment more than a gain edit does, not
     * less: a model appearing where there was none takes u_ff from 0 to
     * whatever the kiln costs to hold at the current setpoint, which on a hot
     * kiln is most of the duty. Without re-seeding, that lands as an
     * instantaneous step on top of an integral that was built to supply the
     * same heat -- the element would go to full for as long as the integrator
     * needs to unwind. Re-seeding hands the same total duty over to the new
     * split between ff and I, and lets the PID walk from there. */
    if (zone_load_model(zi)) {
        ESP_LOGI(TAG, "zone %u plant model reloaded mid-firing: feedforward %s (K_dc %.4g, tau %.4gs) -- "
                      "PID re-seeded so the duty split changes without the duty itself stepping",
                 zi, z->ff_enabled ? "ON" : "OFF", (double)z->ff_k_dc, (double)z->ff_tau_s);
        if (!mode_changed &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY)) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, zi, z->duty);
                z->fuzzy_prev_effective_ki = z->pid_cfg.ki; /* see the gain-reload path above */
            } else {
                pid_reset(&z->pid_state); /* same reasoning as the gain path above */
                z->fuzzy_prev_effective_ki = 0.0f;
            }
        }
        changed = true;
    }

    /* Same 0-means-not-configured substitution as run(), applied before the
     * comparison so an operator clearing a field back to blank reads as
     * "returned to the default", not as a spurious change every reload. */
    float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
    if (zones_config_get_heater_cfg(zi, &window_ms, &min_on_ms, &min_off_ms)) {
        uint32_t w = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS;
        uint32_t on = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS;
        uint32_t off = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS;
        if (w != z->heater_cfg.window_ms || on != z->heater_cfg.min_on_ms || off != z->heater_cfg.min_off_ms) {
            ESP_LOGI(TAG, "zone %u heater timing reloaded mid-firing: window %lu->%lums, min_on %lu->%lums, "
                          "min_off %lu->%lums (current window finishes on the old values)",
                     zi, (unsigned long)z->heater_cfg.window_ms, (unsigned long)w,
                     (unsigned long)z->heater_cfg.min_on_ms, (unsigned long)on,
                     (unsigned long)z->heater_cfg.min_off_ms, (unsigned long)off);
            z->heater_cfg.window_ms = w;
            z->heater_cfg.min_on_ms = on;
            z->heater_cfg.min_off_ms = off;
            changed = true;
        }
    }

    /* Guard thresholds are TODO.md 6A.7's "more dangerous" branch: the doc
     * offers "require the zone to be idle, or log it loudly as an operator
     * action", and this takes the second option -- refusing the edit outright
     * would be worse in the case that actually matters, an operator who has
     * just realised a ceiling is wrong for the ware in the kiln right now and
     * needs it corrected without aborting a multi-hour firing. So each
     * threshold moves immediately and each one is logged individually at WARN
     * with old -> new, because the log is the only record that the protection
     * envelope this firing ran under is not the one the zone is configured
     * with today. Note this can only ever change what trips NEXT tick -- an
     * already-latched trip is untouched (see this function's header). */
    float max_temp_c = 0.0f, min_temp_c = -20.0f;
    if (zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c)) {
        if (max_temp_c != z->guard_cfg.max_temp_c) {
            ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u guard max_temp_c %.1f -> %.1f",
                     zi, (double)z->guard_cfg.max_temp_c, (double)max_temp_c);
            z->guard_cfg.max_temp_c = max_temp_c;
            changed = true;
        }
        if (min_temp_c != z->guard_cfg.min_temp_c) {
            ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u guard min_temp_c %.1f -> %.1f",
                     zi, (double)z->guard_cfg.min_temp_c, (double)min_temp_c);
            z->guard_cfg.min_temp_c = min_temp_c;
            changed = true;
        }
    }

    float sanity_rate = 0.0f;
    if (zones_config_get_sanity_rate(zi, &sanity_rate)) {
        float applied = (sanity_rate > 0.0f) ? sanity_rate : PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN;
        if (applied != z->guard_cfg.sanity_rate_c_per_min) {
            ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u guard sanity_rate_c_per_min %.3f -> %.3f",
                     zi, (double)z->guard_cfg.sanity_rate_c_per_min, (double)applied);
            z->guard_cfg.sanity_rate_c_per_min = applied;
            changed = true;
        }
    }

    /* TODO.md 6A.3's remaining named thresholds -- same "raw pass-through,
     * thermal_guard.c owns the 0->default substitution" reasoning as run()'s
     * own guard_cfg build above, and the same loud-logging-per-field
     * discipline as every other guard threshold in this function. */
    {
        float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
        float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
        if (zones_config_get_guard_thresholds(zi, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                              &runaway_margin, &drift_period_s, &debounce_ticks,
                                              &frozen_window_s)) {
#define RELOAD_GUARD_FIELD(field, new_val, fmt)                                                          \
            if ((new_val) != z->guard_cfg.field) {                                                       \
                ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u guard " #field " " fmt " -> " fmt,     \
                         zi, (double)z->guard_cfg.field, (double)(new_val));                              \
                z->guard_cfg.field = (new_val);                                                           \
                changed = true;                                                                           \
            }
            RELOAD_GUARD_FIELD(wrong_dir_window_s, wd_window_s, "%.1f")
            RELOAD_GUARD_FIELD(wrong_dir_rate_c_per_min, wd_rate, "%.3f")
            RELOAD_GUARD_FIELD(off_settle_s, off_settle_s, "%.1f")
            RELOAD_GUARD_FIELD(runaway_rate_c_per_min, runaway_rate, "%.3f")
            RELOAD_GUARD_FIELD(runaway_margin_c, runaway_margin, "%.1f")
            RELOAD_GUARD_FIELD(drift_period_s, drift_period_s, "%.1f")
            RELOAD_GUARD_FIELD(sensor_fault_debounce_ticks, debounce_ticks, "%.0f")
            RELOAD_GUARD_FIELD(frozen_window_s, frozen_window_s, "%.1f")
            {
                /* The five v8 overrides reload mid-firing on exactly the same
                 * terms as the eight above -- an operator who widens a window
                 * during a run must see it take effect, and must see it
                 * logged. */
                float progress_duty_min = 0.0f, progress_window_s = 0.0f, drift_hysteresis_c = 0.0f;
                float frozen_eps_c = 0.0f, cross_zone_period_s = 0.0f;
                if (zones_config_get_guard_extra(zi, &progress_duty_min, &progress_window_s,
                                                 &drift_hysteresis_c, &frozen_eps_c, &cross_zone_period_s)) {
                    RELOAD_GUARD_FIELD(progress_duty_min, progress_duty_min, "%.3f")
                    RELOAD_GUARD_FIELD(progress_window_s, progress_window_s, "%.1f")
                    RELOAD_GUARD_FIELD(drift_hysteresis_c, drift_hysteresis_c, "%.1f")
                    RELOAD_GUARD_FIELD(frozen_eps_c, frozen_eps_c, "%.3f")
                    RELOAD_GUARD_FIELD(cross_zone_period_s, cross_zone_period_s, "%.1f")
                }
            }
#undef RELOAD_GUARD_FIELD
        }
    }

    float cross_zone_delta_c = 0.0f;
    if (zones_config_get_cross_zone_delta(zi, &cross_zone_delta_c) &&
        cross_zone_delta_c != z->guard_cfg.cross_zone_max_delta_c) {
        /* 0 here disarms guard 8 entirely (zones_http.h's deliberate opposite
         * convention to sanity_rate), so this particular edit can silently
         * remove a protection rather than merely widen it -- all the more
         * reason for it to be in the log by name. */
        ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u guard cross_zone_max_delta_c %.1f -> %.1f%s",
                 zi, (double)z->guard_cfg.cross_zone_max_delta_c, (double)cross_zone_delta_c,
                 (cross_zone_delta_c <= 0.0f) ? " (guard 8 now DISABLED for this zone)" : "");
        z->guard_cfg.cross_zone_max_delta_c = cross_zone_delta_c;
        changed = true;
    }

    /* TODO.md 6A.7's "re-check max_ramp_c_per_hr against a running profile":
     * this ceiling is a run-*start* feasibility gate (profiles_http.c), so
     * an operator lowering it mid-firing below what the running segment
     * demands previously went unnoticed until the ware finished. This does
     * not abort or throttle anything -- the ramp itself is unaffected, same
     * as every other guard-threshold edit above -- it only makes the gap
     * loud in the log, once per zone per time it newly becomes infeasible
     * (not every tick), mirroring this function's existing pattern for
     * guard thresholds. z->max_ramp_warned resets the instant the segment
     * changes or the ceiling is raised back above it, so a real re-trip
     * after that logs again instead of staying silently latched. */
    {
        float ceiling = 0.0f;
        /* `ceiling > 0.0f` here would mean this one site treats 0 as "no
         * ceiling", while the start check above, profile_feasibility.c and
         * profiles_http.c all treat 0 as "every rate is over it". Same value,
         * opposite policy, inside one feature. Fail-closed is the agreed
         * reading (an uncommissioned zone should not fire), so this warning
         * follows it: a zone whose ceiling was zeroed mid-firing is exactly
         * the case worth shouting about, and skipping it was the quietest
         * possible response to it. */
        bool have_ceiling = zones_config_get_max_ramp(zi, &ceiling);
        const profile_segment_t *seg =
            (s_exec.segment_index < s_exec.profile.segment_count)
                ? &s_exec.profile.segments[s_exec.segment_index]
                : NULL;
        bool now_infeasible = have_ceiling && seg && seg->ramp_c_per_hr > ceiling;
        if (now_infeasible && !z->max_ramp_warned) {
            ESP_LOGW(TAG, "OPERATOR ACTION MID-FIRING: zone %u max_ramp_c_per_hr lowered to %.1f, below the "
                          "current segment's %.1f C/hr -- the running ramp is UNCHANGED, this only flags that "
                          "it now exceeds the configured ceiling", zi, (double)ceiling, (double)seg->ramp_c_per_hr);
            z->max_ramp_warned = true;
        } else if (!now_infeasible) {
            z->max_ramp_warned = false;
        }
    }

    return changed;
}

/* One counter comparison per tick, and on the overwhelmingly common
 * unchanged path that is the entire cost -- no getters, no config walk, no
 * NVS. Must be called with s_exec.lock held, from the RUNNING path only:
 * a PAUSED or FAULTED run has no control math to keep bumpless, and picking
 * the edit up when it resumes (via this same path) is both simpler and
 * closer to what the operator expects. */
/* The four per-zone executor thresholds the owner asked to stop being magic
 * numbers (v8). Unlike the guard thresholds -- which thermal_guard.c
 * substitutes for, so its constants stay the single source of the default --
 * these are this module's own numbers, so the 0 -> named-default substitution
 * belongs here. A zone that has never been configured, or an index past
 * thermo_count, reads exactly the constant that was hardcoded before. */
static float exec_threshold(uint8_t zone_index, int which)
{
    float bb = 0.0f, cool_margin = 0.0f, cool_hold = 0.0f, ramp_lock = 0.0f;
    (void)zones_config_get_executor_thresholds(zone_index, &bb, &cool_margin, &cool_hold, &ramp_lock);
    switch (which) {
    case 0: return (bb > 0.0f) ? bb : PROFILE_EXECUTOR_HYSTERESIS_C;
    case 1: return (cool_margin > 0.0f) ? cool_margin : PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C;
    case 2: return (cool_hold > 0.0f) ? cool_hold : PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S;
    default: return (ramp_lock > 0.0f) ? ramp_lock : PROFILE_EXECUTOR_RAMP_LOCK_BAND_C;
    }
}
#define EXEC_BANGBANG_HYSTERESIS_C(zi) exec_threshold((zi), 0)
#define EXEC_COOLING_MARGIN_C(zi)      exec_threshold((zi), 1)
#define EXEC_COOLING_HOLD_S(zi)        exec_threshold((zi), 2)
#define EXEC_RAMP_LOCK_BAND_C(zi)      exec_threshold((zi), 3)

/* Shared PID-family per-zone tick body -- everything downstream of "which
 * pid_cfg_t to run this tick under" (PID_EXPANSION_PLAN.md Phase 3 wiring):
 * the pid_update_terms() call itself, TODO.md 6A.2's cooling-limited
 * diagnostic, and 6A.5's load-cap credit payback. ZONE_CONTROL_MODE_PID
 * calls this with cfg == &z->pid_cfg unchanged (so its behavior is
 * byte-for-byte what it always was -- this refactor changes nothing on that
 * path, only where the code lives). ZONE_CONTROL_MODE_PID_FUZZY calls it
 * with a per-tick fuzzy-adjusted copy. Extracted once instead of duplicated,
 * per PID_CONTROL.md's "each zone with its own instance of the three pure
 * modules" -- fuzzy is a variant of the PID module, not a second copy of
 * this several-hundred-line body that could drift from the original. */
static float pid_family_zone_tick(zone_runtime_t *z, uint8_t zi, const pid_cfg_t *cfg,
                                  bool sensor_ok_zi, float dt_s, uint32_t dt_ms,
                                  bool *out_want_relay_on)
{
    float duty = 0.0f;
    if (sensor_ok_zi) {
        /* TODO.md 6A.2's feedforward. 0.0f -- byte-for-byte the behaviour of
         * every firing before this -- for any zone with no identified
         * model, which is every zone that has never been autotuned. Passed
         * IN to the controller so the clamp, the anti-windup
         * conditional-integration test and the term breakdown all see the
         * same number the element is driven from; the value handed over
         * here is what /api/control reports as "ff", so the operator can
         * read the P/I/D/FF split and see how much of the duty is the model
         * and how much is the loop correcting it. */
        float u_ff = zone_feedforward(z, zi, s_exec.target_c, s_exec.target_rate_c_per_s);
        /* Opus review, blocker 2: the coupled system's MEMBERSHIP (which
         * zones are in it, or whether this zone qualifies at all) can change
         * every tick -- heat_blocked alone is refreshed unconditionally each
         * tick by apply_relay() (see its own doc comment), so a flapping
         * interlock or OTA heat-block can flip a neighbour's qualification
         * at tick rate. That steps the WHOLE hold term with nothing to damp
         * it (measured on this file's own tests: up to ~10 duty points in a
         * single tick, landing on every remaining zone at once).
         *
         * Re-seeding here -- reusing seed_bumpless_with_ff(), the SAME
         * bump-transfer mechanism already used for mode transitions and
         * resume (see its own doc comment) -- was chosen over adding a
         * second mechanism (hysteresis/dwell on membership) for two
         * reasons: (1) the reviewer's own instruction to reuse rather than
         * duplicate, and (2) hysteresis only delays the eventual step, it
         * does not remove it, and would need new dwell-timer state on top
         * of the flag this already reads. Re-seeding does not prevent
         * membership from changing -- a flapping interlock still flips the
         * FEEDFORWARD TARGET every tick, which is a separate, pre-existing
         * hazard this fix does not silently paper over (see apply_relay()'s
         * heat_blocked comment) -- but it guarantees the transition itself
         * never shows up as a COMMANDED DUTY step: the integral is
         * re-seeded so THIS tick's pid_update_terms() call below reproduces
         * z->duty (the duty already being commanded) to within the same
         * anti-windup tolerance every other bumpless seed in this file
         * already accepts, and subsequent ticks converge toward the new
         * target smoothly rather than jumping to it. z->duty, not
         * z->last_pid_terms.ff, is deliberately used as u_desired: it is
         * the actual physical thing that must not step. */
        if (z->ff_membership_changed) {
            seed_bumpless_with_ff(z, zi, z->duty);
        }
        duty = pid_update_terms(&z->pid_state, cfg, s_exec.target_c, z->actual_c, dt_s,
                                u_ff, &z->last_pid_terms);
    }
    /* TODO.md 6A.2's cooling-limited diagnostic. Checked against the RAW
     * duty pid_update_terms() just returned, before the load-cap boost
     * below can add anything to it -- a boosted duty is not "the loop asked
     * for heat," it is "another zone's deferred credit landed here," and
     * boost only ever makes duty larger, never masks a genuine 0. */
    if (sensor_ok_zi && duty <= 0.0f &&
        z->actual_c > s_exec.target_c + EXEC_COOLING_MARGIN_C(zi)) {
        z->cooling_limited_hold_s += dt_s;
    } else {
        z->cooling_limited_hold_s = 0.0f;
    }
    z->cooling_limited = z->cooling_limited_hold_s >= EXEC_COOLING_HOLD_S(zi);
    /* Pay back any load-cap-deferred on-time as a duty boost -- only
     * actually consumed below if this tick turns out to open a fresh
     * window (heater_output_duty() only reads the duty argument at a
     * window boundary; a boost handed to it mid-window is silently
     * ignored, so crediting it back here unconditionally and only debiting
     * on a real boundary keeps the books exact even if several ticks pass
     * between boundaries). */
    float boosted_duty = duty;
    float credit_ms = 0.0f;
    /* sensor_ok_zi gates this the same as the raw PID compute above:
     * without it, a zone that accrued load-cap credit (TODO.md 6A.5) and
     * then lost its thermocouple would have `duty` correctly held at 0.0f
     * by the `if (sensor_ok_zi)` above, but this block ran unconditionally
     * and could still boost `boosted_duty` up to 1.0f from the credit
     * alone -- commanding full output on a dead sensor, the exact case the
     * BANGBANG branch below explicitly refuses ("want_raw = false; no
     * trustworthy reading -> never command heat"). PID had no equivalent
     * until now.
     *
     * The credit itself is left untouched rather than forfeited: it
     * represents on-time this zone was denied by the load cap, a
     * bookkeeping fact that has nothing to do with whether the
     * thermocouple is currently readable. Discarding it would
     * double-penalize the zone -- once for losing its window to the cap,
     * again for a sensor fault that is very likely transient (TODO.md
     * 6A.3's SPI retry/debounce). Leaving deferred_on_ms as-is means the
     * credit is simply not spent this tick and is still there to pay back
     * once the sensor (and therefore sensor_ok_zi) recovers. */
    if (sensor_ok_zi && z->deferred_on_ms > 0.0f && z->heater_cfg.window_ms > 0) {
        float window_ms_f = (float)z->heater_cfg.window_ms;
        credit_ms = z->deferred_on_ms;
        float max_credit_ms = (1.0f - boosted_duty) * window_ms_f;
        if (credit_ms > max_credit_ms) credit_ms = max_credit_ms;
        if (credit_ms < 0.0f) credit_ms = 0.0f;
        boosted_duty += credit_ms / window_ms_f;
    }
    uint32_t elapsed_before = z->heater_state.window_elapsed_ms;
    bool was_started = z->heater_state.window_started;
    *out_want_relay_on = heater_output_duty(&z->heater_state, &z->heater_cfg, boosted_duty, dt_ms);
    /* A fresh window opened this tick iff window_elapsed_ms got reset to 0
     * -- the only place heater_output_duty() sets it to exactly 0 is the
     * new-window branch (see its comment); 1Hz ticks against a >=1s window
     * make an accumulation-only 0 practically impossible. Only then did
     * boosted_duty actually get baked into on_ms_this_window, so only then
     * is the credit actually spent. */
    if (credit_ms > 0.0f && z->heater_state.window_elapsed_ms == 0 &&
        (!was_started || elapsed_before > 0)) {
        z->deferred_on_ms -= credit_ms;
        if (z->deferred_on_ms < 0.0f) z->deferred_on_ms = 0.0f;
    }
    return duty;
}

/* ZONE_CONTROL_MODE_PID_FUZZY's per-tick gain computation
 * (PID_EXPANSION_PLAN.md Phase 3, "two wiring hazards found in review").
 *
 * Hazard 1 (error_rate_c_per_s producer): pid.c's own d_filtered is
 * derivative-on-measurement, low-pass filtered through d_filter_tau_s --
 * exactly the signal PID_EXPANSION_PLAN.md says to reuse rather than have
 * this file invent a raw per-tick finite difference (which would feed the
 * rule table unfiltered ADC noise and chatter gains cell-to-cell). Since
 * error = setpoint - measurement, d(error)/dt = -d(measurement)/dt whenever
 * the setpoint is locally constant -- and pid.c's own d_filtered is defined
 * as exactly -d(measurement)/dt (see pid_update_terms()'s raw_d), so
 * z->pid_state.d_filtered IS error_rate_c_per_s under that same
 * "setpoint locally constant" assumption pid.c already makes for its own D
 * term. Read BEFORE this tick's pid_update_terms() call runs (so it reflects
 * up through last tick, one tick of lag on an already ~30s-time-constant
 * filter -- immaterial) rather than after, because gains have to be chosen
 * before the call that uses them.
 *
 * The dSP/dt decision, stated explicitly per the plan's requirement: this
 * deliberately does NOT add the setpoint's own ramp rate
 * (s_exec.target_rate_c_per_s) into error_rate_c_per_s. During a profile
 * ramp the setpoint is moving at a known, constant, non-disturbance rate;
 * folding it in would make a perfectly ordinary firing register as "rising"
 * or "falling" on the rate axis for the entire ramp, for a reason that has
 * nothing to do with plant behavior the fuzzy layer should be reacting to.
 * pid.c's own derivative-on-measurement choice (not derivative-on-error) is
 * the same judgment call for the same reason -- a moving setpoint must not
 * by itself look like a disturbance -- and this reuses that precedent rather
 * than re-deriving a different answer for the same question. The
 * measurement's own filtered rate can still be large during a
 * well-tracked ramp (the actual temperature IS climbing at close to the
 * ramp rate) -- that is real, physical, filtered signal, not noise, and it
 * is exactly what hazard 2's rescaled RATE_BAND_C_PER_S below is sized to
 * treat as unremarkable rather than "large."
 *
 * Hazard 3 (bump transfer on a Ki move): the rule table can legitimately
 * land on a different cell tick to tick as error/error_rate drift, so the
 * effective Ki pid_fuzzy_adjust() returns can change every tick, and
 * pid_state.integral persists across that change -- i_term = ki*integral
 * would then step discontinuously the instant Ki moves. Rather than route
 * every such move through pid_seed_bumpless() (which re-derives the
 * integral from a *desired output*, appropriate for a deliberate
 * discontinuity like a mode change, not for a per-tick nudge), this rescales
 * the integral inversely so ki_old*integral == ki_new*integral' -- the I
 * term's actual contribution to duty is unchanged by the rescale itself,
 * only by the (deliberate, bounded) change in Ki. z->fuzzy_prev_effective_ki
 * tracks "effective Ki last tick" across the mode-change/reseed paths in
 * reload_zone_config()/resume() too, so this stays correct after any of
 * those discontinuities as well. */
static void pid_fuzzy_prepare_gains(zone_runtime_t *z, uint8_t zi, pid_cfg_t *out_cfg)
{
    *out_cfg = z->pid_cfg; /* d_filter_tau_s/b/pid_range_c untouched -- only kp/ki/kd move */

    float error_c = s_exec.target_c - z->actual_c;
    float error_rate_c_per_s = z->pid_state.d_filtered; /* hazard 1, see comment above */

    float strength_pct_f = 0.0f;
    (void)zones_config_get_fuzzy_strength_pct(zi, &strength_pct_f);
    /* Defence-in-depth on the duty path: zones_http.c validates this on load
     * so a non-finite value should never reach here, but if one ever did,
     * both clamp comparisons below are false for NaN and (uint8_t)(NaN+0.5f)
     * is undefined behaviour -- guard it explicitly rather than trust the
     * loader stayed the only path in. */
    uint8_t strength_pct = (!isfinite(strength_pct_f)) ? 0
                          : (strength_pct_f < 0.0f) ? 0
                          : (strength_pct_f > 100.0f) ? 100
                          : (uint8_t)(strength_pct_f + 0.5f);

    float adj_kp = z->pid_cfg.kp, adj_ki = z->pid_cfg.ki, adj_kd = z->pid_cfg.kd;
    pid_fuzzy_adjust(error_c, error_rate_c_per_s, z->pid_cfg.kp, z->pid_cfg.ki, z->pid_cfg.kd,
                     strength_pct, &adj_kp, &adj_ki, &adj_kd);

    /* Hazard 3's bump transfer, via pid.c's own pid_rescale_integral_for_new_ki()
     * (see its header comment for why this is the right tool, not
     * pid_seed_bumpless()). z->fuzzy_prev_effective_ki starting at 0.0f on a
     * cold start/mode change/reseed makes this a no-op right after any of
     * those -- pid_reset()/pid_seed_bumpless() already gave the integral a
     * correct starting value for THAT discontinuity, so rescaling again on
     * top of a value just deliberately set would be wrong, not extra-safe. */
    pid_rescale_integral_for_new_ki(&z->pid_state, z->fuzzy_prev_effective_ki, adj_ki);
    z->fuzzy_prev_effective_ki = adj_ki;

    out_cfg->kp = adj_kp;
    out_cfg->ki = adj_ki;
    out_cfg->kd = adj_kd;
}

/* How long the PC link may stay silent before a running firing is aborted.
 * Operator-settable since v8 (one global field, not per-zone -- the link is
 * one wire to one PC); 0 keeps the constant this was before. */
static uint32_t pc_link_abort_silence_ms(void)
{
    float cfg_ms = 0.0f;
    if (zones_config_get_pc_link_abort_silence_ms(&cfg_ms) && cfg_ms > 0.0f) {
        return (uint32_t)cfg_ms;
    }
    return PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS;
}

static void reload_config_if_changed(void)
{
    uint32_t gen = zones_config_generation();
    if (gen == s_exec.config_generation) {
        return;
    }
    uint32_t prev = s_exec.config_generation;
    s_exec.config_generation = gen;

    uint8_t rechecked = 0;
    uint8_t changed_mask = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        rechecked++;
        if (reload_zone_config(zi)) {
            changed_mask |= (uint8_t)(1u << zi);
        }
    }

    /* Logged even when changed_mask is 0 -- an edit to a zone this run isn't
     * driving, or to a field the executor doesn't consume (a zone name, the
     * calibration offset, which is read fresh every tick anyway), still moves
     * the generation, and the operator log should show that settings were
     * touched during a firing at all, not only when it altered this run. */
    ESP_LOGI(TAG, "config reloaded mid-run (TODO.md 6A.7): generation %lu -> %lu, %u active zone(s) re-read, "
                  "zones changed 0x%02X",
             (unsigned long)prev, (unsigned long)gen, rechecked, changed_mask);
}

/* ---- control task ----------------------------------------------------------- */

static void executor_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PROFILE_EXECUTOR_TICK_MS));

        xSemaphoreTake(s_exec.lock, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        s_exec.last_tick_tick = now; /* guard 9 -- updated every iteration regardless of run state */

        if (s_exec.state != PROFILE_EXEC_RUNNING) {
            force_all_relays_off();
            /* honor_leave_on=false unconditionally: PAUSED, FAULTED and every
             * post-DONE tick land here, and none of those is the one clean
             * ending (segment-stepping's DONE branch below) that is allowed
             * to honor a segment's leave_on_at_end -- see io_seg_finish()'s
             * doc comment. A segment already finished (DONE already swept
             * it, or it never started) costs nothing extra here. */
            io_segs_force_all_off(false);
            /* Backstop for K4, the same shape as the force-off above it: any
             * state that is not RUNNING must not be holding the safety
             * processor's permission to heat, whether or not the transition
             * that got here remembered to release it. Sends at most one
             * frame -- heat_enable_release() only puts anything on the wire
             * on the last-claimant edge, so every tick after the first is
             * free rather than a release frame per tick. PAUSED lands here too, which is what a pause is
             * supposed to mean -- see profile_executor_pause(). */
            heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
            /* PID_EXPANSION_PLAN.md Phase 7a: the moment a run first lands
             * in DONE or FAULTED, persist its firing stats -- this is the
             * "run completion" write, not waiting on the operator to press
             * Stop (profile_executor_halt() covers the OTHER ending, an
             * operator stop straight out of RUNNING/PAUSED that never
             * passes through here). PAUSED also lands in this branch every
             * tick and must NOT finalize -- a paused run can still resume --
             * hence the explicit state check rather than relying on
             * firing_stats_maybe_finalize()'s own (looser) IDLE-only guard. */
            bool fs_need_persist = false;
            profile_firing_run_record_t fs_rec;
            if (s_exec.state == PROFILE_EXEC_DONE || s_exec.state == PROFILE_EXEC_FAULTED) {
                fs_need_persist = firing_stats_maybe_finalize(&fs_rec);
            }
            xSemaphoreGive(s_exec.lock);
            if (fs_need_persist) {
                firing_stats_persist(&fs_rec);
            }
            continue;
        }

        uint32_t dt_ms = ticks_to_ms(now - s_exec.prev_control_tick);
        if (dt_ms == 0) {
            dt_ms = PROFILE_EXECUTOR_TICK_MS;
        }
        s_exec.prev_control_tick = now;
        float dt_s = (float)dt_ms / 1000.0f;
        /* Real elapsed time for this run -- counts through a ramp-lock
         * stall (that's genuine wall-clock time passing while the plan's
         * remaining_s estimate quietly falls behind), only excluded while
         * PAUSED, since this whole block is skipped then. See
         * profile_exec_status_t.total_elapsed_s. */
        s_exec.total_elapsed_s += (uint32_t)(dt_s + 0.5f);

        /* TODO relay/IO segments: tick every active NON-BLOCKING segment's
         * own hold timer, independent of whichever ramp/dwell segment is
         * current below -- see io_segs_tick()'s doc comment. */
        io_segs_tick(dt_s);

        /* --- Read every physical channel (raw), then combine per zone
         * (TODO.md 10.8) into that zone's control temperature ------------
         * Two index spaces below on purpose: ch_raw_c/ch_sensor_ok are
         * indexed by physical MAX31856 CHANNEL (whatever the bus/sim
         * backend answered), raw_c/sensor_ok stay indexed by ZONE like
         * every downstream consumer of them already expects (ramp-lock,
         * thermal_guard_input_t below, etc.) -- only their MEANING changed,
         * from "zone zi's one hard-wired channel" to "zone zi's combined
         * reading across every channel its thermo_mask names". Nothing
         * downstream needed to change to pick that up. */
        float ch_raw_c[MAX31856_CHANNEL_COUNT];
        bool ch_sensor_ok[MAX31856_CHANNEL_COUNT];
        for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
            ch_raw_c[ci] = NAN;
            ch_sensor_ok[ci] = false;
        }
        if (sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->initialized)) {
            MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
            size_t count = 0;
            if (sim_backend_enabled()) {
                sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
            } else {
                MAX31856_read_all(s_exec.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
            }
            for (size_t i = 0; i < count; i++) {
                uint8_t ci = readings[i].channel;
                /* Recorded for every physical channel the bus answered,
                 * regardless of which (if any) zone claims it this tick --
                 * unlike the pre-10.8 "!s_exec.zones[zi].active continue"
                 * this loop used to have, filtering by a single zone's
                 * active flag here would blind every OTHER zone whose
                 * thermo_mask also names this channel. Per-zone gating
                 * happens below, once, per zone -- not here, once per
                 * channel that happens to alias the same index as a zone
                 * that isn't running. */
                if (ci >= MAX31856_CHANNEL_COUNT) continue;
                ch_raw_c[ci] = readings[i].tc_temperature_c;
                /* THERMO_FAULT_OPEN|OVUV|TCRANGE make the temperature
                 * meaningless per MAX31856Reading's own doc comment --
                 * mirrors thermal_guard.h's sensor_ok contract without
                 * pulling those bit constants into thermal_guard.c. */
                bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
                ch_sensor_ok[ci] = !readings[i].spi_failed && !isnan(ch_raw_c[ci]) && !fault_bits_bad;
            }
        }
        float raw_c[MAX31856_CHANNEL_COUNT];    /* per ZONE: this zone's combined raw reading */
        bool sensor_ok[MAX31856_CHANNEL_COUNT]; /* per ZONE: true iff >=1 assigned channel is valid --
                                                  * this IS guard 6's extended "invalid" definition
                                                  * (TODO.md 10.8: "all assigned thermocouples for
                                                  * this zone are invalid"), computed once here rather
                                                  * than inside thermal_guard.c, which already takes
                                                  * sensor_ok as an opaque caller-decided bool (see
                                                  * thermal_guard.h's thermal_guard_input_t comment) --
                                                  * nothing in thermal_guard.c needed to change. */
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            raw_c[zi] = NAN;
            sensor_ok[zi] = false;
            if (!s_exec.zones[zi].active) continue;
            /* Live read every tick, not cached in zone_runtime_t -- same
             * "no hardware-safety handover needed" reasoning
             * zones_config_apply_cal()'s per-tick call already relies on:
             * unlike relay_mask, a thermo_mask change never needs a
             * force-off-the-old-mask dance (see reload_zone_config()),
             * because it only ever affects what this zone READS, never
             * what it drives. */
            uint8_t tmask = 0;
            zones_config_get_thermo_mask(zi, &tmask);
            bool valid = false;
            float combined = thermo_combine(ch_raw_c, ch_sensor_ok, MAX31856_CHANNEL_COUNT, tmask, &valid);
            raw_c[zi] = combined;
            sensor_ok[zi] = valid;
            s_exec.zones[zi].actual_valid = valid;
            s_exec.zones[zi].actual_c = valid ? zones_config_apply_cal(zi, combined) : NAN;

            /* Cross-zone coupling filter (PID_EXPANSION_PLAN.md 2c/Phase 3b,
             * BLOCKING review finding 3): updated exactly once per control
             * tick, via this helper rather than inline inside
             * zone_feedforward() -- see coupling_filter_tick()'s own doc
             * comment for why. */
            coupling_filter_tick(&s_exec.zones[zi], valid, dt_s);
        }

        /* --- Config reload (TODO.md 6A.7) ----------------------------------
         * Placed after the readings are stored and before any control math:
         * the bumpless seed a gain change needs is only honest against THIS
         * tick's measurement, and every gain/mode/mask/threshold the passes
         * below read must already be the post-edit one, so an edit can never
         * be half-applied across a single tick's decide/apply split. */
        reload_config_if_changed();

        /* --- Ramp-lock (TODO.md 6A.5(d)): is every active, not-already-
         * faulted zone within band of the CURRENT shared target? Only
         * zones still actually being driven count -- a zone that already
         * tripped its own guard is guard 1/2/4/7's problem, not ramp-lock's
         * to also hold the whole firing hostage over. -------------------- */
        bool lock_ok = true;
        uint8_t lagging = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            if (!sensor_ok[zi] || fabsf(s_exec.zones[zi].actual_c - s_exec.target_c) > EXEC_RAMP_LOCK_BAND_C(zi)) {
                lock_ok = false;
                lagging |= (uint8_t)(1u << zi);
            }
        }
        s_exec.ramp_lock_held = !lock_ok;
        s_exec.ramp_lock_lagging_mask = lagging;

        /* --- Ramp/dwell segment stepping (shared across all active zones) - */
        bool segment_changed = false; /* reboot breadcrumb: worth its own NVS write, see below */
        const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index];
        /* Zero unless a ramp is actually being commanded this tick. Falling
         * through this default covers dwelling, a step segment (target_c jumps
         * in one tick -- there is no sustained rate for tau to act on), and
         * ramp-lock: while the lock holds the setpoint deliberately is NOT
         * moving, so paying feedforward for a climb that is not happening
         * would push duty up on exactly the zones the lock is waiting for the
         * laggards to catch up with. */
        s_exec.target_rate_c_per_s = 0.0f;
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            /* Owner's request, verbatim (profiles_http.h): a relay/IO segment
             * is not a temperature step at all, so none of the ramp-lock
             * machinery above (which exists solely to keep the shared
             * SETPOINT from outrunning a lagging zone) applies to it -- it
             * always advances on wall-clock time, lock_ok or not. target_c/
             * dwelling/segment_elapsed_s (the ZONE_RAMP machine's own state)
             * are deliberately left untouched here. */
            if (!s_exec.io_segs[s_exec.segment_index].active) {
                io_seg_start(s_exec.segment_index, seg); /* first tick this segment is current */
            }
            bool ready_to_advance;
            if (seg->io_blocking) {
                /* Behaves exactly like a dwell: the shared schedule does not
                 * move to the next segment until this one's own hold time has
                 * elapsed -- "blocking... before the next segment", the
                 * owner's own wording. Reuses segment_elapsed_s (this
                 * segment owns it exclusively while current, same as a
                 * ZONE_RAMP dwell does) rather than the per-segment
                 * remaining_s a non-blocking segment uses, since a blocking
                 * segment's timer IS the schedule's timer. */
                s_exec.segment_elapsed_s += (uint32_t)(dt_s + 0.5f);
                ready_to_advance = s_exec.segment_elapsed_s >= seg->dwell_min * 60u;
            } else {
                /* "Runs WITH the next segment": advance on the very same tick
                 * it starts. Its own on/off state keeps running afterward,
                 * independent of segment_index -- see io_segs_tick() above. */
                ready_to_advance = true;
            }
            if (ready_to_advance) {
                if (seg->io_blocking) {
                    /* A blocking segment's own command always ends the moment
                     * the schedule moves past it -- there is nothing else
                     * still "running alongside" it for leave_on_at_end to
                     * apply to (validate_io_segment() already refuses that
                     * flag on a blocking segment at save time; this is belt
                     * and braces, not a second decision point). */
                    io_seg_finish(s_exec.segment_index, false);
                }
                s_exec.segment_index++;
                s_exec.segment_elapsed_s = 0;
                segment_changed = true;
                if (s_exec.segment_index >= s_exec.profile.segment_count) {
                    s_exec.state = PROFILE_EXEC_DONE;
                    force_all_relays_off();
                    /* The one path allowed to honor leave_on_at_end: the
                     * schedule reached its own natural end with every
                     * segment accounted for, exactly as planned. */
                    io_segs_force_all_off(true);
                    release_profile_relay_claim();
                    run_snapshot_buf_t done_snap;
                    capture_run_snapshot(&done_snap);
                    xSemaphoreGive(s_exec.lock);
                    run_state_note(RUN_STATE_PHASE_DONE, &done_snap.snap);
                    continue;
                }
                seg = &s_exec.profile.segments[s_exec.segment_index];
            }
        } else if (lock_ok) {
            s_exec.segment_elapsed_s += (uint32_t)(dt_s + 0.5f);
            if (!s_exec.dwelling) {
                float new_target;
                if (seg->ramp_c_per_hr <= 0.0f) {
                    new_target = seg->target_c;
                } else {
                    float direction = (seg->target_c >= s_exec.target_c) ? 1.0f : -1.0f;
                    new_target = s_exec.target_c + direction * seg->ramp_c_per_hr * (dt_s / 3600.0f);
                    bool reached = (direction > 0.0f) ? (new_target >= seg->target_c) : (new_target <= seg->target_c);
                    if (reached) {
                        new_target = seg->target_c;
                    } else {
                        /* The commanded rate, signed and in the units the
                         * feedforward term wants. Only claimed while the ramp
                         * still has distance left to run: the tick that
                         * arrives at the segment target is already a partial
                         * one, and the ticks after it are a dwell. */
                        s_exec.target_rate_c_per_s = direction * seg->ramp_c_per_hr / 3600.0f;
                    }
                }
                s_exec.target_c = new_target;
                if (s_exec.target_c == seg->target_c) {
                    s_exec.dwelling = true;
                    s_exec.segment_elapsed_s = 0;
                }
            } else {
                s_exec.target_c = seg->target_c;
                if (s_exec.segment_elapsed_s >= seg->dwell_min * 60u) {
                    s_exec.segment_index++;
                    if (s_exec.segment_index >= s_exec.profile.segment_count) {
                        s_exec.state = PROFILE_EXEC_DONE;
                        force_all_relays_off();
                        /* This is ALSO a clean end -- the run's LAST segment
                         * happened to be a ZONE_RAMP dwell, but an earlier
                         * non-blocking relay/IO segment may still be active
                         * and running alongside it (that's the whole point of
                         * non-blocking). Same one path allowed to honor
                         * leave_on_at_end as the RELAY_IO branch's own DONE
                         * transition above -- see io_seg_finish()'s doc
                         * comment. */
                        io_segs_force_all_off(true);
                        release_profile_relay_claim();
                        /* A clean end, and it MUST be recorded as one: a
                         * completed firing whose record still says RUNNING
                         * would greet the next boot as an interrupted one and
                         * teach the operator to ignore the warning. */
                        run_snapshot_buf_t done_snap;
                        capture_run_snapshot(&done_snap);
                        xSemaphoreGive(s_exec.lock);
                        run_state_note(RUN_STATE_PHASE_DONE, &done_snap.snap);
                        continue;
                    }
                    s_exec.dwelling = false;
                    s_exec.segment_elapsed_s = 0;
                    seg = &s_exec.profile.segments[s_exec.segment_index];
                    /* target_c stays where it is -- that's the new segment's ramp start. */
                    /* A segment boundary is the transition that most changes
                     * what an interrupted record would say, so it gets its own
                     * write rather than waiting for the periodic refresh. */
                    segment_changed = true;
                }
            }
        }

        /* --- Firing quality stats accumulation (PID_EXPANSION_PLAN.md Phase
         * 7a), per active zone --------------------------------------------
         * Placed here: target_c/dwelling/segment_index are already final for
         * this tick (the segment-stepping block above has run), and
         * s_exec.zones[zi].actual_c/actual_valid were set by the reading
         * loop earlier this same tick. The per-zone math itself lives in
         * firing_stats_zone_tick() (a few hundred lines up, pure and
         * argument-driven) specifically so a host test can drive a synthetic
         * error sequence through it tick-by-tick without a real FreeRTOS
         * task loop -- see that function's own doc comment. */
        s_exec.fs_target_min_c = isnan(s_exec.fs_target_min_c) ? s_exec.target_c : fminf(s_exec.fs_target_min_c, s_exec.target_c);
        s_exec.fs_target_max_c = isnan(s_exec.fs_target_max_c) ? s_exec.target_c : fmaxf(s_exec.fs_target_max_c, s_exec.target_c);
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            firing_stats_zone_tick(&s_exec.zones[zi], s_exec.target_c, s_exec.dwelling, s_exec.total_elapsed_s,
                                    s_exec.segment_index, dt_s);
        }

        /* --- Control mode, per active zone (pass 1: decide, don't apply yet)
         * -------------------------------------------------------------------
         * Split from the apply+guard pass below so the load cap (TODO.md
         * 6A.5 load-staggering) can see every active zone's raw want-on
         * before deciding which ones actually get the relay this tick. */
        bool want_relay_on[MAX31856_CHANNEL_COUNT] = {0};
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            zone_runtime_t *z = &s_exec.zones[zi];

            float duty = 0.0f;
            z->last_pid_terms = (pid_terms_t){0}; /* only ZONE_CONTROL_MODE_PID/PID_FUZZY below fill this in */
            switch (z->control_mode) {
            case ZONE_CONTROL_MODE_PID: {
                /* &z->pid_cfg directly -- no copy, no adjustment. Bit-for-bit
                 * the same call this always was; see pid_family_zone_tick()'s
                 * header comment for why the body moved but the behavior
                 * didn't. */
                duty = pid_family_zone_tick(z, zi, &z->pid_cfg, sensor_ok[zi], dt_s, dt_ms,
                                            &want_relay_on[zi]);
                break;
            }
            case ZONE_CONTROL_MODE_PID_FUZZY: {
                /* strength_pct == 0 (or a non-finite error/rate -- faulted
                 * thermocouple) makes pid_fuzzy_prepare_gains() hand back
                 * z->pid_cfg's own kp/ki/kd unchanged (pid_fuzzy_adjust()'s
                 * documented contract), so this path degrades to exactly the
                 * PID case above at the safe default -- same call, same
                 * shared body, only the gains it's given can differ. */
                pid_cfg_t fuzzy_cfg;
                pid_fuzzy_prepare_gains(z, zi, &fuzzy_cfg);
                duty = pid_family_zone_tick(z, zi, &fuzzy_cfg, sensor_ok[zi], dt_s, dt_ms,
                                            &want_relay_on[zi]);
                break;
            }
            case ZONE_CONTROL_MODE_BANGBANG: {
                bool want_raw = z->relay_commanded_on;
                if (sensor_ok[zi]) {
                    if (z->actual_c < s_exec.target_c - EXEC_BANGBANG_HYSTERESIS_C(zi)) {
                        want_raw = true;
                    } else if (z->actual_c > s_exec.target_c + EXEC_BANGBANG_HYSTERESIS_C(zi)) {
                        want_raw = false;
                    }
                } else {
                    want_raw = false; /* no trustworthy reading -> never command heat */
                }
                want_relay_on[zi] = heater_output_bangbang(&z->heater_state, &z->heater_cfg, want_raw, dt_ms);
                duty = want_relay_on[zi] ? 1.0f : 0.0f;
                /* Cooling-limited is a PID-mode-only diagnostic (see its
                 * field comment) -- clear it rather than let a stale true
                 * from a PID period before a mode switch linger. */
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                break;
            }
            case ZONE_CONTROL_MODE_OFF:
            default:
                want_relay_on[zi] = false;
                duty = 0.0f;
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                break;
            }
            z->duty = duty;
        }

        /* --- Load cap (TODO.md 6A.5 load-staggering): if more active zones
         * want on than zones_config_get_max_simultaneous_relays() allows,
         * suppress the excess -- lowest deferred_on_ms (least "owed") first,
         * so debt self-corrects over time instead of one zone starving
         * forever. Suppressed PID zones accumulate the denied on-time in
         * deferred_on_ms and get it back on their own next window (pass 1,
         * above, next tick); bang-bang zones accumulate it too but it's
         * never paid back -- no window to pay it into, so it's a documented
         * "lost, not deferred" gap for that mode only. 0 = unlimited,
         * unchanged/no-op behavior (matches every run before this pass). */
        uint8_t cap = zones_config_get_max_simultaneous_relays();
        if (cap > 0) {
            uint8_t wanted = 0;
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (s_exec.zones[zi].active && !s_exec.zones[zi].faulted && want_relay_on[zi]) wanted++;
            }
            while (wanted > cap) {
                int8_t victim = -1;
                for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                    if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted || !want_relay_on[zi]) continue;
                    if (victim < 0 || s_exec.zones[zi].deferred_on_ms < s_exec.zones[victim].deferred_on_ms) {
                        victim = (int8_t)zi;
                    }
                }
                if (victim < 0) break; /* shouldn't happen given wanted > cap, but don't loop forever */
                want_relay_on[victim] = false;
                s_exec.zones[victim].deferred_on_ms += (float)dt_ms;
                wanted--;
            }
        }

        /* --- Apply relays + guards, per active zone -------------------------- */
        bool run_faulted_this_tick = false;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && !run_faulted_this_tick; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            zone_runtime_t *z = &s_exec.zones[zi];

            apply_relay(zi, want_relay_on[zi]);

            /* Guards 1/2/3/7 all gate their multi-tick accumulation windows
             * on commanded_duty (thermal_guard.c), and this used to be fed
             * z->relay_commanded_on -- the POST-PWM, POST-load-cap relay
             * state apply_relay() just set a few lines up. heater_output_
             * duty() (heater_output.c) time-proportions any duty strictly
             * between 0 and 1 across its own window (HEATER_DEFAULT_WINDOW_
             * MS, 60s default), so relay_commanded_on drops to false on
             * every off-pulse of that PWM cycle -- and EVERY ONE of these
             * four windows resets on that same transition (thermal_guard.c:
             * guard 1/2 reset when commanded_duty < progress_duty_min,
             * guard 3 needs a CONTIGUOUS duty==0 run and resets the instant
             * duty is nonzero again, guard 7 needs a CONTIGUOUS duty>0 run
             * and resets the instant duty drops to 0). None of guard 1's
             * 300s, guard 2's 120s, guard 3's 120s or guard 7's 600s window
             * can ever complete at any duty strictly between 0 and 1 -- at
             * duty 0.6 the longest contiguous armed run is 36s. A frozen
             * thermocouple during a partial-duty firing was never caught.
             *
             * Fixed the same way autotune_engine.c's identical defect was:
             * feed the guards the INTENDED duty (z->duty, decided in this
             * tick's own "pass 1" above, before load-cap/PWM/authority ever
             * touch it), not the instantaneous post-PWM relay state. No
             * guard constant changes -- the existing windows are fine once
             * they can actually accumulate.
             *
             * Two things intentionally do NOT feed through as "intended,
             * heat commanded" here, each a deliberate choice (both ways
             * have a real argument, so both are spelled out rather than
             * assumed):
             *
             *   LOAD CAP (this tick's want_relay_on[zi] possibly forced
             *   false by the cap loop just above, z->duty left untouched):
             *   still reported as z->duty, NOT zeroed. Treated as the same
             *   kind of scheduling detail PWM chopping is -- deferred_on_ms
             *   is a genuine, self-correcting promise that this zone gets
             *   its owed on-time back, so a HEALTHY load-cap-throttled zone
             *   should still show real rise across guard 1's window (it is
             *   getting real heat, just multiplexed with other zones) and
             *   should not read as "not commanded" just because this
             *   PARTICULAR tick's relay stayed open for a sibling zone
             *   instead. The alternative (zero whenever deferred) would
             *   recreate this exact defect under a different name: a zone
             *   cycling in and out of load-cap deferral resets these
             *   windows on THAT transition instead of PWM's. The remaining
             *   risk -- a zone so persistently starved by the cap that it
             *   never gets enough real on-time to keep up with rate_cfg --
             *   is judged the CORRECT case for guard 1 to eventually catch
             *   and escalate, not silently swallow: that zone is genuinely
             *   not being heated adequately, whatever the reason.
             *
             *   AUTHORITY/INTERLOCK BLOCK (relay_authority_zone_blocked(),
             *   surfaced here as z->heat_blocked): reported as ZERO whenever
             *   z->heat_blocked reads true, want_relay_on[zi] or not.
             *   z->heat_blocked is now refreshed by apply_relay() on EVERY
             *   tick (see that function's own comment) -- an earlier version
             *   of this fix gated the zero on want_relay_on[zi] too, on the
             *   theory that heat_blocked was only fresh when want_on was
             *   true; that theory, and the gate built on it, were WRONG (a
             *   review caught it as a regression: it broke guard 3 during a
             *   real authority block by re-introducing an alternating
             *   zero/nonzero pattern at the PWM period -- see apply_relay()
             *   and profile_executor_guard_commanded_duty()'s own comments
             *   for the full account). Unlike the load cap, a block is a
             *   DETERMINISTIC, ALREADY-REPORTED fact -- the safety link down,
             *   or an earlier guard trip's own latch -- and re-deriving the
             *   same fact through guard 1/2's noisier "no rise despite
             *   commanded heat" statistics would only produce a redundant,
             *   less specific alarm (or, for a global block, one on EVERY
             *   active zone at once) on top of the real one relay_authority_
             *   on_blocked()/the per-zone latch already surfaced. Guard 3
             *   (welded contact) correctly reads this zone as duty==0 either
             *   way -- the relay genuinely is open -- and guard 7 (frozen
             *   sensor) correctly goes inert, since "is the process
             *   responding to commanded heat" is not a meaningful question
             *   while no heat is being commanded at all.
             *
             * (autotune_engine.c's own copy of this reasoning has a
             * corrected note on relay_authority_zone_blocked() not feeding
             * back into that file's duty -- see that file's comment; that
             * file's guard wiring is unchanged by this pass.) */
            float commanded_duty_for_guards = profile_executor_guard_commanded_duty(z->heat_blocked, z->duty);

            /* Guard 1's "climbing" rise requirement (thermal_guard.c: delta
             * >= rate_cfg * elapsed_min whenever error > progress_band_c) is
             * a FIXED absolute rate with no knowledge of what rate is
             * actually being commanded. Review defect 2: this whole PWM fix
             * widened guard 1's real window from "can only ever complete at
             * duty==1.0" to "completes at any duty >= progress_duty_min
             * (0.5)" -- i.e. most of a real firing -- and a perfectly
             * healthy zone tracking a modest ramp (20 C/hr = 0.33 C/min is
             * routine: candling, quartz inversion, thick ware, a large
             * kiln's own thermal lag) legitimately lags more than
             * progress_band_c behind a setpoint that is ITSELF only moving
             * that fast. Demanding rate_cfg's full 0.5 C/min (30 C/hr) rise
             * from such a zone is demanding it outrun the setpoint it is
             * chasing -- guaranteed to false-trip at 300s, and under the
             * load cap (every zone forced to a high duty, lagging further,
             * rising at roughly a fair share of the commanded rate) able to
             * trip EVERY zone in the same window at once, not a cascade.
             *
             * Fix: cap the expected rate at the commanded ramp's own rate
             * (s_exec.target_rate_c_per_s, set a few hundred lines up --
             * "Only claimed while the ramp still has distance left to run",
             * explicitly 0.0f during a dwell/step-segment/ramp-lock tick) --
             * min(configured rate_cfg, |commanded ramp rate|) -- rather than
             * changing rate_cfg's own stored value (a per-zone config field
             * read elsewhere) or thermal_guard.c's guard 1 math itself (no
             * guard constant needs to change, same as the duty fix above).
             * Deliberately NOT applied during a dwell (target_rate_c_per_s
             * is exactly 0.0f then, which would demand zero rise forever,
             * i.e. disarm guard 1 completely for a lagging dwell -- the
             * ORIGINAL "must actively catch up" case this guard exists for
             * and must not be softened): the cap only ever narrows the
             * requirement while a ramp is genuinely in progress, and is a
             * per-tick LOCAL override of a copy of z->guard_cfg -- the
             * zone's own stored guard_cfg.sanity_rate_c_per_min (read
             * elsewhere) is never mutated. */
            thermal_guard_cfg_t guard_cfg_this_tick = z->guard_cfg;
            guard_cfg_this_tick.sanity_rate_c_per_min =
                profile_executor_guard_sanity_rate(z->guard_cfg.sanity_rate_c_per_min, s_exec.target_rate_c_per_s);

            thermal_guard_input_t gin = {
                .sensor_ok = sensor_ok[zi],
                .measurement_c = raw_c[zi], /* RAW -- a calibration offset must not hide an out-of-range sensor */
                .setpoint_c = s_exec.target_c,
                .commanded_duty = commanded_duty_for_guards,
                .dt_s = dt_s,
                /* Guard 8 (TODO.md 6A.3/6A.5): this tick's whole-bus snapshot,
                 * raw and un-averaged, so a zone can be compared against its
                 * neighbours in the same chamber. Costs nothing extra -- the
                 * arrays were already filled at the top of this tick. The
                 * guard is inert unless the zone also has a configured
                 * cross_zone_max_delta_c, which is not set by default. */
                .peer_c = raw_c,
                .peer_ok = sensor_ok,
                .peer_count = MAX31856_CHANNEL_COUNT,
                .peer_index_self = zi,
            };
            if (thermal_guard_tick(&z->guard_state, &guard_cfg_this_tick, &gin)) {
                if (escalate_guard_trip(zi, z->guard_state.reason, z->guard_state.detail)) {
                    run_faulted_this_tick = true;
                }
            }

            /* Contact-cycle accounting (TODO.md 6A.1): hand relay_cycles.c
             * only what this zone has switched since the last tick. Its
             * relays switch as a group, so every relay in the mask takes the
             * same count. */
            uint32_t cycles_now = z->heater_state.cycle_count;
            if (cycles_now != z->cycles_reported) {
                uint8_t mask = 0;
                if (zones_config_get_relay_mask(zi, &mask) && mask != 0) {
                    relay_cycles_add(mask, cycles_now - z->cycles_reported);
                }
                z->cycles_reported = cycles_now;
            }
        }

        /* --- Unowned-relay sweep (TODO.md 6A.7) -----------------------------
         * Last thing in the tick that touches copper, deliberately: it must
         * judge the relay state this tick actually left behind, including a
         * guard trip's force-off above that reported an error. Running it
         * before the apply pass would have it clearing contacts the same tick
         * is about to re-command, i.e. chattering the relay it is supposed to
         * be protecting. RUNNING-only for now: the other states force
         * everything off through masks they can still name, and an IDLE
         * executor has no business second-guessing a relay an operator turned
         * on manually after a firing ended. */
        sweep_unowned_relays();

        /* Rate-limited internally (at most one NVS write per 10 min, and only
         * if something changed) -- see relay_cycles.h's flash-wear note. */
        relay_cycles_maybe_persist();

        /* --- History sample (TODO.md section 0 / 6A.9), single
         * representative zone, one per 30s ---------------------------------- */
        if (!run_faulted_this_tick && s_exec.zones[s_exec.history_zone].active &&
            ticks_to_s(now - s_exec.history_last_sample_tick) >= HISTORY_SAMPLE_PERIOD_S) {
            s_exec.history_last_sample_tick = now;
            zone_runtime_t *hz = &s_exec.zones[s_exec.history_zone];
            history_slot_t *slot = &s_exec.history[s_exec.history_head];
            history_pack(slot, ticks_to_s(now - s_exec.history_run_start_tick),
                         hz->actual_valid ? hz->actual_c : NAN, s_exec.target_c, hz->duty,
                         (uint8_t)hz->guard_state.reason);
            s_exec.history_head = (uint16_t)((s_exec.history_head + 1u) % HISTORY_MAX_SAMPLES);
            if (s_exec.history_count < HISTORY_MAX_SAMPLES) {
                s_exec.history_count++;
            }
        }

        /* --- Reboot breadcrumb (TODO.md 6A.3) -------------------------------
         * Captured under the lock, written outside it. relay_cycles_maybe_
         * persist() above already does its NVS write with the lock held, so
         * the precedent for "a flash write inside the tick" exists -- but
         * this one is easy to keep outside, and the lock also serves
         * profile_executor_get_status(), which the dashboard polls every 2 s.
         * There is no reason to make a status request wait behind an erase. */
        run_snapshot_buf_t tick_snap;
        capture_run_snapshot(&tick_snap);
        bool faulted_now = run_faulted_this_tick;
        xSemaphoreGive(s_exec.lock);

        if (faulted_now) {
            /* Ended badly, but ENDED -- the distinction the operator needs is
             * "did it stop on its own terms", and a guard trip did. The
             * guard number and reason ride along so the next boot can say
             * why without the log. */
            run_state_note(RUN_STATE_PHASE_FAULTED, &tick_snap.snap);
        } else if (segment_changed) {
            run_state_note(RUN_STATE_PHASE_RUNNING, &tick_snap.snap);
        } else {
            /* Rate-limited internally to one write per
             * RUN_STATE_REFRESH_INTERVAL_S -- see run_state.c's flash-wear
             * arithmetic. Cheap to call every tick. */
            run_state_note_progress(&tick_snap.snap);
        }
    }
}

/* ---- watchdog task (guard 9) ------------------------------------------------ */

static void watchdog_task_entry(void *arg)
{
    (void)arg;
    /* Edge-trigger for PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP -- declared
     * outside the for(;;) below so it persists across loop iterations (this
     * task never returns while running, so that's equivalent to `static`
     * here, just without implying re-entrancy this function never has), and
     * outside the switch case below because it's also read/cleared on ticks
     * that land on a different action entirely (see the reset just after the
     * switch). */
    bool s_idle_trip_logged = false;
    /* PC-link fault-source tracking (see profile_executor.h's
     * PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS doc comment): unlike the
     * safety link, SAFETY_FAULT_SRC_PC_LINK is a level read back off
     * safety_link_get_fault_sources(), not something with its own "age since
     * last change" -- uart_bridge.c's link_watchdog_task re-asserts it every
     * UART_BRIDGE_LINK_CHECK_MS while down and clears it the moment the link
     * is back (see that task), so this task has to time the level itself:
     * the tick this bit was FIRST seen asserted, persisted across loop
     * iterations same as s_idle_trip_logged above. */
    bool pc_link_was_down = false;
    TickType_t pc_link_down_since_tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_PERIOD_MS));

        /* Retry a heat-enable request that never landed (heat_enable.h). A
         * no-op unless a run is holding a claim whose REQUEST_ENABLE was
         * refused -- which is exactly the "started while the safety link was
         * down, link came back mid-run" case that would otherwise leave a
         * firing running to completion with K4 open. This task, not the
         * control task: it is the one that already runs at a slow fixed
         * period and does not hold s_exec.lock here, and a blocking link
         * exchange must not sit inside the control tick. */
        heat_enable_reconcile();

        /* LINK_PROTOCOL.md sec 8 / ROADMAP.md M6: "30 s silence aborts a
         * firing" -- distinct from, and much larger than, the 1.5 s
         * SAFETY_FAULT_SRC_SAFETY_LINK check safety_update_health() already
         * runs on every poll (that one only blocks *new* relay-on via
         * relay_authority_on_blocked(); a single dropped frame must not abort
         * a run already in progress). Queried outside s_exec.lock --
         * safety_link_get_status() takes its own lock and never blocks on the
         * far side, same "never talks to the peer" contract everything else
         * in this driver relies on. safety_link_is_stale() is the same pure
         * comparison safety_link.c uses for the 1.5 s case, just against the
         * larger threshold. */
        uint16_t safety_age_ms = SAFETY_LINK_AGE_NEVER;
        bool safety_diag_valid = false;
        uint8_t safety_diag_state = SAFETY_LINK_DIAG_STATE_INIT;
        uint8_t safety_diag_trip_reason = 0;
        if (s_exec.safety) {
            safety_link_status_t safety_status;
            if (safety_link_get_status(s_exec.safety, &safety_status) == ESP_OK) {
                safety_age_ms = safety_status.age_ms;
                /* A trip report is only actionable while the DIAG frame it
                 * came from is fresh -- diag_age_ms shares the same
                 * cached_tick as age_ms (safety_link.c), so age_ms doubles as
                 * the diag frame's own age here. Anything stale beyond
                 * SAFETY_LINK_STALE_MS is exactly the "link went silent"
                 * case the 30s check below already owns; this branch must
                 * never act on a trip read off data that old, or a silent
                 * link's LAST-KNOWN diag_state would get relabeled as a
                 * fresh trip. */
                safety_diag_valid = safety_status.diag_ever_received &&
                                     !safety_link_is_stale(safety_age_ms, SAFETY_LINK_STALE_MS);
                safety_diag_state = safety_status.diag_state;
                safety_diag_trip_reason = safety_status.diag_trip_reason;
            }
        }
        bool safety_link_silent_30s = s_exec.safety != NULL &&
                                       safety_link_is_stale(safety_age_ms, SAFETY_LINK_FIRING_ABORT_SILENCE_MS);
        /* The gap this closes (ROADMAP.md): the link staying HEALTHY while
         * the safety processor itself actively trips is a different failure
         * than the link going silent, and until now nothing here ever read
         * diag_state/diag_trip_reason at all -- relay_authority already
         * blocked new relay-on, but the run itself kept advancing its
         * schedule and both GUIs kept showing a firing in progress while the
         * kiln cooled. */
        bool safety_processor_tripped = safety_diag_valid &&
                                         safety_diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED;

        /* PC link loss (profile_executor.h's PROFILE_EXECUTOR_PC_LINK_ABORT_
         * SILENCE_MS): uart_bridge.c's link_watchdog_task asserts
         * SAFETY_FAULT_SRC_PC_LINK the whole time the PC/UART control link
         * is down and clears it the instant a frame or ACK is seen again --
         * read back here the same way safety_link_get_status() is read
         * above, outside s_exec.lock (safety_link_get_fault_sources() takes
         * its own lock and never blocks on the peer). This task has no
         * "age" for a level bit, so it times the level itself: latch the
         * tick on the rising edge, clear it the instant the bit drops. */
        bool pc_link_now_down = s_exec.safety != NULL &&
                                 (safety_link_get_fault_sources(s_exec.safety) & SAFETY_FAULT_SRC_PC_LINK) != 0;
        TickType_t pc_link_check_now = xTaskGetTickCount();
        if (pc_link_now_down && !pc_link_was_down) {
            pc_link_down_since_tick = pc_link_check_now;
        }
        pc_link_was_down = pc_link_now_down;
        bool pc_link_down_sustained = pc_link_now_down &&
            ticks_to_ms(pc_link_check_now - pc_link_down_since_tick) >= pc_link_abort_silence_ms();

        bool wdt_faulted = false;
        xSemaphoreTake(s_exec.lock, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        uint32_t since_ms = ticks_to_ms(now - s_exec.last_tick_tick);
        bool tick_stale = since_ms > WATCHDOG_TICK_DEAD_MS;

        if (tick_stale) {
            /* Guard 9 proper forces relays off unconditionally the instant the
             * control task's own liveness tick goes stale, regardless of what
             * profile_executor_wd_decide() below says to do about the STATE --
             * this part is not delegated to that pure function (it always
             * needs to happen, not just "when RUNNING/PAUSED"). */
            ESP_LOGE(TAG, "control task tick stale for %lums -- forcing relays off (guard 9)",
                     (unsigned long)since_ms);
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            /* kiln_io_all_relays_off() only touches relay bits 1-4; a
             * relay/IO segment's general-purpose IO_1..7 line needs its own
             * explicit force-off, and this replaces rules_watchdog_entry's
             * force-release-and-off for the segment machinery (rules_task.c
             * is untouched by this pass and is deleted later -- see
             * profiles_http.h's profile_seg_kind_t comment -- so THIS is now
             * the one place a stalled control task still gets a relay/IO
             * segment's contacts open). Unconditional off, same as the relay
             * call just above: a control task that has stopped ticking gets
             * no leave_on_at_end exception. */
            io_segs_force_all_off(false);
            /* See guard9_assert_stale_tick_fault()'s own doc comment for why
             * this is now a helper rather than the bare safety_link_set_
             * fault_source() call this used to be, and what defect that
             * fixes (audit 2026-08-27 item 2). */
            guard9_assert_stale_tick_fault();
        }

        /* profile_executor_wd_decide() (profile_executor.h) is the pure
         * classifier this pass extracted so it could be host-tested without
         * pulling in FreeRTOS/kiln_io/relay_authority -- see that header's
         * doc comment for the full reasoning. Every input it needs was
         * already gathered above (outside s_exec.lock for the safety_link_
         * get_status() call, per this task's own long-standing discipline);
         * this call only classifies, all the I/O (relay writes, logging,
         * state mutation) stays here in the caller. */
        profile_executor_wd_input_t wd_in;
        memset(&wd_in, 0, sizeof(wd_in));
        wd_in.tick_stale = tick_stale;
        wd_in.tick_stale_ms = since_ms;
        wd_in.safety_processor_tripped = safety_processor_tripped;
        wd_in.safety_trip_reason = safety_diag_trip_reason;
        wd_in.safety_link_silent_30s = safety_link_silent_30s;
        wd_in.pc_link_down_sustained = pc_link_down_sustained;
        wd_in.state_running_or_paused = (s_exec.state == PROFILE_EXEC_RUNNING ||
                                          s_exec.state == PROFILE_EXEC_PAUSED);
        wd_in.state_faulted = (s_exec.state == PROFILE_EXEC_FAULTED);
        profile_executor_wd_result_t wd_out = profile_executor_wd_decide(&wd_in);

        switch (wd_out.action) {
        case PROFILE_EXECUTOR_WD_ACTION_FAULT:
            /* "relays dropped and retried until the write succeeds" --
             * kiln_io_all_relays_off() is the same fail-toward-off call guard
             * 9 uses above; this task rechecks it every WATCHDOG_CHECK_
             * PERIOD_MS as long as the firing stays in this faulted state,
             * which is the retry LINK_PROTOCOL.md sec 8 asks for (the RETRY
             * case below is what performs those later rechecks). */
            ESP_LOGE(TAG, "%s", wd_out.fault_reason);
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            io_segs_force_all_off(false); /* abnormal stop -- see escalate_guard_trip()'s branches */
            s_exec.state = PROFILE_EXEC_FAULTED;
            strncpy(s_exec.fault_reason, wd_out.fault_reason, sizeof(s_exec.fault_reason) - 1);
            s_exec.fault_reason[sizeof(s_exec.fault_reason) - 1] = '\0';
            /* Same release as escalate_guard_trip()'s FAULTED branches --
             * this is guard 9, a second and independent path into FAULTED
             * (a dead control task, a silent safety link), and it must leave
             * relay ownership in the same clean state those do. */
            release_profile_relay_claim();
            wdt_faulted = true;
            break;
        case PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF:
            /* Already faulted (this cause or another) but the underlying
             * condition (trip still latched, or link still silent) hasn't
             * cleared -- keep retrying the relay-off write per sec 8's
             * "dropped and retried until the write succeeds", without
             * touching fault_reason or re-triggering run_state_note()
             * (wdt_faulted stays false: nothing NEW happened this tick). */
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            io_segs_force_all_off(false); /* keep retrying, same as the relay-off retry above */
            break;
        case PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP:
            /* Edge-triggered (s_idle_trip_logged just below) so a trip that
             * stays latched for minutes logs once, not once every
             * WATCHDOG_CHECK_PERIOD_MS. The GUIs surface this state
             * themselves by reading diag_state directly (main_page.html's
             * renderSafetyTrip(), ui_page_home.c's refresh()), so no shared
             * state needs adding here just for display. */
            if (!s_idle_trip_logged) {
                ESP_LOGW(TAG, "safety processor tripped (%s) while idle -- no run to abort",
                         profile_executor_safety_trip_words(safety_diag_trip_reason));
            }
            s_idle_trip_logged = true;
            break;
        case PROFILE_EXECUTOR_WD_ACTION_NONE:
        default:
            break;
        }
        /* Reset the edge-trigger the moment the trip input itself clears,
         * outside the switch so it resets regardless of which action ran
         * this tick (a cleared trip that was previously IDLE-logged may
         * land on ACTION_NONE the very next tick, never revisiting the
         * LOG_IDLE_TRIP case body). */
        if (!safety_processor_tripped) {
            s_idle_trip_logged = false;
        }
        run_snapshot_buf_t wdt_snap;
        if (wdt_faulted) {
            capture_run_snapshot(&wdt_snap);
        }
        xSemaphoreGive(s_exec.lock);

        /* Guard 9 is one of the few paths that can end a firing without the
         * control task ever running again, so it has to record the ending
         * itself -- otherwise a dead control task and a pulled plug look
         * identical next boot, which is precisely the confusion this record
         * exists to remove. Written from this task, outside the lock. */
        if (wdt_faulted) {
            run_state_note(RUN_STATE_PHASE_FAULTED, &wdt_snap.snap);
        }
    }
}

/* ---- public API -------------------------------------------------------------- */

esp_err_t profile_executor_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                  SafetyLinkClass *safety_or_null)
{
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.io = io_or_null;
    s_exec.thermo_bus = thermo_bus_or_null;
    s_exec.safety = safety_or_null;
    s_exec.state = PROFILE_EXEC_IDLE;
    s_exec.last_tick_tick = xTaskGetTickCount();

    s_exec.lock = xSemaphoreCreateMutex();
    if (!s_exec.lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return ESP_ERR_NO_MEM;
    }

    /* Load (and log) the previous run's breadcrumb before the control task
     * exists, so the WARN banner lands in the boot log ahead of any tick
     * output. Loading is ALL that happens: the executor comes up IDLE with
     * every relay already off, exactly as it did before this record existed,
     * and nothing below consults it. TODO.md 6A.3: no auto-resume, ever.
     * A failure costs the breadcrumb, not the executor -- same non-fatal
     * convention as relay_cycles_init() in main.c. */
    esp_err_t rs_err = run_state_init();
    if (rs_err != ESP_OK) {
        ESP_LOGW(TAG, "run_state_init failed: %s -- no reboot breadcrumb kept this boot",
                 esp_err_to_name(rs_err));
    }

    /* Priority 5, matching the UART bridge tasks (uart_bridge.c, all 5) --
     * TODO.md 6A.7 calls for "below the link-loss watchdog (6), above the
     * bridge tasks". FreeRTOS priorities are integers with nothing between
     * 5 and 6, so exactly "above the bridge tasks" isn't representable
     * without also renumbering uart_bridge.c's tasks (out of scope here);
     * tying at 5 is the closest achievable approximation, still strictly
     * below link_watchdog_task/UART_PROTOCOL_TASK_PRIORITY (6).
     *
     * 4096: MAX31856_read_all/pid_update_terms/thermal_guard_tick/
     * kiln_io_set_relay_mask/safety_link_set_fault_source all run on this
     * stack, now looped up to MAX31856_CHANNEL_COUNT times per tick. */
    /* INTERNAL stack, deliberately. A 2026-08-22 pass moved this to PSRAM on
     * the reasoning that every flash-touching call was made by
     * profile_executor_run()/_halt()/_pause() from whichever task called
     * them, never from this loop. That reasoning was WRONG and the board
     * crashed on the bench the first time a real firing was started:
     * the tick path itself calls run_state_note() (RUNNING/FAULTED/DONE
     * breadcrumbs, ~line 1516) and relay_cycles_maybe_persist() (~line 1481),
     * both of which write NVS. A task whose stack lives in PSRAM cannot be
     * running when the flash cache is disabled -- ESP-IDF asserts
     * esp_task_stack_is_sane_cache_disabled() in
     * spi_flash_disable_interrupts_caches_and_other_cpu() and panics.
     *
     * Do not move this back without first removing every flash write from
     * the tick path, which is not a stack-placement question but a design
     * one: the run-state breadcrumb exists precisely so a power loss mid-tick
     * is recoverable. */
    BaseType_t ok = xTaskCreatePinnedToCore(executor_task_entry, "profile_executor", 4096, NULL, 5,
                                            &s_exec.task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(profile_executor) failed");
        vSemaphoreDelete(s_exec.lock);
        s_exec.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Opus review round 3, item 5: registered even on the ok==pdPASS-only
     * path (matching every other stack_margin_register() call site in this
     * codebase -- creation failure already returned above, so this line is
     * only ever reached with a real handle). 4096 must match the literal
     * xTaskCreatePinnedToCore() argument two lines up exactly -- see
     * stack_margin.h's own doc comment on why this number is never assumed
     * equal to another task's. */
    stack_margin_register("profile_executor", &s_exec.task, 4096);
    /* Small and independent on purpose -- guard 9 exists precisely because
     * the control task cannot be trusted to notice its own death. Same
     * priority as the control task it's watching.
     *
     * INTERNAL stack, for the same reason as executor_task_entry() above and
     * with the same bench crash behind it. The claim that this task "only
     * reads status and forces relays off" missed run_state_note() on its own
     * abort path (~line 1728): when the safety link goes silent it aborts the
     * firing AND persists why. That is exactly the moment this task must not
     * fail, so its stack must be reachable with the flash cache disabled. */
    ok = xTaskCreatePinnedToCore(watchdog_task_entry, "profile_exec_wdt", 2560, NULL, 5,
                                 &s_exec.watchdog_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(profile_exec_wdt) failed -- guard 9 unavailable this boot");
    }
    /* Registered unconditionally, ok==pdPASS or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time (stack_margin.h's own
     * doc comment), so a creation failure just reads back alive=false
     * rather than needing a second branch here. */
    stack_margin_register("profile_exec_wdt", &s_exec.watchdog_task, 2560);

    ESP_LOGI(TAG, "profile executor up (io_ready=%d, thermo_ready=%d, safety_ready=%d) -- "
                  "NOT YET VERIFIED AGAINST REAL RELAY/THERMOCOUPLE HARDWARE (single- or multi-zone), "
                  "see profile_executor.h",
             io_or_null != NULL, thermo_bus_or_null != NULL && thermo_bus_or_null->initialized,
             safety_or_null != NULL);
    return ESP_OK;
}

/* Guard 5's absolute ceiling, checked BEFORE a firing is allowed to start.
 * TODO.md's 2026-08-27 audit ("Guard 5's absolute ceiling is off by
 * default") found that max_temp_c == 0 means "no ceiling" in
 * thermal_guard.c, so a zone that was never saved through /settings/zones
 * fires with guard 5 permanently a no-op -- every OTHER per-zone threshold
 * in this repo's convention treats 0 as "not configured, substitute a
 * firmware default that still protects" (see zones_http.h's field-by-field
 * doc comments), but max_temp_c is the one field where 0 was instead wired
 * to mean "disabled". That is backwards for the single most dangerous field
 * on the page: a substituted number would have to be invented (there is no
 * physically-meaningful default temperature anywhere in this repo --
 * ZONE_MAX_TEMP_C_MAX is a 1400C INPUT-validation sanity bound mirrored
 * from profiles_http.c's PROFILE_TARGET_C_MAX, not a safe ceiling for an
 * arbitrary owner's kiln), and a wrong invented ceiling either does nothing
 * (too high) or nags a correctly-configured kiln (too low). Refusing
 * instead cannot be silently wrong, matches the ramp-ceiling refusal in
 * profile_executor_run() (same field-is-zero-means-uncommissioned
 * reasoning, same message shape), and matches autotune_engine_run_relay()'s
 * own guard-5 refusal for the identical reason (autotune_engine.c ~line
 * 1164, this task's FILES YOU OWN excludes that file so it is read-only
 * precedent here, not touched). autotune_engine_run() (the step-test path)
 * is the one place in the repo that deliberately tolerates max_temp_c == 0
 * -- see its STEP_TEST_GUARD_HEADROOM_C comment -- because a step test is a
 * short, operator-watched open-loop probe, not an unattended multi-hour
 * firing; that carve-out does not apply here.
 *
 * Pulled out to its own function (rather than left inline in
 * profile_executor_run()) purely so a host test can drive it directly
 * against a profile_t without needing profile_executor_start()'s full
 * FreeRTOS/relay/thermocouple harness -- see
 * test_profile_executor_prestart.c's test_profile_zones_have_ceiling_*.
 *
 * Checked against zones that can actually command heat, not merely against
 * p->zone_mask (2026-08-27, revised after the owner's live board reply:
 * heaters are now physically wired, and a real GET /api/zones read back
 * zone1/zone2 at max_temp_c==0, control_mode==0/OFF, never assigned to any
 * profile that actually drives them -- exactly the case this carve-out
 * exists for). ZONE_CONTROL_MODE_OFF (zones_http.h) "never commands heat" --
 * confirmed by reading heater_output_duty()'s switch in this file, which has
 * no case that can assert a relay for an OFF zone. Guard 5 exists to catch a
 * runaway zone that IS being driven; a zone this profile targets but that
 * cannot physically command a relay has nothing for guard 5 to protect
 * against, so refusing the whole firing over it would be a nuisance refusal
 * of exactly the kind SAFETY_MODEL.md's doctrine warns against, and the
 * fastest way to get this check disabled by whoever hits it. This mirrors
 * profile_executor_run()'s own n_heating_zones logic just below (same
 * "OFF stays a valid per-zone choice" reasoning, same zones_config_get_
 * control_mode() call, same OFF-is-the-safe-fallback-on-read-failure
 * default). Returns false and, if out_missing_zone is non-NULL, the first
 * (lowest-index) offending zone the instant any zone that CAN heat reads
 * max_temp_c == 0; returns true when every zone this profile can actually
 * drive has a real ceiling (including the case where none of them can heat
 * at all -- profile_executor_run()'s separate all-OFF refusal owns that
 * case, not this function). */
static bool profile_zones_have_ceiling(const profile_t *p, uint8_t *out_missing_zone)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p->zone_mask & (1u << zi))) continue;

        /* OFF, not BANGBANG, as the fallback if the getter fails -- same
         * fail-safe default profile_executor_run()'s n_heating_zones loop
         * uses: "we could not read this zone's control mode" must not be
         * read as "assume it can heat, and gate a real firing on it". */
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        if (mode == ZONE_CONTROL_MODE_OFF) continue;

        float max_temp_c = 0.0f, min_temp_c = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
        if (!(max_temp_c > 0.0f)) {
            if (out_missing_zone) *out_missing_zone = zi;
            return false;
        }
    }
    return true;
}

/* ---- Warm-start (PROFILES.md "Warm-start: joining a profile already at
 * temperature", owner request 2026-08-30) -----------------------------------
 *
 * Pure decision core, deliberately pulled out of profile_executor_run() the
 * same way profile_executor_wd_decide() (profile_executor.h) is pulled out
 * of watchdog_task_entry() -- no I/O, no locking, host-testable directly.
 * Decides ONLY where the schedule should enter (segment index, dwelling or
 * not, starting target_c, and how much of the entry segment's ramp time is
 * already spent); profile_executor_run() is the only caller and is the one
 * that turns "entry_segment_index > 0" into replayed RELAY_IO commands and a
 * log line.
 *
 * current_c is the reading profile_executor_run() decided is "current
 * temperature" for this decision -- Q4: the SAME coolest-active-zone reading
 * used elsewhere in this run's own start-of-run baseline, not a second,
 * differently-sourced notion of "now". NAN means no valid reading was
 * available at all (an absent thermocouple bus, every active zone's sensor
 * unhealthy) -- the safe answer is never skip anything without knowing where
 * the kiln actually is, so this returns the exact pre-feature default
 * (segment 0, target_c = the profile's own first segment target, not
 * warm-started).
 *
 * Q2, mid-ramp entry: prev_level tracks the temperature the CURRENT
 * ZONE_RAMP segment under examination ramps FROM (its predecessor's own
 * target, or current_c for segment 0 -- segment 0 has always ramped from
 * "wherever the kiln actually is right now", warm-start or not, which is
 * exactly why the i==0 case below produces byte-identical output to the
 * pre-feature code whether or not a warm start ends up happening). The first
 * segment whose OWN target_c is at or above current_c is where the ramp
 * would cross current_c, so that is where the run enters -- at the crossing
 * point (entry_target_c = current_c, carrying the remaining ramp distance/
 * time automatically: profile_executor_run() seeds s_exec.target_c with
 * entry_target_c and the ordinary per-tick ramp step in the control loop
 * takes it from there), never at the segment's own start (which would be
 * BELOW current_c and reintroduce the bug this feature exists to remove).
 *
 * Q3, dwell segments: deliberately NOT special-cased. A segment already at
 * or above current_c for its own target enters as a DWELL with
 * entry_segment_elapsed_s == 0 -- the full configured soak still runs. The
 * tempting wrong optimization would be "we're already at temperature, count
 * the soak as satisfied too" -- a soak is time AT temperature, not time
 * spent arriving there, and this function never shortens one.
 *
 * Q5, falling edges: the loop scans segments in profile order and BREAKS at
 * the first ZONE_RAMP segment whose target_c is below the level the
 * previous ZONE_RAMP segment left off at -- the "leading ascent" only.
 * RELAY_IO segments are skipped over (their target_c is meaningless, Q1) and
 * never count as an ascent or a descent themselves. A profile that comes
 * down again after climbing (a controlled cool, an anneal) has that descent
 * entirely out of reach of this function -- if current_c is not reached
 * within the leading ascent, warm_started stays false rather than ever
 * considering a later, lower segment.
 *
 * Hotter-than-everything (falls off the end of the loop without ever
 * finding a segment whose target_c >= current_c): lands on the LAST
 * ZONE_RAMP segment of the leading ascent, entered as a DWELL from its own
 * start (entry_segment_elapsed_s == 0) -- i.e. treated the same as "already
 * at this segment's target" above, running that segment's full configured
 * soak. Rejected alternatives and why:
 *   - Refuse to start: the kiln is at a perfectly fireable temperature
 *     (hotter than the profile only means "further along than planned"),
 *     and refusing a start over that would make the feature this exists to
 *     fix -- "firing back-to-back loads... wastes hours" -- worse, not
 *     better, for the exact case (a kiln that never fully cooled) the
 *     owner's request opens with.
 *   - Silently mark DONE / skip straight to whatever comes after the ascent
 *     (a cooling leg, or end of profile): would skip the top segment's own
 *     dwell -- Q3's "a soak is time at temperature" applies here at least as
 *     much as it does mid-profile; the top of the ascent is usually the
 *     whole point of the firing (the final maturing soak of a glaze/bisque
 *     schedule) and is exactly the segment a "just run the last dwell"
 *     choice must not shortcut.
 *   Landing on the top segment's dwell keeps that soak intact and then lets
 *   the ordinary segment-stepping machinery carry on from there completely
 *   unmodified -- if a cooling leg follows, it runs normally once the dwell
 *   finishes, same as any other run that reaches that point the ordinary
 *   way. */
typedef struct {
    bool     warm_started;
    uint8_t  entry_segment_index;
    bool     entry_dwelling;
    float    entry_target_c;
    uint32_t entry_segment_elapsed_s;
} profile_warm_start_plan_t;

static profile_warm_start_plan_t profile_executor_plan_warm_start(const profile_t *p, float current_c)
{
    profile_warm_start_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.entry_segment_index = 0;
    plan.entry_dwelling = false;
    plan.entry_segment_elapsed_s = 0;
    plan.warm_started = false;

    if (p == NULL || p->segment_count == 0 || isnan(current_c)) {
        /* No usable reading (or nothing to scan) -- the pre-feature default:
         * segment 0's own target, ramping from wherever run() otherwise
         * decided the baseline was (it does not use entry_target_c in this
         * branch; see profile_executor_run()'s "only apply plan fields when
         * plan.warm_started" rule). */
        plan.entry_target_c = (p != NULL && p->segment_count > 0) ? p->segments[0].target_c : 0.0f;
        return plan;
    }

    /* Pass 1 (Q5): find the leading-ascent boundary using ONLY the
     * segments' own target_c sequence, completely independent of current_c.
     * Doing this as its own pass (rather than seeding the walk below's
     * "previous level" with current_c and testing descent against THAT)
     * matters: segment 0 always ramps from wherever the kiln actually is
     * (current_c), so a kiln reading above segment 0's own target would
     * otherwise look like an immediate "descent" relative to current_c and
     * wrongly cut the ascent down to nothing, even on a purely ascending
     * profile -- exactly the "hotter than segment 0" case this feature
     * exists to handle, not a real cool-down/anneal profile at all. */
    uint8_t ascent_end = p->segment_count; /* exclusive */
    bool have_last_seg_target = false;
    float last_seg_target = 0.0f;
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *seg = &p->segments[i];
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            continue; /* no temperature of its own -- neither ascent nor descent */
        }
        if (have_last_seg_target && seg->target_c < last_seg_target) {
            ascent_end = i;
            break;
        }
        last_seg_target = seg->target_c;
        have_last_seg_target = true;
    }

    /* Pass 2 (Q2/Q3/Q4): walk the leading ascent looking for where current_c
     * fits. prev_level starts at current_c -- segment 0 (or the first
     * ZONE_RAMP segment, if segment 0 is a RELAY_IO) has always ramped from
     * "wherever the kiln actually is right now", warm-start or not, which is
     * why landing here at i==0 produces byte-identical output to the
     * pre-feature code. Every later ZONE_RAMP segment instead uses the
     * PRECEDING segment's own target_c as prev_level -- current_c already
     * played its one role (picking the ascent boundary above) and must not
     * also masquerade as an earlier segment's target here. */
    float prev_level = current_c;
    for (uint8_t i = 0; i < ascent_end; i++) {
        const profile_segment_t *seg = &p->segments[i];
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            continue; /* Q1: no temperature of its own */
        }

        if (current_c <= seg->target_c) {
            /* This segment's ramp reaches (or already starts at/above)
             * current_c -- this is where the run enters. */
            plan.entry_segment_index = i;
            plan.entry_dwelling = false;
            if (current_c <= prev_level) {
                /* Kiln is at/above where this segment's own ramp starts --
                 * i==0 always lands here (prev_level == current_c), which is
                 * exactly the pre-feature start: segment 0, offset 0,
                 * ramping from current_c. Not a warm start. A later segment
                 * can also land here if an earlier one's target already
                 * reached/exceeded current_c -- entering it at its own start
                 * is correct and IS a warm start (segments before it were
                 * skipped). */
                plan.entry_target_c = prev_level;
                plan.entry_segment_elapsed_s = 0;
                plan.warm_started = (i > 0);
            } else if (seg->ramp_c_per_hr > 0.0f) {
                /* Q2: mid-ramp entry. Seeding target_c at current_c (instead
                 * of prev_level) is what carries the remaining ramp time --
                 * the ordinary per-tick ramp step takes it from there. The
                 * elapsed figure is reported for visibility only (Q6); the
                 * control loop does not consume it during a ramp. */
                plan.entry_target_c = current_c;
                plan.entry_segment_elapsed_s =
                    (uint32_t)(((current_c - prev_level) / seg->ramp_c_per_hr) * 3600.0f + 0.5f);
                plan.warm_started = true;
            } else {
                /* A step segment (no ramp rate configured) has no partial
                 * distance to carry -- it jumps straight to its target. */
                plan.entry_target_c = seg->target_c;
                plan.entry_segment_elapsed_s = 0;
                plan.warm_started = true;
            }
            return plan;
        }

        /* current_c is past this whole segment already -- keep scanning the
         * ascent, and remember this as the fallback "hotter than the whole
         * ascent" landing spot (see this function's header comment). */
        prev_level = seg->target_c;
        plan.entry_segment_index = i;
        plan.entry_target_c = seg->target_c;
        plan.entry_dwelling = true;
        plan.entry_segment_elapsed_s = 0;
        plan.warm_started = true;
    }
    return plan;
}

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    /* Recovery mode (boot_guard.h) deliberately skips profile_executor_start()
     * so it can bring the board up with just Wi-Fi and the OTA HTTP routes --
     * but other code that DOES still run in that mode (safety_link.c's poll
     * task among others, found on the bench: safety_poll_task ->
     * safety_build_and_send_context() -> profile_executor_get_status() ->
     * xQueueSemaphoreTake() -> "assert failed: (( pxQueue ))" -> panic) can
     * still call into this module's public API. s_exec.lock is NULL until
     * profile_executor_start() creates it, and taking a NULL FreeRTOS mutex
     * asserts. Every public entry point below tests it first and returns a
     * clean "not running" answer instead of touching s_exec at all. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(TAG, "profile_executor_run() called before profile_executor_start() -- refused");
        if (err_msg) snprintf(err_msg, err_cap, "profile executor not started");
        return false;
    }

    profile_t p;
    if (!profiles_http_get(profile_id, &p)) {
        if (err_msg) snprintf(err_msg, err_cap, "no such profile");
        return false;
    }
    if (p.segment_count == 0) {
        if (err_msg) snprintf(err_msg, err_cap, "profile has no segments");
        return false;
    }
    if (p.zone_mask == 0) {
        /* Two different causes land here and the operator has to be able to
         * tell them apart: a saved profile whose own mask is empty (edit the
         * profile) versus a board with no thermocouples declared yet, which
         * is what a built-in schedule's mask is derived from (configure the
         * zones). Reporting either as "no such profile", as this path used
         * to via profiles_http_get(), was simply false. */
        if (err_msg) {
            if (zones_config_get_thermo_count() == 0) {
                snprintf(err_msg, err_cap,
                         "no zones are configured yet -- set the thermocouple count and zone settings "
                         "before firing (see /settings/zones)");
            } else {
                snprintf(err_msg, err_cap, "profile targets no zones");
            }
        }
        return false;
    }
    /* TODO.md 8.2 "Tie it to the guards, not only the UI": refuse explicitly
     * rather than let this fall through to apply_relay()'s relay_mask == 0
     * check, which cannot tell "genuinely no zones configured" from "zone
     * config failed to load" -- both read as the same zeroed struct. */
    if (!zones_config_is_valid()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone config failed to load or has not been saved -- this kiln cannot be started "
                     "until zone config loads cleanly (see /settings/zones)");
        }
        return false;
    }
    // Direction B of the mutual OTA interlock (ota_interlock.h/.c is
    // direction A -- "no update while heating"; this is "no heating while
    // updating"): refuse to start a profile while an update is in progress
    // on either processor. heat_interlock.c is the shared pure predicate;
    // ota_http_heat_blocked_by_update() is its ESP-IDF glue, same split as
    // ota_http_check_interlocks()/ota_interlock_check(). Checked here,
    // before the mutex/feasibility checks below, since it's cheapest and
    // orthogonal to zone state.
    if (ota_http_heat_blocked_by_update(err_msg, err_cap)) {
        return false;
    }
    /* B2 (opus review, 2026-08-27): zones_http.c's per-zone current sweep is
     * a sixth writer of the mains-contactor relays, with its own start-time
     * refusal if a profile is already running/paused -- but that check was
     * only ever made ONE-DIRECTIONAL: nothing here refused to start a
     * profile while a sweep was already energizing a zone. See
     * zones_current_sweep_is_active()'s doc comment (zones_http.h) for the
     * full picture. Checked here, right after the OTA check above, for the
     * same "cheap and orthogonal to zone state" reasoning. */
    if (zones_current_sweep_is_active()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- it cannot run at the same time as a firing");
        }
        return false;
    }
    /* Refuse at the door when heat authority is already blocked -- the same
     * check, in the same words, autotune_engine.c's begin_run_locked() makes.
     * Without it a start on a board whose safety link is down answered
     * {"ok":true}, entered RUNNING, and was killed ~1s later by the
     * watchdog's "safety processor link silent for >=30000ms, firing
     * aborted", leaving a latched fault the operator then had to Stop before
     * anything else would start. Reporting success for a firing that cannot
     * heat is the failure this refuses to repeat; the message stays under
     * the char[128] dashboard_http.c's start handler passes. */
    {
        uint32_t sources = 0;
        if (relay_authority_on_blocked(s_exec.safety, &sources)) {
            if (err_msg) {
                /* ROADMAP.md M13: decode the mask instead of showing a bare
                 * hex value -- same shortening (first source + "(+more)")
                 * zones_http.c's ZONE_SWEEP_ZONE_ENERGIZE_REFUSED case and
                 * autotune_engine.c's matching refusal already use. */
                char src_words[160];
                safety_fault_source_words(sources, src_words, sizeof(src_words));
                char *comma = strchr(src_words, ',');
                bool more = (comma != NULL);
                if (comma != NULL) {
                    *comma = '\0';
                }
                snprintf(err_msg, err_cap,
                         "heat is blocked (%.32s%s, usually the safety link down) -- "
                         "a firing cannot start",
                         src_words, more ? " (+more)" : "");
            }
            return false;
        }
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((p.zone_mask & (1u << zi)) && autotune_engine_is_active_on_zone(zi)) {
            if (err_msg) {
                snprintf(err_msg, err_cap, "zone %u has an autotune run active -- it cannot run at the "
                                           "same time as a profile (TODO.md 6A.5)",
                         zi);
            }
            return false;
        }
    }

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);

    if (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) snprintf(err_msg, err_cap, "a profile is already running -- stop it first");
        return false;
    }
    if (s_exec.state == PROFILE_EXEC_FAULTED) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) snprintf(err_msg, err_cap, "a thermal guard is latched -- acknowledge it (Stop) first");
        return false;
    }

    /* Re-run TODO.md section 5's feasibility check against every
     * participating zone's *current* ceiling -- 6A.5: a profile is only
     * feasible if every one of its zones can sustain the requested rate. */
    for (uint8_t i = 0; i < p.segment_count; i++) {
        float rate = p.segments[i].ramp_c_per_hr;
        if (rate <= 0.0f) continue;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(p.zone_mask & (1u << zi))) continue;
            float ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ceiling);
            if (rate > ceiling) {
                xSemaphoreGive(s_exec.lock);
                if (err_msg) {
                    /* A ceiling of 0 is not a ceiling the operator chose, it
                     * is a zone that was never commissioned -- every rate is
                     * "over" it. Quoting "exceeds zone 1's current 0.0 C/hr
                     * ceiling" sent a bench session looking for a ceiling to
                     * raise when the actual answer was that zones 1 and 2 had
                     * never been configured at all. Say which it is. */
                    if (ceiling <= 0.0f) {
                        snprintf(err_msg, err_cap,
                                 "zone %u has no ramp ceiling configured (max_ramp_c_per_hr is 0) -- "
                                 "commission the zone in Settings > Zones before firing it",
                                 zi);
                    } else {
                        snprintf(err_msg, err_cap,
                                 "segment %u: ramp rate %.1f C/hr exceeds zone %u's current %.1f C/hr ceiling",
                                 i + 1, (double)rate, zi, (double)ceiling);
                    }
                }
                return false;
            }
        }
    }

    /* TODO relay/IO segments' SECOND, independent zone-ownership re-check
     * (the storage-side one is profiles_http.c's profile_relay_is_zone_owned(),
     * enforced at save time by validate_io_segment()) -- see this function's
     * own re-run of the ramp-ceiling feasibility check just above for the
     * identical reasoning: a relay can be assigned to a zone AFTER a profile
     * was saved, and this run must not energize a contact a zone now owns. */
    for (uint8_t i = 0; i < p.segment_count; i++) {
        if (p.segments[i].seg_kind != PROFILE_SEG_KIND_RELAY_IO) {
            continue;
        }
        uint8_t t = p.segments[i].io_target;
        bool is_relay = (t >= PROFILE_IO_TARGET_RELAY_BASE) && (t < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
        if (!is_relay) {
            continue; /* general-purpose IO_1..7 has no zone-ownership concept */
        }
        uint8_t owning_zone = 0;
        if (relay_io_target_is_zone_owned(t, &owning_zone)) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "segment %u: relay %u is now assigned to zone %u -- this profile cannot run "
                         "until that segment's target is changed",
                         i + 1, t, owning_zone);
            }
            return false;
        }
    }

    /* Guard 5's absolute ceiling, refused the same way the ramp ceiling just
     * above is: TODO.md's 2026-08-27 audit ("Guard 5's absolute ceiling is
     * off by default") found that max_temp_c == 0 means "no ceiling" in
     * thermal_guard.c, so a zone that was never saved through
     * /settings/zones fires with guard 5 permanently a no-op -- every OTHER
     * per-zone threshold in this repo's convention treats 0 as "not
     * configured, substitute a firmware default that still protects" (see
     * zones_http.h's field-by-field doc comments), but max_temp_c is the one
     * field where 0 was instead wired to mean "disabled". That is backwards
     * for the single most dangerous field on the page: a substituted number
     * would have to be invented (there is no physically-meaningful default
     * temperature anywhere in this repo -- ZONE_MAX_TEMP_C_MAX below is a
     * 1400C INPUT-validation sanity bound mirrored from profiles_http.c's
     * PROFILE_TARGET_C_MAX, not a safe ceiling for an arbitrary owner's
     * kiln), and a wrong invented ceiling either does nothing (too high) or
     * nags a correctly-configured kiln (too low). Refusing instead cannot be
     * silently wrong, matches the ramp-ceiling refusal immediately above
     * (same field-is-zero-means-uncommissioned reasoning, same message
     * shape), and matches autotune_engine_run_relay()'s own guard-5 refusal
     * for the identical reason (autotune_engine.c ~line 1164, this task's
     * FILES YOU OWN excludes that file so it is read-only precedent here,
     * not touched). autotune_engine_run() (the step-test path) is the one
     * place in the repo that deliberately tolerates max_temp_c == 0 -- see
     * its STEP_TEST_GUARD_HEADROOM_C comment -- because a step test is a
     * short, operator-watched open-loop probe, not an unattended multi-hour
     * firing; that carve-out does not apply here. */
    {
        uint8_t missing_zone = 0;
        if (!profile_zones_have_ceiling(&p, &missing_zone)) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "zone %u has no absolute temperature ceiling configured (max_temp_c is 0) -- "
                         "set Max Temp (C) for this zone in Settings > Zones before firing it",
                         missing_zone);
            }
            return false;
        }
    }

    s_exec.profile = p;
    s_exec.profile_id = profile_id;
    s_exec.segment_index = 0;
    s_exec.dwelling = false;
    s_exec.segment_elapsed_s = 0;
    s_exec.ramp_lock_held = false;
    s_exec.ramp_lock_lagging_mask = 0;
    s_exec.fault_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    s_exec.global_fault_source = 0;
    /* Per-run, not cumulative: carrying a previous firing's claim forward
     * would let this run's sweep open a relay the last run once drove and an
     * operator has since taken over manually. Each run starts owing nothing
     * and claims what it touches (see s_exec_state_t.claimed_relay_mask). */
    s_exec.claimed_relay_mask = 0;
    /* Same "starts owing nothing" reasoning as claimed_relay_mask just above,
     * for the relay/IO segment machinery: a previous run's io_segs[] state
     * (which segment was active, what its remaining_s countdown was) has no
     * meaning against a freshly (re)started schedule. */
    memset(s_exec.io_segs, 0, sizeof(s_exec.io_segs));
    /* Warm-start state, same "starts owing nothing" reasoning -- overwritten
     * below if this run actually warm-starts. */
    s_exec.warm_started = false;
    s_exec.warm_start_reason[0] = '\0';
    s_exec.warm_start_replayed_count = 0;
    memset(s_exec.warm_start_replayed_segments, 0, sizeof(s_exec.warm_start_replayed_segments));
    /* Feedforward inputs start from their safe values: no ramp commanded yet,
     * and the fallback ambient until a cold junction actually answers below. */
    s_exec.target_rate_c_per_s = 0.0f;
    s_exec.ambient_c = FALLBACK_AMBIENT_C;
    s_exec.ambient_from_cj = false;

    memset(s_exec.zones, 0, sizeof(s_exec.zones)); /* also zeros every zone's fs_* accumulator */

    /* PID_EXPANSION_PLAN.md Phase 7a: fresh firing-stats accumulator for
     * this run. fs_target_min_c/max_c start NAN (not 0) so the first
     * RUNNING tick's target_c seeds both ends of the span unconditionally --
     * 0 would be a real, wrong target for a kiln firing (see the tick
     * loop's fs_target_min_c/max_c update). */
    s_exec.fs_target_min_c = NAN;
    s_exec.fs_target_max_c = NAN;
    s_exec.fs_persisted = false;
    {
        time_t now = time(NULL);
        /* time(NULL) before SNTP sync reads as a small epoch offset (ESP-IDF
         * boots the RTC near 0), not a plausible 2020s+ date -- treat
         * anything before 2020-01-01 UTC (1577836800) as "unsynced" and
         * store 0 rather than a misleadingly precise-looking fake date. */
        s_exec.run_started_unix_s = (now >= (time_t)1577836800) ? (uint32_t)now : 0u;
    }

    /* Sampled BEFORE the per-zone config read below, not after (TODO.md
     * 6A.7): zones_http.c's writers don't take s_exec.lock, so an edit
     * committed while this loop is running would otherwise be swallowed --
     * counted as "already applied" while half the zones still hold the
     * pre-edit values. Sampling first makes that race resolve the safe way:
     * the first tick sees a generation mismatch and re-reads everything. A
     * redundant reload costs one log line; a missed one costs a firing run
     * with settings the operator believes they changed. */
    s_exec.config_generation = zones_config_generation();

    /* TODO.md 6A.5 load-staggering: n_zones for the phase-offset formula
     * "zone i starts its window at i*window_ms/n_zones" -- i is this run's
     * rank among its own active zones (0-based, ascending zone index), not
     * the raw zone index, so a 2-zone run on zones {0,2} still gets a clean
     * 50/50 offset instead of stretching across 3 slots it isn't using. */
    uint8_t n_active_zones = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (p.zone_mask & (1u << zi)) n_active_zones++;
    }

    /* Refuse a firing that cannot heat anything.
     *
     * ZONE_CONTROL_MODE_OFF is 0, which is also what an uncommissioned zone
     * reads as, so "every zone in this profile is OFF" is the DEFAULT state of
     * a board nobody has configured yet -- not an exotic case. Started in that
     * state the run was accepted, reported state "running" with a target
     * ramping convincingly for its full 21 minutes, duty 0.0, faulted false,
     * heat_blocked false, and no message in the log: every single indicator
     * said a firing was under way and not one relay would ever close. The
     * readiness page agreed ("every configured zone has a mode (OFF is a valid
     * choice)"), which is true of one zone and dangerously incomplete of a
     * whole profile. Found by running it on the bench and watching nothing
     * happen for two minutes.
     *
     * OFF stays a valid per-zone choice -- a 3-zone kiln fired on 2 zones is
     * legitimate -- so only the all-OFF case is refused, and the mixed case
     * gets a log line naming which zones will sit idle. */
    uint8_t n_heating_zones = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        zone_control_mode_t m = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &m);
        if (m != ZONE_CONTROL_MODE_OFF) {
            n_heating_zones++;
        } else {
            ESP_LOGW(TAG, "zone %u is in this profile but its control mode is OFF -- it will not heat", zi);
        }
    }
    if (n_heating_zones == 0) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "every zone in this profile is set to control mode OFF -- nothing would heat. "
                     "Pick bang-bang or PID in Settings > Zones.");
        }
        return false;
    }

    int8_t first_active = -1;
    uint8_t active_rank = 0;
    float baseline_target_c = p.segments[0].target_c;
    /* Warm-start (Q4): "current temperature" is the COOLEST active zone's
     * actual reading, taken from the same start-of-run sample as
     * baseline_target_c above (not a second, separately-timed read) -- if
     * the zones disagree, preferring the coolest one means warm-start can
     * only ever skip work every active zone agrees is already done. NAN
     * until (if) a valid reading is found below. */
    float warm_start_coolest_c = NAN;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        zone_runtime_t *z = &s_exec.zones[zi];
        z->active = true;
        if (first_active < 0) first_active = (int8_t)zi;

        /* OFF, not BANGBANG, as the fallback if the getter fails: "we could
         * not read this zone's control mode" must not resolve to "close the
         * relay". The all-OFF guard above has already refused a run where
         * every zone lands here. */
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        z->control_mode = mode;

        /* Cached only so the mid-run reload (TODO.md 6A.7) can tell a mask
         * edit apart from no change, and can still name the outgoing relays
         * when one happens -- see zone_runtime_t.relay_mask. Everything else
         * in this file keeps reading the live mask through apply_relay(). */
        uint8_t relay_mask = 0;
        zones_config_get_relay_mask(zi, &relay_mask);
        z->relay_mask = relay_mask;
        /* Claimed up front rather than waiting for the first apply_relay():
         * a zone whose mask is edited before it ever energizes still has to
         * be sweepable, and the run has unambiguously taken these relays
         * over the moment it starts. */
        s_exec.claimed_relay_mask |= relay_mask;

        float kp = 0.0f, ki = 0.0f, kd = 0.0f;
        zones_config_get_pid(zi, &kp, &ki, &kd);
        z->pid_cfg = (pid_cfg_t){
            .kp = kp, .ki = ki, .kd = kd,
            .d_filter_tau_s = PID_D_FILTER_TAU_S, .b = PID_SETPOINT_WEIGHT_B,
            .pid_range_c = PID_FUNCTIONAL_RANGE_C,
        };
        pid_reset(&z->pid_state);
        z->fuzzy_prev_effective_ki = 0.0f;

        /* TODO.md 6A.2 feedforward: identified model or nothing. A zone that
         * has never been autotuned simply runs on feedback alone, as every
         * firing did before this existed -- see zone_load_model(). */
        (void)zone_load_model(zi);

        float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
        zones_config_get_heater_cfg(zi, &window_ms, &min_on_ms, &min_off_ms);
        z->heater_cfg = (heater_output_cfg_t){
            .window_ms = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS,
            .min_on_ms = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS,
            .min_off_ms = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS,
        };
        heater_output_reset(&z->heater_state);
        if ((z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) &&
            n_active_zones > 1) {
            uint32_t phase_offset_ms = ((uint32_t)active_rank * z->heater_cfg.window_ms) / n_active_zones;
            heater_output_seed_phase(&z->heater_state, z->heater_cfg.window_ms, phase_offset_ms);
        }
        active_rank++;

        float max_temp_c = 0.0f, min_temp_c = -20.0f, sanity_rate = 0.0f;
        float cross_zone_delta_c = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
        zones_config_get_sanity_rate(zi, &sanity_rate);
        zones_config_get_cross_zone_delta(zi, &cross_zone_delta_c);
        /* TODO.md 6A.3's remaining named thresholds: raw pass-through, 0 and
         * all, straight from zones_config_get_guard_thresholds() -- unlike
         * sanity_rate_c_per_min above (whose 0->default substitution happens
         * HERE, at the caller), these substitute inside thermal_guard.c
         * itself (effective_f()/effective_ticks()), so there is exactly one
         * place that owns each fallback constant rather than two copies that
         * can drift apart. */
        float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
        float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
        zones_config_get_guard_thresholds(zi, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                          &runaway_margin, &drift_period_s, &debounce_ticks, &frozen_window_s);
        /* The five v8 overrides, same raw pass-through: thermal_guard.c owns
         * every 0->default substitution. */
        float progress_duty_min = 0.0f, progress_window_s = 0.0f, drift_hysteresis_c = 0.0f;
        float frozen_eps_c = 0.0f, cross_zone_period_s = 0.0f;
        zones_config_get_guard_extra(zi, &progress_duty_min, &progress_window_s, &drift_hysteresis_c,
                                     &frozen_eps_c, &cross_zone_period_s);
        z->guard_cfg = (thermal_guard_cfg_t){
            .max_temp_c = max_temp_c, .min_temp_c = min_temp_c,
            .sanity_rate_c_per_min = (sanity_rate > 0.0f) ? sanity_rate : PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN,
            .wrong_dir_window_s = wd_window_s,
            .wrong_dir_rate_c_per_min = wd_rate,
            .off_settle_s = off_settle_s,
            .runaway_rate_c_per_min = runaway_rate,
            .runaway_margin_c = runaway_margin,
            .drift_period_s = drift_period_s,
            .sensor_fault_debounce_ticks = debounce_ticks,
            .frozen_window_s = frozen_window_s,
            /* Guard 8: whatever the operator entered on the zones page, and
             * 0 (the default) still means disabled. No substituted default
             * here on purpose -- the number is supposed to come from a
             * measured cross-gain matrix (TODO.md 6A.5's last bullet), so
             * the firmware offers the field rather than inventing a value.
             * The period is now an operator field too (v8): 0 still means
             * thermal_guard.c's CROSS_ZONE_PERIOD_S_DEFAULT (600 s), so a
             * board that never sets it behaves exactly as before. */
            .cross_zone_max_delta_c = cross_zone_delta_c,
            .cross_zone_period_s = cross_zone_period_s,
            .progress_duty_min = progress_duty_min,
            .progress_window_s = progress_window_s,
            .drift_hysteresis_c = drift_hysteresis_c,
            .frozen_eps_c = frozen_eps_c,
        };
        thermal_guard_reset(&z->guard_state);

        /* Ramp baseline: the first active zone's actual (calibrated)
         * reading if we have one, else the segment's own target (makes
         * ramp math a no-op rather than ramping from a fabricated zero).
         * TODO.md 10.8: this must be the same COMBINED reading the very
         * first control tick will compute for this zone (see the main read
         * block above), not just its legacy same-index channel -- otherwise
         * a multi-thermocouple zone would start its ramp math from a
         * different number than the tick right after it settles on. */
        if ((int8_t)zi == first_active &&
            (sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->initialized))) {
            MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
            size_t count = 0;
            if (sim_backend_enabled()) {
                sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
            } else {
                MAX31856_read_all(s_exec.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
            }
            float base_ch_c[MAX31856_CHANNEL_COUNT];
            bool base_ch_ok[MAX31856_CHANNEL_COUNT];
            for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
                base_ch_c[ci] = NAN;
                base_ch_ok[ci] = false;
            }
            for (size_t i = 0; i < count; i++) {
                uint8_t ci = readings[i].channel;
                if (ci >= MAX31856_CHANNEL_COUNT) continue;
                base_ch_c[ci] = readings[i].tc_temperature_c;
                base_ch_ok[ci] = !readings[i].spi_failed && !isnan(base_ch_c[ci]);
            }
            uint8_t base_tmask = 0;
            zones_config_get_thermo_mask(zi, &base_tmask);
            bool base_valid = false;
            float base_combined =
                thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, base_tmask, &base_valid);
            if (base_valid) {
                baseline_target_c = zones_config_apply_cal(zi, base_combined);
            }

            /* Warm-start's "current temperature" (Q4): the coolest ACTIVE
             * zone's own combined+calibrated reading, from this same
             * already-fetched `readings` array -- every active zone gets its
             * own thermo_mask/thermo_combine/apply_cal treatment here (not
             * just first_active's), because a zone other than first_active
             * can legitimately be the coolest one and skipping its
             * temperature would risk skipping work it still needs. This
             * block runs exactly once (gated on zi == first_active, same as
             * the baseline_target_c read above), so it loops over every
             * active zone itself rather than relying on the outer loop's
             * per-zi iteration to reach it. */
            for (uint8_t wzi = 0; wzi < MAX31856_CHANNEL_COUNT; wzi++) {
                if (!(p.zone_mask & (1u << wzi))) continue;
                uint8_t w_tmask = 0;
                zones_config_get_thermo_mask(wzi, &w_tmask);
                bool w_valid = false;
                float w_combined = thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, w_tmask, &w_valid);
                if (!w_valid) continue;
                float w_c = zones_config_apply_cal(wzi, w_combined);
                if (isnan(warm_start_coolest_c) || w_c < warm_start_coolest_c) {
                    warm_start_coolest_c = w_c;
                }
            }

            /* Feedforward's ambient reference (TODO.md 6A.2), taken from THIS
             * read rather than a second one: the cold junction is only honest
             * about the room before the firing has warmed the board, and this
             * is the last moment that is true. Any channel's CJ will do -- all
             * five sit on the same board within centimetres of each other, and
             * accepting the first valid one means a single dead or CJRANGE-
             * flagged channel doesn't cost the whole run its ambient. */
            for (size_t i = 0; i < count; i++) {
                if (!readings[i].spi_failed && !isnan(readings[i].cj_temperature_c)) {
                    s_exec.ambient_c = readings[i].cj_temperature_c;
                    s_exec.ambient_from_cj = true;
                    break;
                }
            }
        }
    }

    /* Warm-start (PROFILES.md, owner request 2026-08-30): decide once, here,
     * before the ramp-lock/segment-stepping machinery ever runs its first
     * tick -- see profile_executor_plan_warm_start()'s own doc comment for
     * the entry-point algorithm and profile_exec_status_t's warm_started
     * field for what's reported back to the operator (Q6). Only APPLIED
     * (segment_index/dwelling/segment_elapsed_s/baseline_target_c
     * overridden) when the plan actually warm-started -- the "not
     * warm-started" branch leaves every one of those exactly as the
     * pre-feature code already set them, byte-for-byte. */
    {
        profile_warm_start_plan_t plan = profile_executor_plan_warm_start(&p, warm_start_coolest_c);
        if (plan.warm_started) {
            s_exec.segment_index = plan.entry_segment_index;
            s_exec.dwelling = plan.entry_dwelling;
            s_exec.segment_elapsed_s = plan.entry_segment_elapsed_s;
            baseline_target_c = plan.entry_target_c;

            s_exec.warm_started = true;
            snprintf(s_exec.warm_start_reason, sizeof(s_exec.warm_start_reason),
                     "starting at segment %u -- kiln already at %.1f C",
                     (unsigned)plan.entry_segment_index + 1, (double)warm_start_coolest_c);
            ESP_LOGI(TAG, "warm start: %s (dwelling=%d, entry target %.1fC, %lus into the entry segment)",
                     s_exec.warm_start_reason, (int)plan.entry_dwelling, (double)plan.entry_target_c,
                     (unsigned long)plan.entry_segment_elapsed_s);

            /* Q1 owner decision: replay every skipped RELAY_IO segment's
             * on/off command, in profile order, before the first ramp tick
             * -- reusing io_seg_start() gets both the hardware write and the
             * relay_authority claim/claimed_relay_mask registration this
             * needs "for free", identical to how a segment reached normally
             * would be started. The one deliberate difference: forcing
             * `blocking = true` afterward makes io_segs_tick() (which skips
             * any segment with blocking == true) leave this segment alone
             * for the rest of the run -- its own hold/dwell_min timer is
             * NOT replayed (that schedule position is already past), only
             * the command is. It is retired the same way a real blocking
             * segment's command is: by the end-of-run sweep
             * (io_segs_force_all_off(), honoring leave_on_at_end only on the
             * clean DONE path, same as always) -- exactly the registration
             * this decision requires so a replayed relay is never left
             * energized with nothing owning it. */
            for (uint8_t i = 0; i < plan.entry_segment_index && i < p.segment_count; i++) {
                if (p.segments[i].seg_kind != PROFILE_SEG_KIND_RELAY_IO) {
                    continue;
                }
                io_seg_start(i, &p.segments[i]);
                s_exec.io_segs[i].blocking = true;
                if (s_exec.warm_start_replayed_count < PROFILE_MAX_SEGMENTS) {
                    s_exec.warm_start_replayed_segments[s_exec.warm_start_replayed_count++] = i;
                }
                ESP_LOGI(TAG, "warm start: replayed relay/IO segment %u command (%s %u %s) -- its own hold "
                              "was NOT restarted, it stays as commanded until the run ends",
                         i + 1, s_exec.io_segs[i].is_relay ? "relay" : "IO_",
                         s_exec.io_segs[i].is_relay
                             ? s_exec.io_segs[i].target
                             : (uint8_t)(s_exec.io_segs[i].target - PROFILE_IO_TARGET_IO_BASE + 1u),
                         s_exec.io_segs[i].state_on ? "ON" : "OFF");
            }
        }
    }

    s_exec.target_c = baseline_target_c;
    s_exec.run_start_c = baseline_target_c;
    s_exec.total_elapsed_s = 0;
    s_exec.history_zone = (first_active >= 0) ? (uint8_t)first_active : 0;

    /* One line per firing recording what feedforward will run on, because it
     * is the difference between two firings of the same profile behaving
     * differently and there is no other record of it: which zones have a model
     * at all, and which ambient the hold term is measured against. */
    if (s_exec.ambient_from_cj) {
        ESP_LOGI(TAG, "feedforward ambient reference: %.1fC (cold junction at firing start, not re-sampled)",
                 (double)s_exec.ambient_c);
    } else {
        ESP_LOGW(TAG, "no valid cold-junction reading at firing start -- feedforward ambient falls back to "
                      "%.1fC; the hold term is off by (true ambient - %.1f)/K_dc, a few percent of duty at most",
                 (double)FALLBACK_AMBIENT_C, (double)FALLBACK_AMBIENT_C);
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        if (s_exec.zones[zi].ff_enabled) {
            /* PID_EXPANSION_PLAN.md section 2c/Phase 3b: report whether this
             * zone's coupling row can contribute anything this firing, same
             * as the K_dc/tau line above is the per-firing record of the
             * base model -- an un-commissioned kiln (every coupling_coeff
             * 0, the migration default) logs "coupling OFF" and behaves
             * exactly as before this existed.
             *
             * Post-review fix (finding 4): a neighbour only counts here if
             * zone_feedforward() would actually use it -- same
             * zone_qualifies_as_coupling_neighbor() gate, not just "has a
             * nonzero coefficient". Before this fix the log said "coupling
             * ON: N neighbor(s)" from the coefficient table alone, so an
             * operator could see "coupling ON: 2 neighbour(s)" for a firing
             * where both neighbours were OFF/blocked/faulted and every one
             * of them was excluded at runtime -- the log claimed coupling
             * was active while it contributed exactly 0.0 all firing. Note
             * this is evaluated once, at firing start: a neighbour that
             * changes qualification mid-firing (an operator flips it to OFF,
             * a guard trips it) is not re-logged -- this line is a
             * per-firing summary of what coupling was set up to do, not a
             * live status feed (that's GET /api/profile_exec). */
            uint8_t coupling_neighbors = count_qualifying_coupling_neighbors(zi);
            bool coupling_any = coupling_neighbors > 0;
            if (coupling_any) {
                ESP_LOGI(TAG, "zone %u feedforward ON: K_dc %.4g C/duty, tau %.4gs (TODO.md 6A.2), "
                              "coupling ON: %u neighbor(s) with a measured coefficient (PID_EXPANSION_PLAN.md 2c)",
                         zi, (double)s_exec.zones[zi].ff_k_dc, (double)s_exec.zones[zi].ff_tau_s,
                         coupling_neighbors);
            } else {
                ESP_LOGI(TAG, "zone %u feedforward ON: K_dc %.4g C/duty, tau %.4gs (TODO.md 6A.2), "
                              "coupling OFF: no measured coefficient for any neighbor",
                         zi, (double)s_exec.zones[zi].ff_k_dc, (double)s_exec.zones[zi].ff_tau_s);
            }
        } else {
            ESP_LOGI(TAG, "zone %u feedforward OFF: no identified plant model (run autotune) -- "
                          "feedback alone, unchanged from before 6A.2's feedforward existed", zi);
        }
    }

    TickType_t now = xTaskGetTickCount();
    s_exec.prev_control_tick = now;
    s_exec.history_run_start_tick = now;
    s_exec.history_last_sample_tick = now;
    s_exec.history_count = 0;
    s_exec.history_head = 0;

    /* The atomic gate (relay_authority.h's heat-claim doc comment): this
     * function's own zones_current_sweep_is_active() check far above is a
     * plain, non-atomic read of zones_http.c's state, made before s_exec.lock
     * was even taken -- a sweep can start in the window between that read
     * and this commit. This call is the last possible moment before the
     * commit, s_exec.lock has been held continuously since the "already
     * running"/"faulted" checks confirmed this is a genuine start (not a
     * reentrant call on an already-RUNNING instance -- see
     * relay_heat_zone_claimant_t's doc comment for why that ordering is what
     * makes release_profile_relay_claim()'s unconditional _end() call safe),
     * and it is a single mutex-protected test-and-set against
     * zones_http.c's/autotune_engine.c's matching gates. Refused with the
     * SAME message the early check already reports for the common
     * (non-race) case. */
    if (!relay_authority_heat_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE)) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- it cannot run at the same time as a firing");
        }
        return false;
    }

    /* TODO.md section 0's ownership decision, closing the "manual relay
     * control is not blocked during a firing" gap: claim every relay this
     * run touches so /api/relay and the UART SET_RELAY* commands refuse a
     * manual command against it until pause/halt hands it back. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_PROFILE);

    /* Ask the safety processor to permit heating -- i.e. close K4. THE fix
     * of 2026-08-29: this call did not exist, so every firing this firmware
     * has ever run closed its own zone relay (K1) and left K4 open, and no
     * element current ever flowed on the normal path. See heat_enable.h.
     *
     * Placed here, at the commit point, and not earlier: every refusal above
     * returns without having asked for anything, and from this statement on
     * the run is RUNNING and will command heat. A false return is NOT a
     * reason to refuse the run -- the only way it can fail is a safety link
     * that is down, which apply_relay()'s relay_authority_zone_blocked()
     * check already reports per zone through heat_blocked/
     * heat_blocked_sources and which heat_enable_reconcile() (called from
     * the watchdog task) retries. heat_enable.c logs the failure loudly. */
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);

    s_exec.state = PROFILE_EXEC_RUNNING;
    run_snapshot_buf_t start_snap;
    capture_run_snapshot(&start_snap);
    xSemaphoreGive(s_exec.lock);

    /* First write of this run's breadcrumb, and the one that overwrites any
     * previous run's record in flash. From here on the stored record says a
     * firing is in progress until something records an ending. */
    run_state_note(RUN_STATE_PHASE_RUNNING, &start_snap.snap);

    ESP_LOGI(TAG, "profile '%s' (id %u, zone_mask 0x%02X) running", p.name, profile_id, p.zone_mask);
    return true;
}

void profile_executor_halt(void)
{
    /* See profile_executor_run()'s guard comment above -- s_exec.lock is
     * NULL until profile_executor_start() runs. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(TAG, "profile_executor_halt() called before profile_executor_start() -- refused");
        return;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        xSemaphoreGive(s_exec.lock);
        return;
    }
    force_all_relays_off();
    /* An operator halt is an abnormal stop for the segment machinery too --
     * force off regardless of leave_on_at_end, same as a guard trip. An
     * operator stopping a firing on purpose is not the "reached its own
     * planned end" case that flag exists for. */
    io_segs_force_all_off(false);
    /* Hand every relay this run ever claimed back to unowned -- a halted run
     * owns nothing, and the next run (or a manual command) starts clean. */
    relay_authority_release_mask(s_exec.claimed_relay_mask);
    /* ...and give back the shared heat claim too (relay_authority.h) -- a
     * halt from RUNNING or PAUSED must free the whole-board sweep to start,
     * not just this run's relays. Not routed through release_profile_relay_
     * claim() (unlike every OTHER terminal transition) because that
     * function's single relay_authority_release_mask() call is this one's
     * near-duplicate, not something halt() can share without also pulling
     * in its own separate lock-held/state-transition assumptions -- calling
     * both here inline keeps halt() self-contained the way it already is.
     * Safe unconditionally, same no-op-if-never-held reasoning as
     * release_profile_relay_claim()'s call. */
    relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
    /* ...and K4, inline for the same reason the two calls above are inline
     * rather than routed through release_profile_relay_claim(). After
     * force_all_relays_off() above, never before it. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    /* Captured BEFORE the fault fields are cleared below: if this halt is the
     * operator acknowledging a trip, the reason for that trip is the most
     * useful thing the breadcrumb can carry, and clearing it first would
     * throw it away. */
    run_snapshot_buf_t halt_snap;
    capture_run_snapshot(&halt_snap);
    profile_exec_state_t state_at_halt = s_exec.state;
    /* PID_EXPANSION_PLAN.md Phase 7a: the OTHER ending the tick loop's DONE/
     * FAULTED branch doesn't see -- an operator Stop straight out of RUNNING
     * or PAUSED. firing_stats_maybe_finalize()'s own fs_persisted/IDLE guard
     * makes this a no-op when state_at_halt is DONE/FAULTED (already
     * persisted by the tick loop above) or when this run never produced a
     * single tick worth persisting. Must happen before s_exec.state is reset
     * to IDLE just below -- firing_stats_build_record() reads s_exec.profile/
     * zones/total_elapsed_s, all still this run's values here. */
    bool fs_need_persist = false;
    profile_firing_run_record_t fs_rec;
    fs_need_persist = firing_stats_maybe_finalize(&fs_rec);
    clear_this_runs_faults();
    s_exec.state = PROFILE_EXEC_IDLE;
    s_exec.fault_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    xSemaphoreGive(s_exec.lock);
    if (fs_need_persist) {
        firing_stats_persist(&fs_rec);
    }

    /* An operator halt is a CLEAN end -- that is the whole point of recording
     * it. Without this write the record would still say RUNNING, and the next
     * boot would report a firing the operator deliberately stopped as one the
     * power cut short. A halt that acknowledges a latched trip keeps the
     * FAULTED phase instead, and dismissing a finished run keeps DONE: "a
     * guard stopped it" and "it ran to completion" are truer summaries of
     * those firings than "the operator stopped it", and halt() is also how
     * both of those states are dismissed from the dashboard. Only a halt out
     * of RUNNING/PAUSED is genuinely an operator stop. Either way the record
     * ends up marked ended, which is the property that matters. */
    run_state_phase_t end_phase = RUN_STATE_PHASE_HALTED;
    if (state_at_halt == PROFILE_EXEC_FAULTED) {
        end_phase = RUN_STATE_PHASE_FAULTED;
    } else if (state_at_halt == PROFILE_EXEC_DONE) {
        end_phase = RUN_STATE_PHASE_DONE;
    }
    run_state_note(end_phase, &halt_snap.snap);

    /* Natural end point for the contact-cycle counter: force a write now
     * rather than waiting out the 10-minute interval, so a firing's relay
     * wear survives a power-down right after it stops. Outside the lock --
     * relay_cycles.c takes its own. */
    relay_cycles_flush();
    ESP_LOGI(TAG, "profile executor halted");
}

bool profile_executor_pause(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(TAG, "profile_executor_pause() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_RUNNING) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
    force_all_relays_off();
    /* A pause gives K4 back, even though the relay claim is only handed to
     * MANUAL rather than released (see the comment just below). The two are
     * not the same question: keeping this run's relays reserved for a resume
     * costs nothing, whereas leaving the safety processor permitting heat
     * across a pause of unknown length -- possibly forever, if nobody ever
     * resumes -- means the ONE interlock that stands between a stuck relay
     * and a live element is held open by a firing that is not driving
     * anything. profile_executor_resume() re-acquires; heat_enable_acquire()
     * is a single frame, so nothing about that is expensive. Also consistent
     * with heater_output_force_off()'s own treatment of a pause as a full
     * de-energize that bypasses the min-on-time hold. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    /* TODO.md section 0: pausing is the one explicit way to hand a
     * PROFILE-owned relay back to MANUAL (not NONE -- a paused firing still
     * "belongs" to the operator's session, it's just not driving right now;
     * resuming reclaims PROFILE below). Chosen over auto-pause-on-touch so
     * a manual command never has the side effect of pausing a firing. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_MANUAL);
    s_exec.state = PROFILE_EXEC_PAUSED;
    run_snapshot_buf_t pause_snap;
    capture_run_snapshot(&pause_snap);
    xSemaphoreGive(s_exec.lock);

    /* PAUSED is recorded but is NOT an ending (see run_state.h): a firing
     * paused at 2am and never resumed because the power failed is still an
     * interrupted firing, and the operator deserves to be told so. Recording
     * it at all is what makes the segment progress accurate at the moment
     * the ramp/dwell clock stopped -- the periodic refresh is RUNNING-only. */
    run_state_note(RUN_STATE_PHASE_PAUSED, &pause_snap.snap);
    return true;
}

bool profile_executor_resume(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(TAG, "profile_executor_resume() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_PAUSED) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
    /* Reclaim PROFILE ownership handed to MANUAL on pause -- see
     * profile_executor_pause()'s comment. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_PROFILE);
    /* Re-ask for K4, released on pause -- see profile_executor_pause()'s
     * comment. Same "not a reason to refuse" handling as the start path. */
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    /* Shared ramp/dwell state (target_c, segment_elapsed_s) is untouched by
     * pause -- the control task simply doesn't tick it while PAUSED, so
     * there's nothing to un-shift on resume (unlike the old tick-delta-
     * based timing this replaced, which needed to shift phase_start_tick by
     * the paused duration). prev_control_tick is reset so the next tick's
     * measured dt_s doesn't include the whole pause. */
    s_exec.prev_control_tick = xTaskGetTickCount();
    /* Bumpless transfer (TODO.md 6A.2): each active PID-mode zone resumes
     * as if it had been driving u=0 the whole pause (relays were off),
     * rather than an integral that jumps on the first post-resume tick.
     *
     * With feedforward on, u=0 is not reachable from a zero integral -- the
     * model contributes its hold duty the moment the loop runs again. So this
     * seeds the integral to 0 (see seed_bumpless_with_ff()) and the zone comes
     * back at exactly its feedforward duty: the model's own estimate of what
     * the setpoint costs to hold, with nothing accumulated on top. That is the
     * right place to restart from -- resuming a firing means resuming the heat
     * it needs -- and it is still bumpless in the sense that matters, no
     * integrator windup survives the pause. */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &s_exec.zones[zi];
        if (z->active && !z->faulted &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) &&
            z->actual_valid) {
            seed_bumpless_with_ff(z, zi, 0.0f);
            z->fuzzy_prev_effective_ki = z->pid_cfg.ki; /* see reload_zone_config()'s same reasoning */
        }
    }
    s_exec.state = PROFILE_EXEC_RUNNING;
    run_snapshot_buf_t resume_snap;
    capture_run_snapshot(&resume_snap);
    xSemaphoreGive(s_exec.lock);

    /* Back to "in progress" -- and it must be written now rather than left to
     * the periodic refresh, or a brownout minutes after a resume would show
     * the firing as paused when it was actively driving elements. */
    run_state_note(RUN_STATE_PHASE_RUNNING, &resume_snap.snap);
    return true;
}

void profile_executor_get_status(profile_exec_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* See profile_executor_run()'s guard comment above -- this is the exact
     * call chain (safety_poll_task -> safety_build_and_send_context() ->
     * profile_executor_get_status()) that panicked on the bench. The zeroed
     * struct above already reads as a well-formed IDLE snapshot
     * (PROFILE_EXEC_IDLE == 0), so a caller here needs nothing more than
     * "don't touch the NULL lock". */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_status() called before profile_executor_start() -- reporting IDLE");
        return;
    }

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    out->state = s_exec.state;
    if (s_exec.state != PROFILE_EXEC_IDLE) {
        out->profile_id = s_exec.profile_id;
        strncpy(out->profile_name, s_exec.profile.name, sizeof(out->profile_name) - 1);
        out->zone_mask = s_exec.profile.zone_mask;
        out->segment_index = s_exec.segment_index;
        out->segment_count = s_exec.profile.segment_count;
        out->dwelling = s_exec.dwelling;
        out->target_c = s_exec.target_c;
        out->segment_elapsed_s = s_exec.segment_elapsed_s;
        out->ramp_lock_held = s_exec.ramp_lock_held;
        out->ramp_lock_lagging_mask = s_exec.ramp_lock_lagging_mask;
        out->run_start_c = s_exec.run_start_c;
        out->total_elapsed_s = s_exec.total_elapsed_s;
        out->warm_started = s_exec.warm_started;
        if (s_exec.warm_started) {
            strncpy(out->warm_start_reason, s_exec.warm_start_reason, sizeof(out->warm_start_reason) - 1);
            out->warm_start_replayed_count = s_exec.warm_start_replayed_count;
            memcpy(out->warm_start_replayed_segments, s_exec.warm_start_replayed_segments,
                   sizeof(out->warm_start_replayed_segments));
        }
        size_t seg_n = s_exec.profile.segment_count;
        if (seg_n > PROFILE_MAX_SEGMENTS) seg_n = PROFILE_MAX_SEGMENTS;
        memcpy(out->segments, s_exec.profile.segments, seg_n * sizeof(out->segments[0]));

        if (s_exec.dwelling) {
            const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index < s_exec.profile.segment_count
                                                                         ? s_exec.segment_index
                                                                         : s_exec.profile.segment_count - 1];
            uint32_t dwell_total_s = seg->dwell_min * 60u;
            out->dwell_remaining_s = s_exec.segment_elapsed_s >= dwell_total_s ? 0 : dwell_total_s - s_exec.segment_elapsed_s;
        }

        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            const zone_runtime_t *z = &s_exec.zones[zi];
            profile_exec_zone_status_t *zo = &out->zones[zi];
            zo->active = z->active;
            if (!z->active) continue;
            zo->actual_c = z->actual_c;
            zo->actual_valid = z->actual_valid;
            zo->relay_commanded_on = z->relay_commanded_on;
            zo->duty = z->duty;
            zo->control_mode = (uint8_t)z->control_mode;
            zo->faulted = z->faulted;
            if (z->faulted) {
                strncpy(zo->fault_reason, z->fault_reason, sizeof(zo->fault_reason) - 1);
                zo->fault_guard = (uint8_t)z->fault_guard;
            }
            zo->pid_p = z->last_pid_terms.p;
            zo->pid_i = z->last_pid_terms.i;
            zo->pid_d = z->last_pid_terms.d;
            zo->pid_ff = z->last_pid_terms.ff;
            zo->cooling_limited = z->cooling_limited;
            zo->heat_blocked = z->heat_blocked;
            zo->heat_blocked_sources = z->heat_blocked_sources;
            zo->ff_hold_used_matrix = z->ff_hold_used_matrix;
            zo->ff_hold_infeasible = z->ff_hold_infeasible;
            zo->ff_membership_change_count = z->ff_membership_change_count;

            /* PID_EXPANSION_PLAN.md Phase 7a: live tracking-quality
             * snapshot, same derivation used for the persisted record --
             * see firing_stats_snapshot()'s doc comment. span uses the
             * run's current fs_target_min_c/max_c even mid-run, so this
             * live figure converges toward (but does not exactly equal, for
             * a still-narrowing/widening span) the final persisted one. */
            {
                float span = fabsf(s_exec.fs_target_max_c - s_exec.fs_target_min_c);
                if (isnan(s_exec.fs_target_min_c) || isnan(s_exec.fs_target_max_c)) {
                    span = 0.0f;
                }
                firing_stats_snapshot(z, span, &zo->firing_stats);
            }
        }

        if (s_exec.state == PROFILE_EXEC_FAULTED) {
            strncpy(out->fault_reason, s_exec.fault_reason, sizeof(out->fault_reason) - 1);
            out->fault_guard = (uint8_t)s_exec.fault_guard;
        }
    }
    xSemaphoreGive(s_exec.lock);
}

bool profile_executor_zone_is_active(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_zone_is_active() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    bool active = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) &&
                  (s_exec.profile.zone_mask & (1u << zone_index)) != 0;
    xSemaphoreGive(s_exec.lock);
    return active;
}

size_t profile_executor_get_firing_history(uint8_t profile_id, profile_firing_run_record_t *out,
                                            size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    /* NVS-only, no s_exec/lock -- see firing_stats_load()'s own doc comment.
     * Safe from any task/state, including before profile_executor_start(). */
    profile_firing_history_blob_t blob;
    if (!firing_stats_load(profile_id, &blob)) {
        return 0;
    }
    size_t n = blob.count;
    if (n > PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH) n = PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH; /* corrupt-blob guard */
    if (n > max_entries) n = max_entries;
    memcpy(out, blob.runs, n * sizeof(blob.runs[0])); /* runs[0] = newest, matches this function's contract */
    return n;
}

size_t profile_executor_get_history_count(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_history_count() called before profile_executor_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    size_t count = s_exec.history_count;
    xSemaphoreGive(s_exec.lock);
    return count;
}

size_t profile_executor_get_history(profile_history_entry_t *out, size_t start_index, size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_history() called before profile_executor_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (start_index >= s_exec.history_count) {
        xSemaphoreGive(s_exec.lock);
        return 0;
    }
    size_t count = s_exec.history_count - start_index;
    if (count > max_entries) {
        count = max_entries;
    }
    /* Oldest-first: history_head is the next WRITE slot, so the oldest
     * valid entry (once the buffer has wrapped) is exactly history_head;
     * before it wraps, the oldest is index 0. start_index is relative to
     * that chronological ordering, not the raw array index. */
    uint16_t oldest = (s_exec.history_count < HISTORY_MAX_SAMPLES) ? 0 : s_exec.history_head;
    for (size_t i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)((oldest + start_index + i) % HISTORY_MAX_SAMPLES);
        history_unpack(&s_exec.history[idx], &out[i]);
    }
    xSemaphoreGive(s_exec.lock);
    return count;
}
