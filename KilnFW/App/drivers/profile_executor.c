#include "profile_executor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "autotune_engine.h"
#include "heater_output.h"
#include "pid.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "sim_backend.h"
#include "zones_http.h"

static const char *TAG = "profile_executor";

/* Time-proportioning window defaults (TODO.md 6A.1), used when a zone
 * hasn't configured its own via zones_config_get_heater_cfg() (6A.9). 60s
 * matches the doc's "mechanical relay" default (the open question about SSR
 * vs. direct element drive is still open per TODO.md 6A.0). */
#define HEATER_WINDOW_MS 60000u
#define HEATER_MIN_ON_MS 2000u
#define HEATER_MIN_OFF_MS 2000u

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
    char     fault_reason[96];
    thermal_guard_trip_t fault_guard;
} zone_runtime_t;

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

    /* Which GLOBAL fault this run asserted, if any -- so halt() clears
     * exactly that and nothing another caller may have separately asserted. */
    uint32_t global_fault_source; /* 0 = none asserted by this run */
    char     fault_reason[96];
    thermal_guard_trip_t fault_guard;

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

/* The feedforward duty for one zone, already clamped to [0,1]. Returns exactly
 * 0.0f -- i.e. the behaviour of every firing before this existed -- for a zone
 * with no usable model.
 *
 * The SUM is clamped, not each term: on a cooling ramp the second term is
 * legitimately negative and is supposed to reduce the hold duty below what a
 * steady hold would need. Clamping the terms separately would throw that away
 * and hold the kiln up through a controlled cool. */
static float zone_feedforward(const zone_runtime_t *z, float setpoint_c, float rate_c_per_s)
{
    if (!z->ff_enabled) {
        return 0.0f;
    }
    float hold = (setpoint_c - s_exec.ambient_c) / z->ff_k_dc;
    float climb = (rate_c_per_s * z->ff_tau_s) / z->ff_k_dc;
    float u_ff = hold + climb;
    if (!isfinite(u_ff)) {
        return 0.0f; /* belt-and-braces: a non-finite setpoint can only come
                      * from a corrupted profile, but it must not become duty */
    }
    if (u_ff < 0.0f) u_ff = 0.0f;
    if (u_ff > 1.0f) u_ff = 1.0f;
    return u_ff;
}

/* Seeds the PID integral so the very next tick reproduces u_desired -- with
 * the feedforward term accounted for. pid_seed_bumpless() solves
 * integral = (u_desired - P)/Ki, which was exact while ff was always 0, but
 * the tick it seeds now computes P + I + D + ff: seeding against u_desired
 * directly would come back one whole feedforward term HIGH, and on a hot kiln
 * ff is the largest term in the sum. Handing it (u_desired - u_ff) instead
 * keeps "reproduce the duty this zone was already commanding" true.
 *
 * When u_ff alone already exceeds u_desired the shortfall cannot be expressed
 * -- the integral floor is 0, since a negative one violates the anti-windup
 * clamp on the next tick -- so the zone comes back at its feedforward duty.
 * That is the honest outcome rather than a defect: the model's estimate of
 * what the current setpoint costs to hold is exactly what the operator asked
 * to resume onto. Must be called with s_exec.lock held. */
static void seed_bumpless_with_ff(zone_runtime_t *z, float u_desired)
{
    float u_ff = zone_feedforward(z, s_exec.target_c, s_exec.target_rate_c_per_s);
    pid_seed_bumpless(&z->pid_state, &z->pid_cfg, s_exec.target_c, z->actual_c, u_desired - u_ff);
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

    if (want_on) {
        uint32_t sources = 0;
        if (relay_authority_zone_blocked(s_exec.safety, zi, &sources)) {
            if (s_exec.zones[zi].relay_commanded_on) {
                ESP_LOGW(TAG, "zone %u relay(s) forced off: blocked, sources 0x%02X", zi, (unsigned)sources);
            }
            want_on = false;
        }
    }

    if (s_exec.io) {
        esp_err_t err = kiln_io_set_relay_mask(s_exec.io, mask, want_on ? mask : 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "kiln_io_set_relay_mask failed: %s -- relay state for zone %u is unknown",
                     esp_err_to_name(err), zi);
        }
    }
    /* No-op unless CONFIG_KILNCTL_SIM_PLANT. Fed the POST-gate decision, not
     * what control wanted, so the simulated kiln heats only when a real one
     * would have. */
    sim_backend_note_zone_relay(zi, want_on);
    s_exec.zones[zi].relay_commanded_on = want_on;
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

/* Escalation policy (TODO.md 6A.6, "decide which -- see 6A.6"): guards whose
 * failure mode is severe/board-wide (a welded relay, an out-of-range
 * reading, an electrically faulted sensor) assert the GLOBAL fault source,
 * which blocks every zone via relay_authority_on_blocked() and faults the
 * WHOLE run, not just the tripping zone. Guards whose failure mode is
 * specific to one zone's physics only block that zone, via
 * relay_authority_zone_blocked()'s per-zone mask -- the run continues for
 * any other still-healthy active zone (TODO.md 6A.5's multi-zone execution
 * is what makes that distinction meaningful; with only one zone ever
 * running before this pass, "per-zone" and "whole run" were the same
 * thing). Returns true if this trip faulted the whole run (global trip, or
 * the last active zone just faulted), false if the run continues. Must be
 * called with s_exec.lock held. */
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
        ESP_LOGE(TAG, "every active zone faulted -- whole run faulted");
        return true;
    }
    return false;
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
        esp_err_t err = kiln_io_set_relay_mask(s_exec.io, mask, 0);
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
    esp_err_t err = kiln_io_set_relay_mask(s_exec.io, stray, 0);
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
        if (!mode_changed && z->control_mode == ZONE_CONTROL_MODE_PID) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, z->duty);
            } else {
                /* Seeding off a fabricated measurement would bake this tick's
                 * bad reading into the integral and keep driving from it long
                 * after the sensor recovers. A cold restart costs one
                 * transient; a poisoned integral costs the rest of the run. */
                pid_reset(&z->pid_state);
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
        if (!mode_changed && z->control_mode == ZONE_CONTROL_MODE_PID) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, z->duty);
            } else {
                pid_reset(&z->pid_state); /* same reasoning as the gain path above */
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

    return changed;
}

/* One counter comparison per tick, and on the overwhelmingly common
 * unchanged path that is the entire cost -- no getters, no config walk, no
 * NVS. Must be called with s_exec.lock held, from the RUNNING path only:
 * a PAUSED or FAULTED run has no control math to keep bumpless, and picking
 * the edit up when it resumes (via this same path) is both simpler and
 * closer to what the operator expects. */
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
            xSemaphoreGive(s_exec.lock);
            continue;
        }

        uint32_t dt_ms = ticks_to_ms(now - s_exec.prev_control_tick);
        if (dt_ms == 0) {
            dt_ms = PROFILE_EXECUTOR_TICK_MS;
        }
        s_exec.prev_control_tick = now;
        float dt_s = (float)dt_ms / 1000.0f;

        /* --- Read every active zone's channel (raw, then calibrated) ------- */
        float raw_c[MAX31856_CHANNEL_COUNT];
        bool sensor_ok[MAX31856_CHANNEL_COUNT];
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            raw_c[zi] = NAN;
            sensor_ok[zi] = false;
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
                uint8_t zi = readings[i].channel;
                if (zi >= MAX31856_CHANNEL_COUNT || !s_exec.zones[zi].active) continue;
                raw_c[zi] = readings[i].tc_temperature_c;
                /* THERMO_FAULT_OPEN|OVUV|TCRANGE make the temperature
                 * meaningless per MAX31856Reading's own doc comment --
                 * mirrors thermal_guard.h's sensor_ok contract without
                 * pulling those bit constants into thermal_guard.c. */
                bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
                sensor_ok[zi] = !readings[i].spi_failed && !isnan(raw_c[zi]) && !fault_bits_bad;
            }
        }
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active) continue;
            s_exec.zones[zi].actual_valid = sensor_ok[zi];
            s_exec.zones[zi].actual_c = sensor_ok[zi] ? zones_config_apply_cal(zi, raw_c[zi]) : NAN;
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
            if (!sensor_ok[zi] || fabsf(s_exec.zones[zi].actual_c - s_exec.target_c) > PROFILE_EXECUTOR_RAMP_LOCK_BAND_C) {
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
        if (lock_ok) {
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
            z->last_pid_terms = (pid_terms_t){0}; /* only ZONE_CONTROL_MODE_PID below fills this in */
            switch (z->control_mode) {
            case ZONE_CONTROL_MODE_PID: {
                if (sensor_ok[zi]) {
                    /* TODO.md 6A.2's feedforward. 0.0f -- byte-for-byte the
                     * behaviour of every firing before this -- for any zone
                     * with no identified model, which is every zone that has
                     * never been autotuned. Passed IN to the controller so the
                     * clamp, the anti-windup conditional-integration test and
                     * the term breakdown all see the same number the element
                     * is driven from; the value handed over here is what
                     * /api/control reports as "ff", so the operator can read
                     * the P/I/D/FF split and see how much of the duty is the
                     * model and how much is the loop correcting it. */
                    float u_ff = zone_feedforward(z, s_exec.target_c, s_exec.target_rate_c_per_s);
                    duty = pid_update_terms(&z->pid_state, &z->pid_cfg, s_exec.target_c, z->actual_c, dt_s,
                                            u_ff, &z->last_pid_terms);
                }
                /* Pay back any load-cap-deferred on-time as a duty boost --
                 * only actually consumed below if this tick turns out to
                 * open a fresh window (heater_output_duty() only reads the
                 * duty argument at a window boundary; a boost handed to it
                 * mid-window is silently ignored, so crediting it back here
                 * unconditionally and only debiting on a real boundary
                 * keeps the books exact even if several ticks pass between
                 * boundaries). */
                float boosted_duty = duty;
                float credit_ms = 0.0f;
                if (z->deferred_on_ms > 0.0f && z->heater_cfg.window_ms > 0) {
                    float window_ms_f = (float)z->heater_cfg.window_ms;
                    credit_ms = z->deferred_on_ms;
                    float max_credit_ms = (1.0f - boosted_duty) * window_ms_f;
                    if (credit_ms > max_credit_ms) credit_ms = max_credit_ms;
                    if (credit_ms < 0.0f) credit_ms = 0.0f;
                    boosted_duty += credit_ms / window_ms_f;
                }
                uint32_t elapsed_before = z->heater_state.window_elapsed_ms;
                bool was_started = z->heater_state.window_started;
                want_relay_on[zi] = heater_output_duty(&z->heater_state, &z->heater_cfg, boosted_duty, dt_ms);
                /* A fresh window opened this tick iff window_elapsed_ms got
                 * reset to 0 -- the only place heater_output_duty() sets it
                 * to exactly 0 is the new-window branch (see its comment);
                 * 1Hz ticks against a >=1s window make an accumulation-only
                 * 0 practically impossible. Only then did boosted_duty
                 * actually get baked into on_ms_this_window, so only then
                 * is the credit actually spent. */
                if (credit_ms > 0.0f && z->heater_state.window_elapsed_ms == 0 &&
                    (!was_started || elapsed_before > 0)) {
                    z->deferred_on_ms -= credit_ms;
                    if (z->deferred_on_ms < 0.0f) z->deferred_on_ms = 0.0f;
                }
                break;
            }
            case ZONE_CONTROL_MODE_BANGBANG: {
                bool want_raw = z->relay_commanded_on;
                if (sensor_ok[zi]) {
                    if (z->actual_c < s_exec.target_c - PROFILE_EXECUTOR_HYSTERESIS_C) {
                        want_raw = true;
                    } else if (z->actual_c > s_exec.target_c + PROFILE_EXECUTOR_HYSTERESIS_C) {
                        want_raw = false;
                    }
                } else {
                    want_raw = false; /* no trustworthy reading -> never command heat */
                }
                want_relay_on[zi] = heater_output_bangbang(&z->heater_state, &z->heater_cfg, want_raw, dt_ms);
                duty = want_relay_on[zi] ? 1.0f : 0.0f;
                break;
            }
            case ZONE_CONTROL_MODE_OFF:
            default:
                want_relay_on[zi] = false;
                duty = 0.0f;
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

            thermal_guard_input_t gin = {
                .sensor_ok = sensor_ok[zi],
                .measurement_c = raw_c[zi], /* RAW -- a calibration offset must not hide an out-of-range sensor */
                .setpoint_c = s_exec.target_c,
                .commanded_duty = z->relay_commanded_on ? (z->duty > 0.0f ? z->duty : 1.0f) : 0.0f,
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
            if (thermal_guard_tick(&z->guard_state, &z->guard_cfg, &gin)) {
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_PERIOD_MS));

        bool wdt_faulted = false;
        xSemaphoreTake(s_exec.lock, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        uint32_t since_ms = ticks_to_ms(now - s_exec.last_tick_tick);
        if (since_ms > WATCHDOG_TICK_DEAD_MS) {
            ESP_LOGE(TAG, "control task tick stale for %lums -- forcing relays off (guard 9)",
                     (unsigned long)since_ms);
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            if (s_exec.safety) {
                safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, true);
            }
            if (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) {
                s_exec.state = PROFILE_EXEC_FAULTED;
                snprintf(s_exec.fault_reason, sizeof(s_exec.fault_reason),
                        "control task tick stale for %lums", (unsigned long)since_ms);
                wdt_faulted = true;
            }
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
    BaseType_t ok = xTaskCreatePinnedToCore(executor_task_entry, "profile_executor", 4096, NULL, 5,
                                            &s_exec.task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(profile_executor) failed");
        vSemaphoreDelete(s_exec.lock);
        s_exec.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Small and independent on purpose -- guard 9 exists precisely because
     * the control task cannot be trusted to notice its own death. Same
     * priority as the control task it's watching. */
    ok = xTaskCreatePinnedToCore(watchdog_task_entry, "profile_exec_wdt", 2560, NULL, 5,
                                 &s_exec.watchdog_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(profile_exec_wdt) failed -- guard 9 unavailable this boot");
    }

    ESP_LOGI(TAG, "profile executor up (io_ready=%d, thermo_ready=%d, safety_ready=%d) -- "
                  "NOT YET VERIFIED AGAINST REAL RELAY/THERMOCOUPLE HARDWARE (single- or multi-zone), "
                  "see profile_executor.h",
             io_or_null != NULL, thermo_bus_or_null != NULL && thermo_bus_or_null->initialized,
             safety_or_null != NULL);
    return ESP_OK;
}

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
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
        if (err_msg) snprintf(err_msg, err_cap, "profile targets no zones");
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
                    snprintf(err_msg, err_cap,
                             "segment %u: ramp rate %.1f C/hr exceeds zone %u's current %.1f C/hr ceiling",
                             i + 1, (double)rate, zi, (double)ceiling);
                }
                return false;
            }
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
    /* Feedforward inputs start from their safe values: no ramp commanded yet,
     * and the fallback ambient until a cold junction actually answers below. */
    s_exec.target_rate_c_per_s = 0.0f;
    s_exec.ambient_c = FALLBACK_AMBIENT_C;
    s_exec.ambient_from_cj = false;

    memset(s_exec.zones, 0, sizeof(s_exec.zones));

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

    int8_t first_active = -1;
    uint8_t active_rank = 0;
    float baseline_target_c = p.segments[0].target_c;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        zone_runtime_t *z = &s_exec.zones[zi];
        z->active = true;
        if (first_active < 0) first_active = (int8_t)zi;

        zone_control_mode_t mode = ZONE_CONTROL_MODE_BANGBANG;
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
        if (z->control_mode == ZONE_CONTROL_MODE_PID && n_active_zones > 1) {
            uint32_t phase_offset_ms = ((uint32_t)active_rank * z->heater_cfg.window_ms) / n_active_zones;
            heater_output_seed_phase(&z->heater_state, z->heater_cfg.window_ms, phase_offset_ms);
        }
        active_rank++;

        float max_temp_c = 0.0f, min_temp_c = -20.0f, sanity_rate = 0.0f;
        float cross_zone_delta_c = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
        zones_config_get_sanity_rate(zi, &sanity_rate);
        zones_config_get_cross_zone_delta(zi, &cross_zone_delta_c);
        z->guard_cfg = (thermal_guard_cfg_t){
            .max_temp_c = max_temp_c, .min_temp_c = min_temp_c,
            .sanity_rate_c_per_min = (sanity_rate > 0.0f) ? sanity_rate : PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN,
            /* Guard 8: whatever the operator entered on the zones page, and
             * 0 (the default) still means disabled. No substituted default
             * here on purpose -- the number is supposed to come from a
             * measured cross-gain matrix (TODO.md 6A.5's last bullet), so
             * the firmware offers the field rather than inventing a value.
             * The period stays at thermal_guard.c's CROSS_ZONE_PERIOD_S_
             * DEFAULT (600 s); one knob is enough to arm the guard, and a
             * second one is easier to get wrong than to get value from. */
            .cross_zone_max_delta_c = cross_zone_delta_c,
            .cross_zone_period_s = 0.0f,
        };
        thermal_guard_reset(&z->guard_state);

        /* Ramp baseline: the first active zone's actual (calibrated)
         * reading if we have one, else the segment's own target (makes
         * ramp math a no-op rather than ramping from a fabricated zero). */
        if ((int8_t)zi == first_active &&
            (sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->initialized))) {
            MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
            size_t count = 0;
            if (sim_backend_enabled()) {
                sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
            } else {
                MAX31856_read_all(s_exec.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
            }
            for (size_t i = 0; i < count; i++) {
                if (readings[i].channel == zi && !readings[i].spi_failed && !isnan(readings[i].tc_temperature_c)) {
                    baseline_target_c = zones_config_apply_cal(zi, readings[i].tc_temperature_c);
                    break;
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

    s_exec.target_c = baseline_target_c;
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
            ESP_LOGI(TAG, "zone %u feedforward ON: K_dc %.4g C/duty, tau %.4gs (TODO.md 6A.2)",
                     zi, (double)s_exec.zones[zi].ff_k_dc, (double)s_exec.zones[zi].ff_tau_s);
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
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        xSemaphoreGive(s_exec.lock);
        return;
    }
    force_all_relays_off();
    /* Captured BEFORE the fault fields are cleared below: if this halt is the
     * operator acknowledging a trip, the reason for that trip is the most
     * useful thing the breadcrumb can carry, and clearing it first would
     * throw it away. */
    run_snapshot_buf_t halt_snap;
    capture_run_snapshot(&halt_snap);
    profile_exec_state_t state_at_halt = s_exec.state;
    clear_this_runs_faults();
    s_exec.state = PROFILE_EXEC_IDLE;
    s_exec.fault_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    xSemaphoreGive(s_exec.lock);

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
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_RUNNING) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
    force_all_relays_off();
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
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_PAUSED) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
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
        if (z->active && !z->faulted && z->control_mode == ZONE_CONTROL_MODE_PID && z->actual_valid) {
            seed_bumpless_with_ff(z, 0.0f);
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
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    bool active = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) &&
                  (s_exec.profile.zone_mask & (1u << zone_index)) != 0;
    xSemaphoreGive(s_exec.lock);
    return active;
}

size_t profile_executor_get_history_count(void)
{
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
