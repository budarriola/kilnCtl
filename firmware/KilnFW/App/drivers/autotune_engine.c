#include "autotune_engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "heater_output.h"
#include "kiln_io_owner.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- heat_interlock.h's own doc comment */
#include "profile_executor.h"
#include "relay_authority.h"
#include "sim_backend.h"
#include "thermo_combine.h"
#include "zones_http.h"

static const char *TAG = "autotune_engine";

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

#define HEATER_WINDOW_MS 60000u
#define HEATER_MIN_ON_MS 2000u
#define HEATER_MIN_OFF_MS 2000u

/* "Reached steady state" heuristic for ending STEPPING early (TODO.md 6A.4
 * doesn't mandate a specific detector -- the two-point fit itself is what
 * validates the trace is usable): the last SETTLE_CHECK_SAMPLES samples all
 * within SETTLE_CHECK_BAND_C of each other. */
#define SETTLE_CHECK_SAMPLES 6u
#define SETTLE_CHECK_BAND_C 1.0f
#define MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK 12u /* don't even look until well past the dead-time region */

/* Step-test guards 1/2 fallback headroom -- see the thermal_guard_input_t
 * comment at its use site below for the full defect this fixes. Any strictly
 * positive value works for guard 1: its "did it rise enough" math
 * (thermal_guard.c) never reads the *size* of setpoint_c - measurement_c,
 * only its sign, so this doesn't need to resemble a real target temperature.
 * Kept comfortably under thermal_guard.c's DRIFT_HYSTERESIS_C (25.0f) so
 * guard 4 reads this fallback the same way it always has when no ceiling is
 * configured -- "at setpoint" (abs_error <= hysteresis), i.e. still dormant,
 * not a new false trip. */
#define STEP_TEST_GUARD_HEADROOM_C 5.0f

typedef struct {
    kiln_io_t *io;
    MAX31856BusClass *thermo_bus;
    SafetyLinkClass *safety;

    SemaphoreHandle_t lock;
    TaskHandle_t task;

    autotune_engine_state_t state;
    autotune_method_t method;
    uint8_t  zone_index;
    float    step_duty;

    /* Relay-feedback method only (AUTOTUNE_METHOD_RELAY). relay_on is the
     * current branch of the bang-bang law, held across the band so the relay
     * has real hysteresis rather than switching on every noise wiggle; it is
     * also what the cycle counter watches for edges. */
    float    relay_setpoint_c;
    float    relay_d;
    float    relay_h;
    autotune_rule_t relay_rule;
    bool     relay_on;
    uint16_t relay_cycles_seen;
    relay_model_t relay;

    TickType_t phase_start_tick;
    TickType_t last_sample_tick;
    TickType_t prev_tick;
    uint32_t   elapsed_s; /* time in the current phase, updated every tick */

    heater_output_cfg_t heater_cfg;
    heater_output_state_t heater_state;
    thermal_guard_cfg_t guard_cfg;
    thermal_guard_state_t guard_state;

    bool  per_zone_blocked;
    char  abort_reason[96];

    /* Row-indexed by observed zone, not just the zone under test --
     * TODO.md 6A.5(b): every configured zone is sampled every period while
     * STEPPING, at the same tick, so any zone's trace can be fit against
     * the SAME duty step applied to s_at.zone_index. zone_baseline_c is
     * captured at the same SETTLING->STEPPING transition as the tested
     * zone's own baseline_c used to be. */
    float zone_baseline_c[MAX31856_CHANNEL_COUNT];
    bool  zone_baseline_valid[MAX31856_CHANNEL_COUNT];
    float zone_last_valid_c[MAX31856_CHANNEL_COUNT]; /* carry-forward for a zone's momentary bad read, so one dropped
                                                        * sample on a non-tested zone doesn't NaN its whole trace */
    /* Packed, 2 bytes a sample per zone -- see autotune_engine.h. */
    autotune_trace_sample_t zone_trace[MAX31856_CHANNEL_COUNT][AUTOTUNE_ENGINE_MAX_SAMPLES];
    uint16_t trace_count; /* shared sample index -- every zone's row advances together */

    float actual_c;
    bool  actual_valid;
    float duty;

    fopdt_model_t    model;
    autotune_gains_t proposed_gains;
    float            predicted_max_ramp_c_per_hr;

    autotune_coupling_matrix_t coupling;
} s_at_t;

static s_at_t s_at;

/* "A test is in progress" -- the one definition of it. There are four running
 * states across two methods now, and every place that used to spell out
 * "SETTLING || STEPPING" (relay authority, mutual exclusion with
 * profile_executor, the tick's own early-out, the abort paths) has to agree
 * about all four or the engine can end up driving relays in a state the rest
 * of the firmware believes is idle. */
static bool state_is_running(autotune_engine_state_t s)
{
    return s == AUTOTUNE_ENGINE_SETTLING || s == AUTOTUNE_ENGINE_STEPPING ||
           s == AUTOTUNE_ENGINE_RELAY_APPROACH || s == AUTOTUNE_ENGINE_RELAY_CYCLING;
}

static uint32_t ticks_to_s(TickType_t t) { return (uint32_t)(t / configTICK_RATE_HZ); }
static uint32_t ticks_to_ms(TickType_t t) { return (uint32_t)t * (1000u / configTICK_RATE_HZ); }

/* Must be called with s_at.lock held. */
static void apply_relay(bool want_on)
{
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(s_at.zone_index, &mask) || mask == 0) {
        s_at.duty = 0.0f;
        return;
    }
    if (want_on) {
        uint32_t sources = 0;
        if (relay_authority_zone_blocked(s_at.safety, s_at.zone_index, &sources)) {
            want_on = false;
        }
    }
    if (s_at.io) {
        /* AUTHORIZED, not the manual gate -- see kiln_io_owner.h's top
         * comment and profile_executor.c's apply_relay() for the identical
         * reasoning (2026-08-19, TODO.md 10.14 Phase 1). */
        kiln_io_owner_command_set_relay_mask_authorized(mask, want_on ? mask : 0);
    }
    sim_backend_note_zone_relay(s_at.zone_index, want_on); /* no-op unless CONFIG_KILNCTL_SIM_PLANT */
}

/* Must be called with s_at.lock held. */
static void force_relays_off(void)
{
    heater_output_force_off(&s_at.heater_state);
    apply_relay(false);
    s_at.duty = 0.0f;
    /* TODO.md 6A.6: release the RELAY_OWNER_AUTOTUNE claim begin_run_locked()
     * took on this zone's mask. force_relays_off() is the one function every
     * terminal path (finalize_fit(), finalize_relay_fit(),
     * escalate_and_abort(), abort_locked()) calls before leaving the running
     * states, so it's the single place the claim can be released without
     * duplicating this at each call site. A relay no longer in the zone's
     * mask (reconfigured mid-test) is simply not released here -- harmless,
     * since relay_authority_claim_mask() only ever wrote owners for the bits
     * that were in the mask it was given. */
    uint8_t owned_mask = 0;
    if (zones_config_get_relay_mask(s_at.zone_index, &owned_mask) && owned_mask != 0) {
        relay_authority_release_mask(owned_mask);
    }
}

/* Same escalation split as profile_executor.c's escalate_guard_trip() --
 * duplicated rather than shared because the two modules' state structs
 * differ enough that a shared helper would need to take 4+ out-params; see
 * TODO.md 6A.6 for the policy this mirrors. */
static void escalate_and_abort(thermal_guard_trip_t reason, const char *detail)
{
    bool global = (reason == THERMAL_GUARD_TRIP_RUNAWAY || reason == THERMAL_GUARD_TRIP_MAX_TEMP ||
                  reason == THERMAL_GUARD_TRIP_MIN_TEMP || reason == THERMAL_GUARD_TRIP_SENSOR_INVALID);
    uint32_t source = (reason == THERMAL_GUARD_TRIP_SENSOR_INVALID) ? SAFETY_FAULT_SRC_THERMO
                                                                    : SAFETY_FAULT_SRC_THERMAL_SANITY;
    if (global) {
        if (s_at.safety) {
            safety_link_set_fault_source(s_at.safety, source, true);
        }
    } else {
        relay_authority_set_zone_blocked(s_at.zone_index, true);
        s_at.per_zone_blocked = true;
    }
    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_ABORTED;
    /* Explicit precision (sizeof(abort_reason) - strlen("guard tripped: ") -
     * 1) rather than a bare %s -- under -Os this call started inlining into
     * task_entry, and GCC's -Wformat-truncation can't bound an unadorned %s
     * against a caller-supplied `detail` even though the destination is
     * fixed-size; found building 2026-08-18 (the -Og build never triggered
     * this since the call stayed out-of-line there). Genuinely truncating an
     * overlong detail string here is fine -- abort_reason is a status
     * readout, not parsed anywhere -- this only silences a false positive. */
    snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "guard tripped: %.79s", detail);
    ESP_LOGE(TAG, "autotune zone %u aborted: %s", s_at.zone_index, s_at.abort_reason);
}

static void clear_block_if_any(void)
{
    if (s_at.per_zone_blocked) {
        relay_authority_set_zone_blocked(s_at.zone_index, false);
        s_at.per_zone_blocked = false;
    }
}

/* The single non-guard abort path, extracted from autotune_engine_abort() so
 * the relay method's own aborts (approach budget expired, unusable trace)
 * cannot drift away from what the operator's Abort button does. Guard trips
 * still go through escalate_and_abort() instead, because those additionally
 * have to raise a fault source or block the zone -- an abort that *found
 * something wrong with the kiln* must not clear itself the way a cancelled
 * test does. Must be called with s_at.lock held, and only from a running
 * state. */
static void abort_locked(const char *reason)
{
    force_relays_off();
    clear_block_if_any();
    thermal_guard_clear(&s_at.guard_state);
    s_at.state = AUTOTUNE_ENGINE_ABORTED;
    snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "%s", reason ? reason : "aborted");
}

/* The trace is stored packed (see autotune_engine.h) but pid_autotune.c is
 * pure math over {t_s, measurement_c} pairs and has no business knowing that.
 * Unpacking one zone's row into a scratch buffer for the duration of a fit
 * costs ~11 KB transiently, which is affordable *because* the packing freed
 * far more than that permanently -- and it is on the heap rather than this
 * task's 4 KB stack for the obvious reason. Returns NULL on allocation
 * failure; the caller aborts rather than fitting a partial trace. */
static autotune_sample_t *unpack_zone_trace(uint8_t zone, size_t count)
{
    if (count == 0) {
        return NULL;
    }
    autotune_sample_t *out = malloc(count * sizeof(*out));
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        out[i].t_s = (float)(i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        int16_t dc = s_at.zone_trace[zone][i];
        out[i].measurement_c = (dc == AUTOTUNE_TRACE_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
    }
    return out;
}

static void finalize_fit(void)
{
    float baseline_c = s_at.zone_baseline_c[s_at.zone_index];

    autotune_sample_t *scratch = unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "out of memory unpacking a %u-sample trace", (unsigned)s_at.trace_count);
        ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.model = pid_autotune_fit_fopdt(scratch, s_at.trace_count, baseline_c, s_at.step_duty);
    free(scratch);
    if (!s_at.model.valid) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "fit failed: %s", s_at.model.invalid_reason);
        ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.proposed_gains = pid_autotune_tune_from_fopdt(&s_at.model, AUTOTUNE_RULE_SIMC, 0.0f);
    s_at.predicted_max_ramp_c_per_hr =
        pid_autotune_estimate_max_ramp_c_per_hr(&s_at.model, 1.0f, s_at.actual_valid ? s_at.actual_c : baseline_c,
                                                baseline_c);

    /* TODO.md 6A.5(b): fill row zone_index of the coupling matrix -- the
     * direct cell (i==i) is this same model, every other configured zone
     * gets its own fit against the identical duty step. A zone whose
     * baseline was never valid (no sensor, or thermo_count doesn't cover
     * it) is left invalid rather than fit against garbage. */
    uint8_t thermo_count = zones_config_get_thermo_count();
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        autotune_coupling_cell_t *cell = &s_at.coupling.cell[s_at.zone_index][j];
        if (j == s_at.zone_index) {
            cell->model = s_at.model;
            cell->valid = true;
            continue;
        }
        if (j >= thermo_count || !s_at.zone_baseline_valid[j]) {
            cell->valid = false;
            continue;
        }
        autotune_sample_t *peer = unpack_zone_trace(j, s_at.trace_count);
        if (!peer) {
            /* Only the cross-gain cell is lost; the direct fit above already
             * succeeded and the tuning it produced stands. */
            cell->valid = false;
            ESP_LOGW(TAG, "autotune zone %u: out of memory fitting cross-gain against zone %u",
                     s_at.zone_index, j);
            continue;
        }
        fopdt_model_t m = pid_autotune_fit_fopdt(peer, s_at.trace_count, s_at.zone_baseline_c[j], s_at.step_duty);
        free(peer);
        cell->model = m;
        cell->valid = m.valid;
    }

    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    ESP_LOGI(TAG, "autotune zone %u done: K=%.2f tau=%.1fs L=%.1fs -> Kp=%.5f Ki=%.5f Kd=%.5f", s_at.zone_index,
             (double)s_at.model.k_gain_c_per_duty, (double)s_at.model.tau_s, (double)s_at.model.dead_time_s,
             (double)s_at.proposed_gains.kp, (double)s_at.proposed_gains.ki, (double)s_at.proposed_gains.kd);
}

/* One tick of the bang-bang relay law. Returns the duty this tick wants and
 * reports, through *edge_on_to_off, the moment the relay switched from its
 * high branch to its low one.
 *
 * That single edge is the cycle boundary for everything downstream: it is the
 * instant the measurement first crossed the TOP of the switching band, so
 * consecutive edges bound exactly one period, and starting the recorded trace
 * on one means the trace begins at a known point in the cycle rather than
 * wherever the approach happened to end.
 *
 * The law is evaluated every tick (1 Hz) rather than once per recorded sample
 * (10 s): the switch instants are what set Tu, and quantising them to the
 * trace's sample period would quantise the measured period along with them.
 *
 * Control uses the CALIBRATED reading, the same way profile_executor's loop
 * does -- the operator's setpoint is in the units they read off the page.
 * (Guards separately get the raw value; see the guard input below.)
 *
 * One consequence of centring on 0.5 rather than swinging to full-off is worth
 * stating plainly instead of leaving to be discovered: the low branch is duty
 * 0.15, not 0, so guard 3 (runaway with heat commanded OFF -- the welded-relay
 * check) never opens its window during cycling, because heat is never
 * commanded off. Nothing about the guard is disabled or reconfigured; it
 * simply has no off-period to observe. A welded contact during a relay test is
 * caught instead by guard 5's absolute ceiling and guard 4's drift check, both
 * of which are armed and now have a real setpoint to judge against. The
 * alternative -- a full-off low branch -- would arm guard 3 but would also make
 * it fire on the ordinary case, since a kiln's temperature keeps climbing for
 * minutes after heat is cut and guard 3 trips on exactly that after 120s.
 *
 * Must be called with s_at.lock held. */
static float relay_law_tick(bool sensor_ok, float meas_c, bool *edge_on_to_off)
{
    *edge_on_to_off = false;
    /* A bad reading freezes the branch rather than switching on a guess. The
     * caller zeroes the commanded duty for this tick anyway, and guard 6 ends
     * the run after its own debounce if the sensor really is gone; what must
     * not happen is a dropout being counted as a cycle. */
    if (sensor_ok) {
        bool was_on = s_at.relay_on;
        if (meas_c < s_at.relay_setpoint_c - s_at.relay_h) {
            s_at.relay_on = true;
        } else if (meas_c > s_at.relay_setpoint_c + s_at.relay_h) {
            s_at.relay_on = false;
        } /* inside the band: hold the branch -- this is the hysteresis */
        *edge_on_to_off = was_on && !s_at.relay_on;
    }
    /* u0 +/- d, never clamped: run_relay() has already refused any d that
     * would push a branch outside [0, 1], because a clamped branch would make
     * the real half-amplitude smaller than the d handed to the fit and inflate
     * Ku by exactly that ratio. */
    return AUTOTUNE_RELAY_CENTER_DUTY + (s_at.relay_on ? s_at.relay_d : -s_at.relay_d);
}

/* Writes one row of the packed trace, for both methods.
 *
 * Only the tested zone's row is ever read back for a relay run: the coupling
 * matrix (TODO.md 6A.5(b)) is fitted with pid_autotune_fit_fopdt() against a
 * known duty *step*, and a relay run never applies one, so a relay run
 * deliberately leaves the matrix untouched rather than filling it with cells
 * fitted against an oscillation. The peer rows are still recorded because
 * MAX31856_read_all() already read them and a future cross-gain-from-relay
 * method would want them; they cost nothing extra here.
 *
 * Must be called with s_at.lock held. */
static void record_trace_sample(const float *raw_by_zone, const bool *ok_by_zone)
{
    if (s_at.trace_count >= AUTOTUNE_ENGINE_MAX_SAMPLES) {
        return;
    }
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        float c = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : s_at.zone_last_valid_c[z];
        if (ok_by_zone[z]) s_at.zone_last_valid_c[z] = c;
        /* Packed: t is implicit in the index (see autotune_engine.h),
         * temperature is 0.1degC. */
        int16_t dc;
        if (isnan(c)) {
            dc = AUTOTUNE_TRACE_TEMP_INVALID;
        } else {
            float scaled = c * 10.0f;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32767.0f) scaled = -32767.0f;
            dc = (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        }
        s_at.zone_trace[z][s_at.trace_count] = dc;
    }
    s_at.trace_count++;
}

/* End of a relay run: fit (Ku, Tu) and propose gains, or abort.
 *
 * The asymmetry with finalize_fit() is the point of the whole method. A step
 * test produces a plant MODEL; a relay test produces one point of the
 * frequency response and nothing else -- no K, no tau, no L. So there is no
 * model to propose here, no ramp-ceiling estimate to derive (that needs tau),
 * and no coupling-matrix row to fill.
 *
 * Must be called with s_at.lock held. */
static void finalize_relay_fit(void)
{
    if (s_at.trace_count == 0) {
        /* Distinguished from the allocation failure below because they mean
         * completely different things to whoever reads the abort reason: an
         * empty trace is "the sensor never gave us a usable reading", not
         * "the board ran out of memory". */
        abort_locked("relay test recorded no samples -- no usable readings during cycling");
        ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    autotune_sample_t *scratch = unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "out of memory unpacking a %u-sample trace", (unsigned)s_at.trace_count);
        ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.relay = pid_autotune_fit_relay(scratch, s_at.trace_count, s_at.relay_d, s_at.relay_h);
    free(scratch);

    if (!s_at.relay.valid) {
        /* REJECTED, not approximated. pid_autotune_fit_relay() distinguishes
         * "never oscillated", "cycles still growing/decaying" and "amplitude
         * never escaped the hysteresis band", and every one of those means the
         * plant never reached a limit cycle -- so there is no frequency-response
         * point to have measured and nothing legitimate to propose. Proposing
         * gains from a non-limit-cycle would be inventing a Ku, and the whole
         * value of this method is that Ku came from the kiln. */
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "relay fit rejected: %s", s_at.relay.invalid_reason);
        ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    s_at.proposed_gains = pid_autotune_tune_from_relay(&s_at.relay, s_at.relay_rule);
    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    ESP_LOGI(TAG,
             "autotune zone %u relay done: Ku=%.4f Tu=%.0fs a=%.2fC over %d cycles -> Kp=%.5f Ki=%.5f Kd=%.5f",
             s_at.zone_index, (double)s_at.relay.ku, (double)s_at.relay.tu_s, (double)s_at.relay.amplitude_c,
             s_at.relay.cycles_used, (double)s_at.proposed_gains.kp, (double)s_at.proposed_gains.ki,
             (double)s_at.proposed_gains.kd);
}

/* One tick of the SETTLING/STEPPING/RELAY_APPROACH/RELAY_CYCLING state
 * machine, factored out of task_entry() so host tests can drive it
 * deterministically (see test_autotune_engine_prestart.c's STEPPING-loop
 * guard-coverage tests) without a real FreeRTOS task or vTaskDelay(). Caller
 * must already hold s_at.lock and have confirmed state_is_running(s_at.state)
 * -- this function never takes or gives the lock itself, matching
 * task_entry()'s existing discipline; every early-return below stands in for
 * that loop's "xSemaphoreGive(s_at.lock); continue;" pairs. */
static void autotune_engine_tick_locked(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t dt_ms = ticks_to_ms(now - s_at.prev_tick);
    if (dt_ms == 0) dt_ms = AUTOTUNE_ENGINE_TICK_MS;
    s_at.prev_tick = now;

    float raw_c = NAN;
    bool sensor_ok = false;
    /* TODO.md 6A.5(b): every channel's reading is captured this tick,
     * not just the zone under test -- MAX31856_read_all() already reads
     * the whole bus, so logging every zone's response is free (no extra
     * SPI traffic).
     *
     * ch_raw_c/ch_ok are indexed by physical MAX31856 channel, exactly
     * like profile_executor.c's identical split (TODO.md 10.8). Every
     * OTHER slot of raw_by_zone/ok_by_zone below still means what it
     * always has here -- "physical channel z's own reading", because the
     * coupling-matrix cross-fit in finalize_fit() is unchanged 6A.5(b)
     * scope and still assumes channel i == zone i for the peer zones.
     * Only index s_at.zone_index -- the zone actually under test, whose
     * baseline/trace/actual_c this tick's control math reads -- is
     * overwritten with thermo_combine()'s result across every channel
     * that zone's thermo_mask names, same pattern profile_executor.c's
     * control tick uses for every active zone. TODO.md 10.8 called this
     * file out by name as the one read path a previous pass left on the
     * legacy single-channel mapping. */
    float raw_by_zone[MAX31856_CHANNEL_COUNT];
    bool  ok_by_zone[MAX31856_CHANNEL_COUNT];
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        raw_by_zone[z] = NAN;
        ok_by_zone[z] = false;
    }
    if (sim_backend_enabled() || (s_at.thermo_bus && s_at.thermo_bus->initialized)) {
        float ch_raw_c[MAX31856_CHANNEL_COUNT];
        bool  ch_ok[MAX31856_CHANNEL_COUNT];
        for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
            ch_raw_c[z] = NAN;
            ch_ok[z] = false;
        }
        MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
        size_t count = 0;
        if (sim_backend_enabled()) {
            sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
        } else {
            MAX31856_read_all(s_at.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
        }
        for (size_t i = 0; i < count; i++) {
            uint8_t ch = readings[i].channel;
            if (ch >= MAX31856_CHANNEL_COUNT) continue;
            bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
            bool ok = !readings[i].spi_failed && !isnan(readings[i].tc_temperature_c) && !fault_bits_bad;
            ch_raw_c[ch] = readings[i].tc_temperature_c;
            ch_ok[ch] = ok;
        }
        /* Peer zones (finalize_fit()'s cross-gain rows): legacy
         * channel-equals-zone mapping, unchanged from before 10.8. */
        for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
            raw_by_zone[z] = ch_raw_c[z];
            ok_by_zone[z] = ch_ok[z];
        }
        /* The zone actually under test: its real thermo_mask, combined,
         * is what drives the step, the guards, and the fit. */
        uint8_t tmask = 0;
        zones_config_get_thermo_mask(s_at.zone_index, &tmask);
        bool combined_valid = false;
        float combined_c =
            thermo_combine(ch_raw_c, ch_ok, MAX31856_CHANNEL_COUNT, tmask, &combined_valid);
        raw_by_zone[s_at.zone_index] = combined_c;
        ok_by_zone[s_at.zone_index] = combined_valid;
        raw_c = combined_c;
        sensor_ok = combined_valid;
    }
    s_at.actual_valid = sensor_ok;
    s_at.actual_c = sensor_ok ? zones_config_apply_cal(s_at.zone_index, raw_c) : NAN;

    /* The relay law has to run in BOTH of its states: the approach is the
     * same law, just not yet recorded. Its edge report is consumed by the
     * phase logic further down, after the guards have had their say --
     * nothing about a cycle boundary may pre-empt a guard trip. */
    bool relay_edge = false;
    float want_duty;
    if (s_at.method == AUTOTUNE_METHOD_RELAY) {
        want_duty = relay_law_tick(sensor_ok, s_at.actual_c, &relay_edge);
    } else {
        want_duty = (s_at.state == AUTOTUNE_ENGINE_SETTLING) ? 0.0f : s_at.step_duty;
    }
    bool want_relay_on = heater_output_duty(&s_at.heater_state, &s_at.heater_cfg, sensor_ok ? want_duty : 0.0f, dt_ms);
    apply_relay(want_relay_on);
    s_at.duty = want_relay_on ? want_duty : 0.0f;

    thermal_guard_input_t gin = {
        .sensor_ok = sensor_ok,
        .measurement_c = raw_c,
        /* A relay run has a real setpoint, so guard 4 (drift after
         * settling) becomes a genuine check that the oscillation stayed
         * around the target instead of walking away from it -- the step
         * test has no setpoint and can only feed the guard its ceiling.
         * The comparison is raw-vs-calibrated by exactly the zone's
         * calibration offset, which is worth far less than guard 4's 25degC
         * band, and guards must keep seeing raw readings (TODO.md 6A.7:
         * a calibration offset may never hide a sensor from a guard).
         *
         * When no ceiling is configured (max_temp_c == 0, the default for a
         * zone the operator has never set one on -- readiness_http.c's
         * non-blocking "guard_max_temp" item), this used to fall back to
         * raw_c itself. That made setpoint_c == measurement_c on every tick,
         * pinning thermal_guard's `error` at exactly 0.0f for the whole run.
         * error > 0.0f is guard 1's (heating-failed / no-progress) branch
         * selector, so with error permanently at 0 guard 1 never ran at
         * all -- a dead element or a flat-but-plausible thermocouple could
         * duty-cycle for the full 4h budget undetected. Guard 2
         * (wrong-direction) isn't actually broken by this -- its own math
         * (thermal_guard.c) only ever reads real measurement deltas across
         * the progress window, never setpoint_c's magnitude -- but pinning
         * error at exactly 0 permanently steers every window into guard 2's
         * branch instead of guard 1's, so a stalled-but-not-yet-falling zone
         * (delta ~= 0, the common dead-element case) still passed guard 2's
         * "not falling faster than threshold" test with nothing to catch it
         * on the way through.
         *
         * Fix: fall back to raw_c + a fixed positive headroom instead of
         * bare raw_c. That keeps error strictly positive every tick, which
         * keeps every progress window on guard 1's branch -- the guard whose
         * job this actually is, since "delta < expected-rise" already
         * catches both a flat AND a falling reading, guard 2's narrower
         * "falling" case included. This was chosen over refusing to start a
         * step test without a configured ceiling (which would contradict
         * autotune_engine_run_relay()'s own comment that no-ceiling is
         * "survivable for a step test the operator watches climb" -- that
         * reasoning covers guard 5, not guards 1/2, but a step test's open
         * loop and short-ish default budget make an outright refusal more
         * restrictive than this bug warrants) and over silently disabling
         * guards 1/2 with a logged flag (unnecessary now that they are
         * genuinely covered). Guard 4 keeps behaving exactly as before in
         * this configuration -- see STEP_TEST_GUARD_HEADROOM_C's own comment. */
        .setpoint_c = (s_at.method == AUTOTUNE_METHOD_RELAY)
                          ? s_at.relay_setpoint_c
                          : (s_at.guard_cfg.max_temp_c > 0.0f ? s_at.guard_cfg.max_temp_c
                                                               : raw_c + STEP_TEST_GUARD_HEADROOM_C),
        .commanded_duty = want_relay_on ? want_duty : 0.0f,
        .dt_s = (float)dt_ms / 1000.0f,
    };
    if (thermal_guard_tick(&s_at.guard_state, &s_at.guard_cfg, &gin)) {
        escalate_and_abort(s_at.guard_state.reason, s_at.guard_state.detail);
        return;
    }

    s_at.elapsed_s = ticks_to_s(now - s_at.phase_start_tick);

    if (s_at.state == AUTOTUNE_ENGINE_SETTLING) {
        if (s_at.elapsed_s >= AUTOTUNE_ENGINE_SETTLE_S) {
            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                s_at.zone_baseline_valid[z] = ok_by_zone[z];
                s_at.zone_baseline_c[z] = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : 0.0f;
                s_at.zone_last_valid_c[z] = s_at.zone_baseline_c[z];
            }
            s_at.state = AUTOTUNE_ENGINE_STEPPING;
            s_at.phase_start_tick = now;
            s_at.last_sample_tick = now;
            s_at.trace_count = 0;
            heater_output_reset(&s_at.heater_state);
            ESP_LOGI(TAG, "autotune zone %u: settled at %.1fC, stepping duty to %.2f", s_at.zone_index,
                     (double)s_at.zone_baseline_c[s_at.zone_index], (double)s_at.step_duty);
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_STEPPING) {
        if (s_at.elapsed_s >= AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S) {
            finalize_fit(); /* attempt a fit on whatever we have; ABORTED if it doesn't fit */
            return;
        }
        if (ticks_to_s(now - s_at.last_sample_tick) >= AUTOTUNE_ENGINE_SAMPLE_PERIOD_S) {
            s_at.last_sample_tick = now;
            /* The shared index only advances on the tested zone's own valid
             * sample -- other zones carry forward zone_last_valid_c on a
             * momentary bad read of their own, so a single dropped reading
             * on a non-tested zone can't shift its trace out of alignment
             * with the tested zone's t_s. */
            if (s_at.actual_valid) {
                record_trace_sample(raw_by_zone, ok_by_zone);
            }

            if (s_at.trace_count >= MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK &&
                s_at.trace_count >= SETTLE_CHECK_SAMPLES) {
                float lo = 1e9f, hi = -1e9f;
                for (uint16_t i = s_at.trace_count - SETTLE_CHECK_SAMPLES; i < s_at.trace_count; i++) {
                    int16_t dc = s_at.zone_trace[s_at.zone_index][i];
                    if (dc == AUTOTUNE_TRACE_TEMP_INVALID) continue;
                    float v = (float)dc / 10.0f;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (hi - lo <= SETTLE_CHECK_BAND_C) {
                    finalize_fit();
                    return;
                }
            }
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_RELAY_APPROACH) {
        /* Nothing is recorded here. The kiln is climbing (or falling) to
         * the setpoint under the relay's high (or low) branch, and that
         * transit is a step response at best and a half-cycle of nothing
         * at worst -- feeding it to pid_autotune_fit_relay() would put a
         * long monotonic ramp in the head of the trace, which both wastes
         * the fixed-size buffer and drags the midline the crossing
         * detector slices cycles against away from the oscillation's own
         * centre. Recording starts at the first high->low edge instead:
         * the first moment the plant is provably at temperature and the
         * relay's phase is known. */
        if (s_at.elapsed_s >= AUTOTUNE_RELAY_APPROACH_MAX_S) {
            char msg[96];
            snprintf(msg, sizeof(msg), "did not reach %.0fC within %us -- setpoint out of reach",
                     (double)s_at.relay_setpoint_c, (unsigned)AUTOTUNE_RELAY_APPROACH_MAX_S);
            abort_locked(msg);
            ESP_LOGW(TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
            return;
        }
        if (relay_edge) {
            s_at.state = AUTOTUNE_ENGINE_RELAY_CYCLING;
            s_at.phase_start_tick = now;   /* the cycling budget is its own, not the approach's leftovers */
            s_at.last_sample_tick = now;
            s_at.trace_count = 0;
            s_at.relay_cycles_seen = 0;
            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                s_at.zone_last_valid_c[z] = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : NAN;
            }
            ESP_LOGI(TAG, "autotune zone %u: reached %.1fC, relay cycling around %.1fC (d=%.2f h=%.1fC)",
                     s_at.zone_index, (double)s_at.actual_c, (double)s_at.relay_setpoint_c,
                     (double)s_at.relay_d, (double)s_at.relay_h);
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_RELAY_CYCLING) {
        /* Edges are counted before the budget check so a run that
         * completes its last cycle on the same tick the budget expires is
         * treated as the success it is. */
        if (relay_edge && s_at.relay_cycles_seen < UINT16_MAX) {
            s_at.relay_cycles_seen++;
            ESP_LOGI(TAG, "autotune zone %u: relay cycle %u of %u complete", s_at.zone_index,
                     (unsigned)s_at.relay_cycles_seen, (unsigned)AUTOTUNE_RELAY_TARGET_CYCLES);
        }

        if (ticks_to_s(now - s_at.last_sample_tick) >= AUTOTUNE_ENGINE_SAMPLE_PERIOD_S) {
            s_at.last_sample_tick = now;
            /* Same rule as the step path: the shared index only advances on
             * a valid reading of the tested zone, so its row never contains
             * a hole. For a relay run that also protects Tu, since t_s is
             * implicit in the sample index -- a recorded gap would show up
             * as a shortened period rather than as missing data. A dropout
             * long enough to matter trips guard 6 and ends the run anyway. */
            if (s_at.actual_valid) {
                record_trace_sample(raw_by_zone, ok_by_zone);
            }
        }

        /* Enough cycles, or the trace buffer is full (which at 10s
         * sampling is the 4h budget by another name) -- fit. */
        if (s_at.relay_cycles_seen >= AUTOTUNE_RELAY_TARGET_CYCLES ||
            s_at.trace_count >= AUTOTUNE_ENGINE_MAX_SAMPLES ||
            s_at.elapsed_s >= AUTOTUNE_RELAY_CYCLE_MAX_S) {
            /* On budget expiry this still attempts the fit rather than
             * aborting outright, exactly as the step path does: the fit is
             * the thing that decides whether the data is usable, and
             * pid_autotune_fit_relay() refuses a trace that never settled
             * into a limit cycle. Either way finalize_relay_fit() leaves
             * the relays off -- DONE with a proposal, or ABORTED with the
             * fitter's own reason. */
            finalize_relay_fit();
            return;
        }
    }
}

static void task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AUTOTUNE_ENGINE_TICK_MS));

        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        if (!state_is_running(s_at.state)) {
            xSemaphoreGive(s_at.lock);
            continue;
        }

        autotune_engine_tick_locked();

        xSemaphoreGive(s_at.lock);
    }
}

esp_err_t autotune_engine_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                SafetyLinkClass *safety_or_null)
{
    memset(&s_at, 0, sizeof(s_at));
    s_at.io = io_or_null;
    s_at.thermo_bus = thermo_bus_or_null;
    s_at.safety = safety_or_null;
    s_at.state = AUTOTUNE_ENGINE_IDLE;

    s_at.lock = xSemaphoreCreateMutex();
    if (!s_at.lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Same priority as profile_executor's control task (5) -- the two never
     * run their control logic simultaneously (mutual exclusion in
     * autotune_engine_run()/profile_executor_run()), so there's no
     * starvation concern between them; both still sit below the link-loss
     * watchdog (6). */
    /* 2026-08-22: PSRAM stack -- audited against uart_bridge_ext.c's
     * cache-disable hazard (see that file's boot-time comment). task_entry()
     * never touches flash/NVS itself (autotune_engine_accept()'s zones_http
     * writes run on whichever task calls it, not this one) and reaches
     * hardware only through thermo_owner_task/kiln_io_owner_task's queues
     * (owner tasks keep their own internal stacks; the caller-owned result
     * struct they write into being in PSRAM is a plain memory store, not a
     * DMA target). Safe to move off internal SRAM, which several other
     * tasks are contending for during the WiFi-driver boot-time buffer
     * storm that same comment documents. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(task_entry, "autotune_engine", 4096, NULL, 5, &s_at.task,
                                                    tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_at.lock);
        s_at.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "autotune engine up -- step test (default) and relay-feedback methods, see autotune_engine.h "
                  "for scope; neither has ever produced a fit on real hardware");
    return ESP_OK;
}

/* Everything both methods must do before either can start: prove the zone is
 * ours to drive, prove nothing else is driving it, and arm the heater window
 * and the FULL guard suite from that zone's own configuration.
 *
 * The guard configuration is built identically for both methods, and that is
 * deliberate and load-bearing: a relay test drives real elements around a real
 * setpoint, so it is the run that most needs guards 1-8, not the one that can
 * afford a relaxed set. Nothing here consults `method`.
 *
 * Returns with s_at.lock HELD on success -- the caller then fills in the
 * method-specific fields and sets the initial state under the same lock, so
 * the tick task can never observe a half-configured run. Returns false with
 * the lock NOT held on failure. */
static bool begin_run_locked(uint8_t zone_index, char *err_msg, size_t err_cap)
{
    /* Recovery mode (boot_guard.h) deliberately skips autotune_engine_start()
     * -- but other code that DOES still run in that mode can still call into
     * this module's public API. s_at.lock is NULL until
     * autotune_engine_start() creates it, and taking a NULL FreeRTOS mutex
     * asserts (the exact failure profile_executor.c hit on the bench --
     * safety_poll_task -> ... -> xQueueSemaphoreTake() -> "assert failed:
     * (( pxQueue ))" -> panic; see that file for the full backtrace). Every
     * public entry point below tests it first and returns a clean
     * "not running" answer instead of touching s_at at all. This covers both
     * autotune_engine_run() and autotune_engine_run_relay(), which both
     * funnel through here before taking the lock themselves. */
    if (s_at.lock == NULL) {
        ESP_LOGW(TAG, "autotune begin_run() called before autotune_engine_start() -- refused");
        if (err_msg) snprintf(err_msg, err_cap, "autotune engine not started");
        return false;
    }

    /* Direction B of the mutual OTA interlock (see profile_executor_run()'s
     * identical check and ota_http.h's doc comment above
     * ota_http_heat_blocked_by_update()): refuse to start EITHER autotune
     * method while an update is in progress on either processor. Checked
     * first, before any zone-config/relay-mask state, for the same
     * "cheapest and orthogonal" reasoning ota_interlock_check() documents
     * for its own mutex check. */
    if (ota_http_heat_blocked_by_update(err_msg, err_cap)) {
        return false;
    }

    /* Refuse up front if heat is blocked at all -- most importantly a safety
     * link that is down or faulted. Every heat command this engine issues is
     * already gated (see apply_duty()'s relay_authority_zone_blocked() call),
     * so a run started this way was never DANGEROUS: the relays simply never
     * closed. It was dishonest, which is its own problem. Observed on the
     * bench: with no safety processor answering, POST /api/autotune/start
     * returned {"ok":true} and the engine sat in "settling" indefinitely,
     * heating nothing and explaining nothing, while the same condition makes
     * a manual relay-ON return a 403 that says exactly what is wrong and
     * makes a profile abort with "safety processor link silent". An autotune
     * that cannot heat should say so at the point the operator asks for it,
     * in the same words. */
    {
        uint32_t sources = 0;
        if (relay_authority_on_blocked(s_at.safety, &sources)) {
            if (err_msg) {
                /* Kept under 128 chars: dashboard_http.c's autotune handler
                 * passes a char[128], and the first draft of this message was
                 * truncated mid-word on the page ("...so it w"). */
                snprintf(err_msg, err_cap,
                         "heat is blocked (fault sources 0x%02X, usually the safety link down) -- "
                         "autotune cannot drive the element",
                         (unsigned)sources);
            }
            return false;
        }
    }

    /* TODO.md 8.2 "Tie it to the guards, not only the UI": same explicit
     * refusal as profile_executor_run() -- must not rely on the relay_mask
     * check below happening to read 0 for a failed-to-load config too. */
    if (!zones_config_is_valid()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone config failed to load or has not been saved -- autotune cannot run until "
                     "zone config loads cleanly (see /settings/zones)");
        }
        return false;
    }
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(zone_index, &mask) || mask == 0) {
        if (err_msg) snprintf(err_msg, err_cap, "zone has no relay mask configured");
        return false;
    }

    /* TODO.md 6A.5: per-zone, not "any profile anywhere" -- a profile
     * running on a *different* zone no longer blocks autotune here, now
     * that concurrent multi-zone execution exists. Zones must still be
     * autotuned one at a time relative to each OTHER autotune run (this
     * engine is a single global instance, see autotune_engine_run()'s own
     * SETTLING/STEPPING check below), and never on a zone a profile is
     * actively driving. */
    if (profile_executor_zone_is_active(zone_index)) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "a profile is running on zone %u -- autotune cannot run on it at "
                                       "the same time",
                     zone_index);
        }
        return false;
    }

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (state_is_running(s_at.state)) {
        xSemaphoreGive(s_at.lock);
        if (err_msg) snprintf(err_msg, err_cap, "autotune already running on zone %u", s_at.zone_index);
        return false;
    }

    s_at.zone_index = zone_index;
    s_at.trace_count = 0;
    s_at.abort_reason[0] = '\0';
    s_at.per_zone_blocked = false;

    /* TODO.md 6A.6: claim this zone's relays for the duration of the run, same
     * shape as profile_executor.c's RELAY_OWNER_PROFILE claim -- see
     * force_relays_off()'s matching release and relay_authority.h's
     * RELAY_OWNER_AUTOTUNE doc comment for why this exists. `mask` was already
     * validated non-zero above (before this function took the lock), so this
     * cannot silently claim nothing. */
    relay_authority_claim_mask(mask, RELAY_OWNER_AUTOTUNE);
    /* Both results are invalidated at the start of every run, whichever method
     * follows: exactly one of them will be filled in, and a stale `valid` from
     * the previous run would let autotune_engine_accept() write gains this run
     * never produced. */
    s_at.model.valid = false;
    s_at.relay.valid = false;
    s_at.relay_cycles_seen = 0;

    float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
    zones_config_get_heater_cfg(zone_index, &window_ms, &min_on_ms, &min_off_ms);
    s_at.heater_cfg = (heater_output_cfg_t){
        .window_ms = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS,
        .min_on_ms = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS,
        .min_off_ms = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS,
    };
    heater_output_reset(&s_at.heater_state);

    float max_temp_c = 0.0f, min_temp_c = -20.0f, sanity_rate = 0.0f;
    zones_config_get_temp_limits(zone_index, &max_temp_c, &min_temp_c);
    zones_config_get_sanity_rate(zone_index, &sanity_rate);
    /* TODO.md 6A.3's remaining named thresholds -- an autotune run arms the
     * full thermal_guard suite (this file's own doc note) exactly like a
     * firing does, so it must honour the same per-zone overrides a firing
     * would, not fall back to firmware-wide constants a running profile no
     * longer uses. Raw pass-through, same reasoning as profile_executor.c's
     * identical block: thermal_guard.c owns the 0->default substitution. */
    float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
    float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
    zones_config_get_guard_thresholds(zone_index, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                      &runaway_margin, &drift_period_s, &debounce_ticks, &frozen_window_s);
    s_at.guard_cfg = (thermal_guard_cfg_t){
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
    };
    thermal_guard_reset(&s_at.guard_state);

    TickType_t now = xTaskGetTickCount();
    s_at.phase_start_tick = now;
    s_at.prev_tick = now;
    /* Lock stays held -- caller sets method, method-specific fields, state. */
    return true;
}

bool autotune_engine_run(uint8_t zone_index, float step_duty, char *err_msg, size_t err_cap)
{
    if (!(step_duty > 0.0f) || step_duty > 1.0f) {
        if (err_msg) snprintf(err_msg, err_cap, "step_duty must be in (0, 1]");
        return false;
    }
    if (!begin_run_locked(zone_index, err_msg, err_cap)) {
        return false;
    }

    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.step_duty = step_duty;
    s_at.state = AUTOTUNE_ENGINE_SETTLING;
    bool no_ceiling = !(s_at.guard_cfg.max_temp_c > 0.0f);
    xSemaphoreGive(s_at.lock);

    ESP_LOGI(TAG, "autotune zone %u starting: settling %us at duty 0 before stepping to %.2f", zone_index,
             AUTOTUNE_ENGINE_SETTLE_S, (double)step_duty);
    if (no_ceiling) {
        /* Not a refusal (see the guard-fallback comment at this run's
         * thermal_guard_input_t construction) -- guards 1/2 stay fully
         * covered without a ceiling. Guard 4 (drift near the ceiling) does
         * not, though, and this is the one place that's ever surfaced: the
         * failure mode must be visible, not just silently absorbed the way
         * the old error==0 fallback used to hide it. */
        ESP_LOGW(TAG, "autotune zone %u: no max_temp_c configured -- guard 4 (drift near ceiling) has "
                      "nothing to compare against for this run; guards 1/2/5/6 are unaffected",
                 zone_index);
    }
    return true;
}

bool autotune_engine_run_relay(uint8_t zone_index, float setpoint_c, float relay_d, float hysteresis_c,
                               autotune_rule_t rule, char *err_msg, size_t err_cap)
{
    /* <= 0 means "use the default"; anything else is range-checked. The
     * defaults and their reasoning live in autotune_engine.h next to the
     * constants, not duplicated here. */
    float d = (relay_d > 0.0f) ? relay_d : AUTOTUNE_RELAY_DEFAULT_D;
    float h = (hysteresis_c > 0.0f) ? hysteresis_c : AUTOTUNE_RELAY_DEFAULT_H_C;

    if (d > AUTOTUNE_RELAY_MAX_D) {
        /* Not clamped, refused: silently shrinking d would leave the fit
         * dividing by a d the relay never actually drove, and Ku is directly
         * proportional to it. */
        if (err_msg) snprintf(err_msg, err_cap, "relay amplitude d must be in (0, %.2f]", (double)AUTOTUNE_RELAY_MAX_D);
        return false;
    }
    if (h < AUTOTUNE_RELAY_MIN_H_C || h > AUTOTUNE_RELAY_MAX_H_C) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "hysteresis h must be in [%.1f, %.1f] degC (half-width of the band)",
                     (double)AUTOTUNE_RELAY_MIN_H_C, (double)AUTOTUNE_RELAY_MAX_H_C);
        }
        return false;
    }
    if (rule != AUTOTUNE_RULE_ZIEGLER_NICHOLS && rule != AUTOTUNE_RULE_TYREUS_LUYBEN) {
        /* pid_autotune_tune_from_relay() returns all-zero gains for SIMC
         * rather than erroring, so this has to be caught here or the operator
         * would sit through a multi-hour oscillation to be handed kp=ki=kd=0. */
        if (err_msg) snprintf(err_msg, err_cap, "relay tuning rule must be Tyreus-Luyben or Ziegler-Nichols");
        return false;
    }

    /* The setpoint is checked against this zone's OWN guard limits before
     * anything is driven. A test whose oscillation is designed to sit where
     * guard 5 trips is not a test, it is a scheduled abort several hours from
     * now with the kiln at temperature in the meantime. */
    float max_temp_c = 0.0f, min_temp_c = -20.0f;
    zones_config_get_temp_limits(zone_index, &max_temp_c, &min_temp_c);
    if (!(max_temp_c > 0.0f)) {
        /* thermal_guard treats max_temp_c == 0 as "no ceiling". That is
         * survivable for a step test the operator watches climb; it is not
         * something to hand a deliberate oscillation at temperature. */
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u has no max temperature set -- set one before running a relay test", zone_index);
        }
        return false;
    }
    if (!(setpoint_c > 0.0f)) {
        if (err_msg) snprintf(err_msg, err_cap, "relay test requires an oscillation setpoint (degC)");
        return false;
    }
    if (setpoint_c > max_temp_c - AUTOTUNE_RELAY_SETPOINT_HEADROOM_C) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "setpoint must be at least %.0fC below this zone's %.0fC limit",
                     (double)AUTOTUNE_RELAY_SETPOINT_HEADROOM_C, (double)max_temp_c);
        }
        return false;
    }
    if (setpoint_c < min_temp_c + AUTOTUNE_RELAY_SETPOINT_HEADROOM_C) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "setpoint must be at least %.0fC above this zone's %.0fC floor",
                     (double)AUTOTUNE_RELAY_SETPOINT_HEADROOM_C, (double)min_temp_c);
        }
        return false;
    }

    if (!begin_run_locked(zone_index, err_msg, err_cap)) {
        return false;
    }

    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.step_duty = 0.0f; /* no step is ever applied on this path */
    s_at.relay_setpoint_c = setpoint_c;
    s_at.relay_d = d;
    s_at.relay_h = h;
    s_at.relay_rule = rule;
    /* Start on the high branch and let the law correct it on the first tick
     * that has a reading. Starting "on" is the right guess for the usual case
     * (a cold kiln heading up to the setpoint) and costs nothing in the other:
     * if the zone is already above the band the very next tick switches it
     * off, and the approach phase simply waits for the first full high->low
     * edge, which it would have had to do anyway. */
    s_at.relay_on = true;
    s_at.state = AUTOTUNE_ENGINE_RELAY_APPROACH;
    xSemaphoreGive(s_at.lock);

    ESP_LOGW(TAG,
             "autotune zone %u starting RELAY test: oscillating around %.1fC, duty %.2f/%.2f, band +/-%.1fC, "
             "%u cycles wanted -- this deliberately cycles the kiln at temperature",
             zone_index, (double)setpoint_c, (double)(AUTOTUNE_RELAY_CENTER_DUTY + d),
             (double)(AUTOTUNE_RELAY_CENTER_DUTY - d), (double)h, (unsigned)AUTOTUNE_RELAY_TARGET_CYCLES);
    return true;
}

void autotune_engine_abort(const char *reason)
{
    /* See begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        ESP_LOGW(TAG, "autotune_engine_abort() called before autotune_engine_start() -- refused");
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (!state_is_running(s_at.state)) {
        xSemaphoreGive(s_at.lock);
        return;
    }
    abort_locked(reason);
    xSemaphoreGive(s_at.lock);
    ESP_LOGI(TAG, "autotune zone %u aborted by request: %s", s_at.zone_index, s_at.abort_reason);
}

bool autotune_engine_accept(void)
{
    /* See begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        ESP_LOGW(TAG, "autotune_engine_accept() called before autotune_engine_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool have_result = (s_at.method == AUTOTUNE_METHOD_RELAY) ? s_at.relay.valid : s_at.model.valid;
    if (s_at.state != AUTOTUNE_ENGINE_DONE || !have_result) {
        xSemaphoreGive(s_at.lock);
        return false;
    }
    uint8_t zone = s_at.zone_index;
    autotune_method_t method = s_at.method;
    autotune_gains_t g = s_at.proposed_gains;
    fopdt_model_t m = s_at.model;
    xSemaphoreGive(s_at.lock);

    if (!zones_config_set_pid(zone, g.kp, g.ki, g.kd)) {
        return false;
    }

    if (method == AUTOTUNE_METHOD_RELAY) {
        /* A relay test measures ONE point of the frequency response: the gain
         * and period at which the loop's phase is -180 degrees. It does not
         * measure K, tau or L, and no combination of (Ku, Tu) recovers them --
         * infinitely many FOPDT plants share any given ultimate gain and
         * period. So there is nothing to write through zones_config_set_model()
         * here, and equally importantly nothing to OVERWRITE: if a previous
         * step test measured this zone's model, that model is still the best
         * (and only) thing 6A.2's feedforward and the ramp-ceiling estimate
         * have to work from. Clearing it, or synthesising one from Ku, would
         * trade a measurement for a guess and would do it silently.
         *
         * The gains above are the entire result of this run. */
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        clear_block_if_any();
        s_at.state = AUTOTUNE_ENGINE_IDLE;
        xSemaphoreGive(s_at.lock);
        ESP_LOGI(TAG, "autotune zone %u: relay-test gains accepted (no plant model written -- a relay test "
                      "measures none; any model from a previous step test is left untouched)", zone);
        return true;
    }
    /* The model goes with the gains, through the same owner and at the same
     * moment (TODO.md 6A.4: results are proposed, never auto-applied, and
     * zones_http stays the one owner of zone config -- this engine must not
     * touch NVS itself). 6A.2's feedforward is computed from K and tau, so
     * a run whose gains were accepted but whose model was dropped would
     * leave the loop tuned but unable to feed forward, which is the half of
     * the improvement that actually keeps a ramp on the curve.
     *
     * A failure here is logged, not propagated: the gains above are already
     * live and persisted, and returning false would tell the operator the
     * acceptance failed when the thing they clicked Accept for did in fact
     * land. The consequence of the missing model is a zone that runs on
     * feedback alone -- exactly how every zone ran before this existed --
     * and it is recoverable by re-running the test. Losing the accepted
     * gains to a false return would not be. */
    if (!zones_config_set_model(zone, m.k_gain_c_per_duty, m.tau_s, m.dead_time_s)) {
        ESP_LOGW(TAG,
                 "autotune zone %u: gains accepted but plant model (K=%.2f tau=%.1f L=%.1f) was rejected or "
                 "failed to persist -- feedforward will stay off for this zone",
                 zone, (double)m.k_gain_c_per_duty, (double)m.tau_s, (double)m.dead_time_s);
    }

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    clear_block_if_any();
    s_at.state = AUTOTUNE_ENGINE_IDLE;
    xSemaphoreGive(s_at.lock);
    ESP_LOGI(TAG, "autotune zone %u: gains accepted and written to zone config", zone);
    return true;
}

void autotune_engine_get_status(autotune_engine_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* See begin_run_locked()'s guard comment above. The zeroed struct above
     * already reads as a well-formed IDLE snapshot (AUTOTUNE_ENGINE_IDLE ==
     * 0), so a caller here needs nothing more than "don't touch the NULL
     * lock". */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_status() called before autotune_engine_start() -- reporting IDLE");
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    out->state = s_at.state;
    out->method = s_at.method;
    out->zone_index = s_at.zone_index;
    out->elapsed_s = s_at.elapsed_s;
    out->sample_count = s_at.trace_count;
    out->actual_c = s_at.actual_c;
    out->actual_valid = s_at.actual_valid;
    out->duty = s_at.duty;
    strncpy(out->abort_reason, s_at.abort_reason, sizeof(out->abort_reason) - 1);
    /* Relay parameters and cycle progress are reported in every state, not
     * just DONE: while cycling they are the only way for the operator to see
     * how much longer the kiln will be oscillating, and after an abort they
     * are what makes the abort reason interpretable. */
    out->relay_setpoint_c = s_at.relay_setpoint_c;
    out->relay_amplitude_duty = s_at.relay_d;
    out->relay_hysteresis_c = s_at.relay_h;
    out->relay_cycles_seen = s_at.relay_cycles_seen;
    out->relay_cycles_target = AUTOTUNE_RELAY_TARGET_CYCLES;
    if (s_at.state == AUTOTUNE_ENGINE_DONE) {
        out->model = s_at.model;
        out->relay = s_at.relay;
        out->proposed_gains = s_at.proposed_gains;
        out->predicted_max_ramp_c_per_hr = s_at.predicted_max_ramp_c_per_hr;
    }
    xSemaphoreGive(s_at.lock);
}

size_t autotune_engine_get_trace(autotune_sample_t *out, size_t start_index, size_t max_entries)
{
    if (!out || max_entries == 0) return 0;
    /* See begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_trace() called before autotune_engine_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (start_index >= s_at.trace_count) {
        xSemaphoreGive(s_at.lock);
        return 0;
    }
    size_t count = (size_t)s_at.trace_count - start_index;
    if (count > max_entries) count = max_entries;
    /* Trace is written oldest-first into a flat (non-ring) array during
     * STEPPING -- no modular indexing needed, unlike the history buffer.
     * This is the zone under test's own row; the other zones' rows (TODO.md
     * 6A.5(b)) aren't downloadable as CSV in this pass, only their fitted
     * models via autotune_engine_get_coupling_matrix(). */
    for (size_t i = 0; i < count; i++) {
        size_t idx = start_index + i;
        int16_t dc = s_at.zone_trace[s_at.zone_index][idx];
        out[i].t_s = (float)(idx * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        out[i].measurement_c = (dc == AUTOTUNE_TRACE_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
    }
    xSemaphoreGive(s_at.lock);
    return count;
}

void autotune_engine_get_coupling_matrix(autotune_coupling_matrix_t *out)
{
    if (!out) return;
    /* See begin_run_locked()'s guard comment above. Zeroing here matches
     * every cell's own "unmeasured" representation (autotune_coupling_cell_t
     * ::valid == false), same as a never-run engine's real coupling matrix. */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_coupling_matrix() called before autotune_engine_start() -- reporting empty");
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    *out = s_at.coupling;
    xSemaphoreGive(s_at.lock);
}

void autotune_engine_compute_rga(const autotune_coupling_matrix_t *m, autotune_rga_t *out)
{
    if (!out) return;
    if (!m) {
        memset(out, 0, sizeof(*out));
        return;
    }

    /* Only the steady-state gain enters the RGA. tau and L are deliberately
     * discarded here: the RGA is a *steady-state* interaction measure, and
     * the dynamic version (the RGA evaluated at a frequency rather than at
     * DC) needs a full transfer-function model per pair, which nothing in
     * this firmware identifies. Mixing the two would be an upgrade in
     * apparent rigour and a downgrade in correctness. */
    float k[MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT];
    bool k_valid[MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT];
    for (int i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            const autotune_coupling_cell_t *c = &m->cell[i][j];
            /* A cell is usable only if the *fit* converged too -- cell.valid
             * alone can be true for a row that was written with a model the
             * fitter rejected, and an unfitted gain is a hole, not a zero. */
            bool ok = c->valid && c->model.valid;
            k[i * MAX31856_CHANNEL_COUNT + j] = ok ? c->model.k_gain_c_per_duty : 0.0f;
            k_valid[i * MAX31856_CHANNEL_COUNT + j] = ok;
        }
    }
    *out = pid_autotune_rga(k, k_valid, MAX31856_CHANNEL_COUNT);
}

bool autotune_engine_is_active(void)
{
    /* See begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool active = state_is_running(s_at.state);
    xSemaphoreGive(s_at.lock);
    return active;
}

bool autotune_engine_is_active_on_zone(uint8_t zone_index)
{
    /* See begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool active = state_is_running(s_at.state) && s_at.zone_index == zone_index;
    xSemaphoreGive(s_at.lock);
    return active;
}
