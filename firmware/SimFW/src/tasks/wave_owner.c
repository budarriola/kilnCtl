// wave_owner.c -- see wave_owner.h for the public API and design summary.
// Real body: docs/DESIGN_NOTES.md section 3.3's synthesis detail, driving
// src/drivers/ct_wave_i2s.h (the PIO+DMA I2S MASTER transport, this task's
// private peripheral) through src/sim/ct_i2s_gen.h (the pure, host-testable
// per-sample generator -- not reimplemented here), which itself calls into
// src/sim/sine_synth.h and src/sim/ct_calibration.h.
//
// Replaces the retired PWM+RC path (formerly src/drivers/ct_wave_pwm.{c,h},
// deleted 2026-08-23 per docs/DESIGN_NOTES.md section 3.3's PWM->I2S
// decision). The old file's DMA/pacer hardware generated samples entirely on
// its own once a table was loaded; this task's own loop is now the ONLY
// thing that ever produces a sample (ct_i2s_gen_fill_block(), called from
// this file's ct_wave_i2s_refill_fn implementation below), so this task's
// tick period is now load-bearing for audio continuity, not just for
// command/model-poll latency -- see WAVE_OWNER_TICK_MS's comment.
#include "wave_owner.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/ct_wave_i2s.h"
#include "drivers/simfw_fatal.h"
#include "sim/ct_calibration.h"
#include "sim/ct_i2s_gen.h"
#include "sim/sim_snapshot.h"

// ct_i2s_gen.h/ct_calibration.h deliberately do not include this header
// (src/sim/ must stay RTOS/SDK-free, tools/check_sim_purity.ps1), so the
// three channel counts are asserted equal here, at the one place all three
// are visible.
_Static_assert(CT_CAL_NUM_CHANNELS == CT_WAVE_NUM_CHANNELS,
               "ct_calibration.h's CT_CAL_NUM_CHANNELS must match wave_owner.h's CT_WAVE_NUM_CHANNELS");
_Static_assert(CT_I2S_GEN_NUM_CHANNELS == CT_WAVE_NUM_CHANNELS,
               "ct_i2s_gen.h's CT_I2S_GEN_NUM_CHANNELS must match wave_owner.h's CT_WAVE_NUM_CHANNELS");

#define WAVE_OWNER_STACK_WORDS  (configMINIMAL_STACK_SIZE * 2u) // headroom for generate_joint_block()'s two per-module scratch buffers

// Poll/apply-commands period. ct_wave_i2s.h's SEAM CHOICE comment requires
// ct_wave_i2s_poll() to run at least once per CT_WAVE_I2S_FRAMES_PER_BUFFER
// worth of playback time (128 frames / 16,000 Hz = 8 ms), WITH MARGIN, or a
// module's PIO state machine stalls waiting on its TX FIFO's autopull. 3 ms
// leaves better than 2x margin under that 8 ms figure. This is a real
// behavior change from the PWM-era file: there, the DMA/pacer hardware fed
// samples on its own once a table was loaded, and this task's tick only
// needed to keep up with COMMAND changes (10 ms was plenty). Now this task's
// own loop is the sole sample source, so its period is an audio-continuity
// deadline, not just a UI-latency one.
#define WAVE_OWNER_TICK_MS      3u

// Depth of the small per-module block queue below -- see its header comment
// for why it exists. 4 is generous headroom over the 2 the current
// call-order analysis requires; cheap to keep since a queued block is only
// CT_WAVE_I2S_SAMPLES_PER_BUFFER int16s (512 bytes).
#define CT_WAVE_BLOCK_QUEUE_DEPTH 4u

// --- Command queue (unchanged shape/doctrine from the PWM-era file -- the
// only path any other task has to make wave_owner touch its state, mirrors
// i2c_owner.c's pattern) -----------------------------------------------
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

// Last configuration actually staged into ct_i2s_gen for each channel, so
// this task only calls ct_i2s_gen_stage_config() when something actually
// changed -- mirrors the PWM-era file's wave_owner_applied_cfg_t/
// applied_cfg_equal() pattern. Unlike that file, there is no software
// zero-crossing pre-check here: ct_i2s_gen_stage_config()'s own gate runs
// at EVERY sample (ct_i2s_gen.h's header explains why 16 kHz needs an
// explicit per-sample gate, unlike the PWM path's once-per-256-sample DMA
// boundary), so there is no coarser boundary left for a software shortcut
// to usefully anticipate.
typedef struct {
    bool valid;
    ct_wave_mode_t mode;
    float amps;
    float phase_deg;
    ct_wave_distortion_t distortion;
} wave_owner_staged_cfg_t;

static wave_owner_staged_cfg_t s_last_staged[CT_WAVE_NUM_CHANNELS];

// --- CT sample generation context and per-module block queue ---------------
//
// ct_i2s_gen_fill_block() produces module A's and module B's samples
// TOGETHER, from one shared running time cursor (ct_i2s_gen.h: "no DDS/phase
// accumulator", just an absolute time_s). ct_wave_i2s's refill callback,
// though, asks for ONE module's samples per call, and its own prefill
// sequence (ct_wave_i2s_init()) calls module A twice (buf0, buf1) before
// module B gets any -- so naively calling ct_i2s_gen_fill_block() once per
// refill would either desync the two modules' time cursors (each pulling
// from a different point in time) or regenerate/skip a time window
// depending on call order.
//
// The fix: every refill request pulls from a small per-module FIFO of
// already-generated blocks. When the requested module's FIFO is empty, this
// file generates exactly ONE joint block (advancing the shared cursor
// exactly once) and pushes it into BOTH modules' FIFOs, then pops the
// requested one. This keeps module A's and module B's samples aligned to
// the same time windows no matter what order/pairing the driver happens to
// call refill in -- verified against ct_wave_i2s_init()'s actual call order
// (A-buf0, A-buf1, B-buf0, B-buf1): the two calls for A each generate a
// fresh joint block (queueing one spare block for B each time), and B's two
// calls then drain that queue in the same order the blocks were produced,
// FIFO.
typedef struct {
    int16_t frames[CT_WAVE_I2S_SAMPLES_PER_BUFFER];
} wave_owner_block_t;

static ct_i2s_gen_ctx_t s_gen_ctx;
static wave_owner_block_t s_queue[CT_WAVE_I2S_NUM_MODULES][CT_WAVE_BLOCK_QUEUE_DEPTH];
static uint8_t s_queue_head[CT_WAVE_I2S_NUM_MODULES];
static uint8_t s_queue_count[CT_WAVE_I2S_NUM_MODULES];

static void queue_push(uint8_t module, const int16_t *frames)
{
    if (s_queue_count[module] >= CT_WAVE_BLOCK_QUEUE_DEPTH) {
        // Should not happen -- see this section's header comment; both
        // modules drain at the same hardware rate, started in lock-step via
        // pio_enable_sm_mask_in_sync() (ct_wave_i2s.c). If it ever does,
        // drop the OLDEST queued block rather than overflow the array: an
        // audible glitch on this bench-only fixture is a vastly better
        // failure mode than corrupting this queue.
        s_queue_head[module] = (uint8_t)((s_queue_head[module] + 1u) % CT_WAVE_BLOCK_QUEUE_DEPTH);
        s_queue_count[module]--;
    }
    uint8_t idx = (uint8_t)((s_queue_head[module] + s_queue_count[module]) % CT_WAVE_BLOCK_QUEUE_DEPTH);
    memcpy(s_queue[module][idx].frames, frames, sizeof(s_queue[module][idx].frames));
    s_queue_count[module]++;
}

static void queue_pop(uint8_t module, int16_t *out)
{
    memcpy(out, s_queue[module][s_queue_head[module]].frames, sizeof(s_queue[module][0].frames));
    s_queue_head[module] = (uint8_t)((s_queue_head[module] + 1u) % CT_WAVE_BLOCK_QUEUE_DEPTH);
    s_queue_count[module]--;
}

// Advances s_gen_ctx by exactly one CT_WAVE_I2S_FRAMES_PER_BUFFER-frame
// block and pushes the result into both modules' queues -- see this
// section's header comment for why this must always generate for BOTH
// modules together, never for just the one that happened to run dry.
static void generate_joint_block(void)
{
    int16_t block_a[CT_WAVE_I2S_SAMPLES_PER_BUFFER];
    int16_t block_b[CT_WAVE_I2S_SAMPLES_PER_BUFFER];
    ct_i2s_gen_fill_block(&s_gen_ctx, block_a, block_b, CT_WAVE_I2S_FRAMES_PER_BUFFER);
    queue_push(0, block_a);
    queue_push(1, block_b);
}

// ct_wave_i2s_refill_fn implementation -- see ct_wave_i2s.h's contract:
// called only from ct_wave_i2s_poll()'s own task context (this task is the
// only caller, from wave_owner_task_fn() below), never from an ISR, so it
// is free to do ordinary task-context work. It does not need to take
// s_state_mutex: s_gen_ctx/s_queue/s_queue_head/s_queue_count are all
// private to this one task.
static void wave_owner_i2s_refill(uint8_t module, int16_t *out, uint32_t frame_count, void *user_ctx)
{
    (void)user_ctx;
    if (module >= CT_WAVE_I2S_NUM_MODULES || frame_count != CT_WAVE_I2S_FRAMES_PER_BUFFER) {
        // Contract violation by the caller (ct_wave_i2s.c always passes
        // CT_WAVE_I2S_FRAMES_PER_BUFFER for a valid module today), not a
        // runtime condition this function can do anything sensible about --
        // there is no well-defined block to hand back. Fill with silence
        // rather than leaving `out` uninitialised.
        if (out != NULL && frame_count <= CT_WAVE_I2S_FRAMES_PER_BUFFER) {
            memset(out, 0, (size_t)frame_count * 2u * sizeof(int16_t));
        }
        return;
    }

    while (s_queue_count[module] == 0) {
        generate_joint_block();
    }
    queue_pop(module, out);
}

static void state_lock(void) { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_state_mutex); }

float ct_wave_amps_to_pwm_scale(uint8_t channel, float amps)
{
    // NAME IS NOW MISLEADING, kept for source compatibility. Before
    // docs/DESIGN_NOTES.md section 3.3's 2026-08-23 PWM->I2S decision, this
    // returned a PWM duty-cycle fraction; the PWM+RC backend is gone
    // (formerly src/drivers/ct_wave_pwm.{c,h}) and this now returns a DAC
    // FULL-SCALE AMPLITUDE fraction (0..1, "how much of the UDA1334A's
    // output swing this channel uses"), not a duty cycle. The arithmetic
    // itself is unchanged: per-channel clamp(gain*amps + offset, 0, 1) from
    // the compiled-in calibration table (sim/ct_calibration.h). Kept under
    // its old name only because cmd_task.c and other callers already depend
    // on this symbol; see this task's report for a rename recommendation
    // (e.g. ct_wave_amps_to_dac_scale()) for whoever next touches this API.
    //
    // ct_i2s_gen.c calls this exact same mapping again, internally, every
    // sample (ct_cal_apply() against the same default table) when it turns
    // a channel's `amps` into an actual DAC sample -- this function is not
    // in that data path. It stays exported for ct_wave_get_state()'s
    // last_pwm_scale reporting field and for any external caller that wants
    // the mapping without generating a sample. That table is all-
    // UNCALIBRATED today -- no CT hardware exists and no calibration run has
    // ever been taken -- so every channel still resolves to exact identity,
    // clamp(amps, 0, 1).
    return ct_cal_apply(ct_cal_default_table(), channel, amps);
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

static bool staged_cfg_equal(const wave_owner_staged_cfg_t *a, const wave_owner_staged_cfg_t *b)
{
    return a->valid == b->valid &&
           a->mode == b->mode &&
           a->amps == b->amps &&
           a->phase_deg == b->phase_deg &&
           memcmp(&a->distortion, &b->distortion, sizeof(a->distortion)) == 0;
}

// One tick: for each channel, resolve its current target (MODEL from the
// sim snapshot, or MANUAL from the last commanded amps), and if the
// resulting config actually differs from what was last staged, stage it
// into ct_i2s_gen -- which does its own zero-crossing-gated apply (or
// immediate step, per distortion.apply_immediately) at the sample level,
// every sample, per ct_i2s_gen.h's contract. Unlike the PWM-era file, there
// is no separate software zero-crossing pre-check here: that existed only to
// anticipate a coarser 256-sample DMA-boundary gate, which no longer exists.
static void wave_owner_tick(void)
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

        wave_owner_staged_cfg_t new_cfg = {
            .valid = true,
            .mode = mode,
            .amps = amps,
            .phase_deg = phase_deg,
            .distortion = distortion,
        };

        if (!staged_cfg_equal(&new_cfg, &s_last_staged[ch])) {
            ct_i2s_gen_channel_cfg_t gen_cfg = {
                .mode = (mode == CT_WAVE_MODE_MANUAL) ? CT_I2S_GEN_MODE_MANUAL : CT_I2S_GEN_MODE_MODEL,
                .amps = amps,
                .synth = {
                    .amplitude = 0.0f, // ignored/overwritten every sample by ct_i2s_gen.c -- see ct_i2s_gen.h
                    .phase_deg = phase_deg,
                    .dc_offset = distortion.dc_offset,
                    .clip_fraction = distortion.clip_fraction,
                    .dropout_half_cycle = distortion.dropout_half_cycle,
                    .dropout_negative_half = distortion.dropout_negative_half,
                },
                .apply_immediately = distortion.apply_immediately,
            };
            ct_i2s_gen_stage_config(&s_gen_ctx, ch, &gen_cfg);
            s_last_staged[ch] = new_cfg;

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

    for (;;) {
        apply_pending_commands();
        wave_owner_tick();
        ct_wave_i2s_poll();
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
    memset(s_last_staged, 0, sizeof(s_last_staged));
    memset(s_queue_head, 0, sizeof(s_queue_head));
    memset(s_queue_count, 0, sizeof(s_queue_count));
    for (uint8_t ch = 0; ch < CT_WAVE_NUM_CHANNELS; ch++) {
        s_channel_state[ch].mode = CT_WAVE_MODE_MODEL;
    }

    ct_i2s_gen_init(&s_gen_ctx, ct_cal_default_table());

    // wave_owner has no fallback CT backend to fall back to (ct_wave_pwm.c
    // is deleted, docs/DESIGN_NOTES.md section 3.3) -- ct_wave_i2s_init()
    // itself routes essentially every real failure mode (DMA exhaustion,
    // PIO SM exhaustion, PIO1 program-memory exhaustion -- the one EXPECTED
    // to actually fire on the current build, see ct_wave_i2s.h) through
    // simfw_fatal() and never returns from those. Reaching a `false` return
    // here only happens for its one non-fatal reason (a NULL refill
    // callback), which cannot occur with the function pointer passed below
    // -- but this task fails loudly anyway rather than silently booting
    // with no CT output, per this project's "no silent stub" doctrine
    // (drivers/simfw_fatal.h).
    if (!ct_wave_i2s_init(wave_owner_i2s_refill, NULL)) {
        simfw_fatal("wave_owner", "ct_wave_i2s_init() returned false -- no CT waveform backend available");
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
