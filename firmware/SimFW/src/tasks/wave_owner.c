// wave_owner.c -- see wave_owner.h for the public API and design summary.
// Real body: docs/DESIGN_NOTES.md section 3.3's synthesis detail, milestone M-D
// (section 10), implemented against src/drivers/ct_wave_pwm.h (the PWM+DMA
// driver, this task's private peripheral) and src/sim/sine_synth.h (the
// pure waveform math, reused verbatim -- not reimplemented here) plus
// src/sim/sim_snapshot.h (the amplitude source in MODEL mode).
#include "wave_owner.h"

#include <math.h>
#include <string.h>

#include "pico/time.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/ct_wave_pwm.h"
#include "sim/ct_calibration.h"
#include "sim/sim_snapshot.h"
#include "sim/sine_synth.h"

// ct_calibration.h deliberately does not include this header (src/sim/ must
// stay RTOS/SDK-free, tools/check_sim_purity.ps1), so the two channel counts
// are asserted equal here, at the one place both are visible.
_Static_assert(CT_CAL_NUM_CHANNELS == CT_WAVE_NUM_CHANNELS,
               "ct_calibration.h's CT_CAL_NUM_CHANNELS must match wave_owner.h's CT_WAVE_NUM_CHANNELS");

#define WAVE_OWNER_STACK_WORDS  (configMINIMAL_STACK_SIZE * 2u) // headroom for 3x 256-entry uint16_t table scratch buffers
#define WAVE_OWNER_TICK_MS      10u  // 100 Hz command/model-poll rate; DMA (not this loop) is what actually feeds samples in steady state

// Nominal synthesis sample rate per docs/DESIGN_NOTES.md 3.3: 256 entries x 60 Hz.
// The real hardware pacer (ct_wave_pwm.c) lands at ~15,361.19 Hz, not this
// exact value, for the integer-wrap-register reasons documented in that
// file's header comment (~0.0077% off) -- irrelevant at the software-model
// level this file works at, so the table is always computed against the
// clean nominal rate below.
#define CT_WAVE_NOMINAL_FREQ_HZ    60.0f
#define CT_WAVE_NOMINAL_SAMPLE_HZ  ((float)SINE_SYNTH_TABLE_LEN * CT_WAVE_NOMINAL_FREQ_HZ) // 15,360 Hz

#define CT_WAVE_PWM_LEVEL_MAX 255u  // must match ct_wave_pwm.c's CT_WAVE_CARRIER_WRAP (8-bit resolution)
#define CT_WAVE_PWM_LEVEL_MID (0.5f * (float)CT_WAVE_PWM_LEVEL_MAX)

// --- Command queue (the only path any other task has to make wave_owner
// touch its PWM/DMA state -- mirrors i2c_owner.c's pattern) -----------------
typedef enum {
    WAVE_OWNER_CMD_SET_MODE,
    WAVE_OWNER_CMD_SET_AMPS,
    WAVE_OWNER_CMD_SET_PHASE,
    WAVE_OWNER_CMD_SET_DISTORTION,
} wave_owner_cmd_type_t;

typedef struct {
    wave_owner_cmd_type_t type;
    uint8_t channel;
    union {
        struct { ct_wave_mode_t mode; } set_mode;
        struct { float amps; } set_amps;
        struct { float phase_deg; } set_phase;
        struct { ct_wave_distortion_t distortion; } set_distortion;
    } u;
} wave_owner_cmd_t;

#define WAVE_OWNER_CMD_QUEUE_DEPTH 16u

static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

// Per-channel commanded configuration -- single writer: this task's own loop
// (applying drained commands), guarded by s_state_mutex since
// ct_wave_get_state() reads it from any calling task.
static ct_wave_channel_state_t s_channel_state[CT_WAVE_NUM_CHANNELS];

// Last configuration actually pushed to ct_wave_pwm for each channel, so the
// task only recomputes/reloads a 256-entry table when something actually
// changed (steady state should cost ~nothing on core 1, matching DESIGN_NOTES.md
// 4.1's "hard-real-time producers" framing -- the DMA does the steady-state
// work, this loop is not meant to burn cycles every 10 ms recomputing an
// unchanged waveform).
typedef struct {
    bool valid;
    uint8_t channel;  // which channel these constants/this config belong to -- the
                       // calibration lookup is per channel, so build_table() must know it
    ct_wave_mode_t mode;
    float amps;
    float phase_deg;
    ct_wave_distortion_t distortion;
} wave_owner_applied_cfg_t;

static wave_owner_applied_cfg_t s_applied_cfg[CT_WAVE_NUM_CHANNELS];

// Software-side zero-crossing tracking, one reference phase per channel, so
// a config change that lands just after a real zero crossing can be pushed
// immediately instead of waiting up to one full ~16.7 ms cycle for
// ct_wave_pwm's own DMA-completion-boundary gate (DESIGN_NOTES.md 3.3: "apply ...
// at zero crossings only"). This is a latency optimization layered on top
// of the driver's own always-correct hardware-level gate (ct_wave_pwm.h's
// top comment) -- if this software detection misses a crossing between two
// 10 ms ticks, the driver's boundary gate still catches it at worst one
// cycle later, so correctness never depends on this task's own timing.
// sine_synth_zero_crossing() (sim/sine_synth.h) is exactly the detector its
// own header comment says a caller like this one should use.
static float s_ref_prev_raw[CT_WAVE_NUM_CHANNELS];
static bool s_ref_prev_valid[CT_WAVE_NUM_CHANNELS];

static float s_sine_table[SINE_SYNTH_TABLE_LEN];

static void state_lock(void) { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_state_mutex); }

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float ct_wave_amps_to_pwm_scale(uint8_t channel, float amps)
{
    // Per-channel clamp(gain*amps + offset, 0, 1), from the compiled-in
    // calibration table (sim/ct_calibration.h). That table is all-
    // UNCALIBRATED today -- no CT hardware exists and no calibration run has
    // ever been taken -- so every channel still resolves to exact identity,
    // clamp(amps, 0, 1), the same behavior this function has always had. The
    // arithmetic lives in src/sim/ so it is host-testable; the table is
    // regenerated from the PC-side runner's JSON by
    // tools/gen_ct_cal_table.py (see ct_calibration.h).
    return ct_cal_apply(ct_cal_default_table(), channel, amps);
}

// Builds one channel's 256-entry PWM duty-level table from its current
// applied config, reusing sine_synth.h's sample function verbatim (per this
// pass's constraint: sine_synth.c/.h are not reimplemented, only called).
static void build_table(const wave_owner_applied_cfg_t *cfg, uint16_t out_levels[SINE_SYNTH_TABLE_LEN])
{
    sine_channel_cfg_t sc = {
        .amplitude = ct_wave_amps_to_pwm_scale(cfg->channel, cfg->amps),
        .phase_deg = cfg->phase_deg,
        .dc_offset = cfg->distortion.dc_offset,
        .clip_fraction = cfg->distortion.clip_fraction,
        .dropout_half_cycle = cfg->distortion.dropout_half_cycle,
        .dropout_negative_half = cfg->distortion.dropout_negative_half,
    };

    for (uint32_t i = 0; i < SINE_SYNTH_TABLE_LEN; i++) {
        float t_s = (float)i / CT_WAVE_NOMINAL_SAMPLE_HZ;
        float sample = sine_synth_sample(&sc, s_sine_table, CT_WAVE_NOMINAL_FREQ_HZ, t_s);
        sample = clampf(sample, -1.0f, 1.0f);

        float level_f = CT_WAVE_PWM_LEVEL_MID + CT_WAVE_PWM_LEVEL_MID * sample;
        long level = lroundf(level_f);
        if (level < 0) level = 0;
        if (level > (long)CT_WAVE_PWM_LEVEL_MAX) level = (long)CT_WAVE_PWM_LEVEL_MAX;
        out_levels[i] = (uint16_t)level;
    }
}

static bool applied_cfg_equal(const wave_owner_applied_cfg_t *a, const wave_owner_applied_cfg_t *b)
{
    return a->valid == b->valid &&
           a->channel == b->channel &&
           a->mode == b->mode &&
           a->amps == b->amps &&
           a->phase_deg == b->phase_deg &&
           memcmp(&a->distortion, &b->distortion, sizeof(a->distortion)) == 0;
}

static void apply_pending_commands(void)
{
    wave_owner_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd.channel >= CT_WAVE_NUM_CHANNELS) {
            continue;
        }
        state_lock();
        ct_wave_channel_state_t *st = &s_channel_state[cmd.channel];
        switch (cmd.type) {
        case WAVE_OWNER_CMD_SET_MODE:
            st->mode = cmd.u.set_mode.mode;
            break;
        case WAVE_OWNER_CMD_SET_AMPS:
            st->amps = cmd.u.set_amps.amps;
            break;
        case WAVE_OWNER_CMD_SET_PHASE:
            st->phase_deg = cmd.u.set_phase.phase_deg;
            break;
        case WAVE_OWNER_CMD_SET_DISTORTION:
            st->distortion = cmd.u.set_distortion.distortion;
            break;
        }
        state_unlock();
    }
}

// One tick: for each channel, resolve its current target (MODEL from the
// sim snapshot, or MANUAL from the last commanded amps), and if the
// resulting config actually differs from what was last pushed to
// ct_wave_pwm, rebuild and push its table -- gated at a zero crossing
// (immediately if one was just detected in software, or via ct_wave_pwm's
// own DMA-boundary gate otherwise) unless the channel's own distortion
// config asks for an immediate mid-cycle step.
static void wave_owner_tick(uint64_t now_us)
{
    sim_snapshot_t snap;
    bool have_snap = sim_snapshot_read(&snap);

    for (uint8_t ch = 0; ch < CT_WAVE_NUM_CHANNELS; ch++) {
        state_lock();
        ct_wave_mode_t mode = s_channel_state[ch].mode;
        float manual_amps = s_channel_state[ch].amps;
        float phase_deg = s_channel_state[ch].phase_deg;
        ct_wave_distortion_t distortion = s_channel_state[ch].distortion;
        state_unlock();

        float amps;
        if (mode == CT_WAVE_MODE_MANUAL) {
            amps = manual_amps;
        } else if (have_snap && ch < snap.zone_count) {
            amps = snap.zones[ch].current_a;
        } else {
            amps = 0.0f; // no snapshot yet, or this channel has no backing zone this run
        }

        wave_owner_applied_cfg_t new_cfg = {
            .valid = true,
            .channel = ch,
            .mode = mode,
            .amps = amps,
            .phase_deg = phase_deg,
            .distortion = distortion,
        };

        // Software zero-crossing tracking for the low-latency path (see
        // s_ref_prev_raw's comment above) -- runs every tick regardless of
        // whether a change is pending, so the reference phase never falls
        // behind.
        float t_s = (float)now_us / 1.0e6f;
        float curr_raw = sine_synth_raw(s_sine_table, CT_WAVE_NOMINAL_FREQ_HZ, phase_deg, t_s);
        bool crossed_since_last_tick = false;
        if (s_ref_prev_valid[ch]) {
            bool rising;
            crossed_since_last_tick = sine_synth_zero_crossing(s_ref_prev_raw[ch], curr_raw, &rising);
            (void)rising;
        }
        s_ref_prev_raw[ch] = curr_raw;
        s_ref_prev_valid[ch] = true;

        if (!applied_cfg_equal(&new_cfg, &s_applied_cfg[ch])) {
            uint16_t levels[SINE_SYNTH_TABLE_LEN];
            build_table(&new_cfg, levels);

            bool apply_now = distortion.apply_immediately || crossed_since_last_tick;
            ct_wave_pwm_load_table(ch, levels, apply_now);

            s_applied_cfg[ch] = new_cfg;

            state_lock();
            s_channel_state[ch].last_pwm_scale = ct_wave_amps_to_pwm_scale(ch, amps);
            s_channel_state[ch].valid = true;
            state_unlock();
        }
    }
}

static void wave_owner_task_fn(void *arg)
{
    (void)arg;

    sine_synth_init_table(s_sine_table);

    for (;;) {
        apply_pending_commands();
        wave_owner_tick(time_us_64());
        vTaskDelay(pdMS_TO_TICKS(WAVE_OWNER_TICK_MS));
    }
}

bool wave_owner_start(void)
{
    s_cmd_queue = xQueueCreate(WAVE_OWNER_CMD_QUEUE_DEPTH, sizeof(wave_owner_cmd_t));
    if (s_cmd_queue == NULL) {
        return false;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return false;
    }

    memset(s_channel_state, 0, sizeof(s_channel_state));
    memset(s_applied_cfg, 0, sizeof(s_applied_cfg));
    memset(s_ref_prev_valid, 0, sizeof(s_ref_prev_valid));
    for (uint8_t ch = 0; ch < CT_WAVE_NUM_CHANNELS; ch++) {
        s_channel_state[ch].mode = CT_WAVE_MODE_MODEL;
    }

    if (!ct_wave_pwm_init()) {
        return false;
    }

    BaseType_t ok = xTaskCreate(wave_owner_task_fn, "wave_owner", WAVE_OWNER_STACK_WORDS, NULL,
                                 SIMFW_PRIO_WAVE_OWNER, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_RT_PATH);
    return true;
}

bool ct_wave_set_mode(uint8_t channel, ct_wave_mode_t mode)
{
    if (!s_cmd_queue || channel >= CT_WAVE_NUM_CHANNELS) {
        return false;
    }
    wave_owner_cmd_t cmd = { .type = WAVE_OWNER_CMD_SET_MODE, .channel = channel, .u.set_mode = { .mode = mode } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool ct_wave_set_amps(uint8_t channel, float amps)
{
    if (!s_cmd_queue || channel >= CT_WAVE_NUM_CHANNELS) {
        return false;
    }
    wave_owner_cmd_t cmd = { .type = WAVE_OWNER_CMD_SET_AMPS, .channel = channel, .u.set_amps = { .amps = amps } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool ct_wave_set_phase(uint8_t channel, float phase_deg)
{
    if (!s_cmd_queue || channel >= CT_WAVE_NUM_CHANNELS) {
        return false;
    }
    wave_owner_cmd_t cmd = { .type = WAVE_OWNER_CMD_SET_PHASE, .channel = channel, .u.set_phase = { .phase_deg = phase_deg } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool ct_wave_set_distortion(uint8_t channel, const ct_wave_distortion_t *distortion)
{
    if (!s_cmd_queue || channel >= CT_WAVE_NUM_CHANNELS || !distortion) {
        return false;
    }
    wave_owner_cmd_t cmd = { .type = WAVE_OWNER_CMD_SET_DISTORTION, .channel = channel,
                              .u.set_distortion = { .distortion = *distortion } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool ct_wave_get_state(uint8_t channel, ct_wave_channel_state_t *out)
{
    if (channel >= CT_WAVE_NUM_CHANNELS || !out) {
        return false;
    }
    state_lock();
    *out = s_channel_state[channel];
    state_unlock();
    return true;
}
