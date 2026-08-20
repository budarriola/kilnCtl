// sim_engine.c -- see sim_engine.h for the full task doc, the MODEL-group
// command surface, and the zone-to-relay/TC-channel mapping assumption this
// file makes (K1/K2/K3 <-> zone0/1/2, documented at length in the header so
// it is easy to find and correct once docs/HARDWARE.md exists).
//
// Also implements src/sim/sim_snapshot.h's reader API
// (sim_snapshot_read()/sim_event_ring_drain()) per that header's "sole
// writer/producer" doctrine, and calls fault_sched_tick() once per tick in
// lockstep (see fault_sched.h's header comment for why that call lives here
// rather than in an independently-scheduled fault_sched task loop).
//
// --- Tick order (deterministic, single task, single thread of execution) ---
//   1. Drain the MODEL-group command queue (params/preset/ambient/
//      timescale/seed/reset/force-temp/clear-manual), applied at this tick
//      boundary (PLAN.md 4.5's queue-then-apply-next-tick doctrine).
//   2. Read this tick's inputs: relay states + E-stop + DUT power from
//      i2c_owner's existing public getters (i2c_owner.h; this file never
//      touches I2C0 itself).
//   3. Build fault_engine.h's snapshot type from *last* tick's zone temps
//      (state.T_zone) and *this* tick's relay levels, then call
//      fault_sched_tick() -- its FIRED/CLEARED effects (duty/health
//      overrides, TC corruption via tc_fault_state.h, i2c_owner_set_estop(),
//      sim_engine_set_ambient()) land immediately, before step 4, so this
//      tick's physics already sees them.
//   4. Compute relay-derived duty[] per zone, apply any active duty
//      override, build an effective params copy with any active health
//      override, then thermal_model_tick().
//   5. Apply MANUAL zone-temp overrides (pin T_zone/T_tc, post-physics).
//   6. Advance sim_time_us by dt_s (wall period * timescale).
//   7. Compute current_a[] per zone from the *effective* duty/health used
//      in step 4 (PLAN.md 3.3's formula) -- so a welded-relay fault's
//      forced duty correctly shows up as CT current too.
//   8. Publish the snapshot (seq-counter protocol).
//   9. Detect relay edges vs. last tick's mask; push SIM_EVENT_RELAY_EDGE
//      events, then push this tick's fault_sched_tick() events as
//      SIM_EVENT_FAULT_FIRED/CLEARED. sim_engine.c is the ring's sole
//      producer throughout (sim_snapshot.h's doctrine) -- fault_sched only
//      ever hands back a plain fault_event_t array, never touches the ring.
#include "sim_engine.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "sim/sim_snapshot.h"
#include "tasks/fault_sched.h"
#include "tasks/i2c_owner.h"

#define SIM_ENGINE_STACK_WORDS (configMINIMAL_STACK_SIZE * 2u) // headroom for the fault-event buffer + snapshot copies below

// --- MODEL-group command queue (i2c_owner.c's queue-then-apply-next-tick
// pattern, per this pass's own instructions) ---------------------------------
typedef enum {
    SIM_ENGINE_CMD_SET_ZONE_PARAMS,
    SIM_ENGINE_CMD_LOAD_PRESET,
    SIM_ENGINE_CMD_SET_AMBIENT,
    SIM_ENGINE_CMD_SET_TIMESCALE,
    SIM_ENGINE_CMD_SET_SEED,
    SIM_ENGINE_CMD_RESET,
    SIM_ENGINE_CMD_FORCE_ZONE_TEMP,
    SIM_ENGINE_CMD_CLEAR_ZONE_MANUAL,
} sim_engine_cmd_type_t;

typedef struct {
    sim_engine_cmd_type_t type;
    union {
        struct { uint8_t zone; thermal_zone_params_t params; } zone_params;
        struct { thermal_preset_id_t preset; } preset;
        struct { float ambient_c; } ambient;
        struct { uint32_t timescale_x100; } timescale;
        struct { uint32_t seed; } seed;
        struct { bool keep_params; } reset;
        struct { uint8_t zone; float temp_c; } force_temp;
        struct { uint8_t zone; } clear_manual;
    } u;
} sim_engine_cmd_t;

#define SIM_ENGINE_CMD_QUEUE_DEPTH 16u

static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_params_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

// --- Task-context-only state (single writer: this task's own tick) --------
static thermal_model_params_t s_params;
static thermal_model_state_t  s_state;
static thermal_preset_id_t    s_current_preset = THERMAL_PRESET_FAST_TEST;
static uint32_t s_timescale_x100 = 100; // 1.00x
static uint32_t s_seed = 0;
static uint64_t s_sim_time_us = 0;
static uint16_t s_relay_mask_prev = 0;

static bool  s_zone_manual[THERMAL_MODEL_MAX_ZONES];
static float s_zone_manual_temp_c[THERMAL_MODEL_MAX_ZONES];

// fault_sched's power-path hook -- NOT queued, see sim_engine.h's doc on
// sim_engine_set_zone_duty_override()/_health_override(): only ever called
// from fault_sched_tick(), itself only ever called from this task's own
// tick below, so a direct write here has no cross-task race.
static bool  s_zone_duty_override_active[THERMAL_MODEL_MAX_ZONES];
static float s_zone_duty_override_value[THERMAL_MODEL_MAX_ZONES];
static bool  s_zone_health_override_active[THERMAL_MODEL_MAX_ZONES];
static float s_zone_health_override_value[THERMAL_MODEL_MAX_ZONES];

// --- Published snapshot (sim_snapshot.h's seq-counter protocol) -----------
// Same lock-free discipline as src/sim/tc_fault_state.c: RP2040 SMP cores
// are in-order with no data cache, so a volatile seq counter + a compiler
// memory-clobber barrier is sufficient, no FreeRTOS primitive needed.
static volatile uint32_t s_pub_seq = 0; // 0 == never published (even, so a
                                          // reader sees a torn-free all-zero
                                          // snapshot rather than spinning)
static sim_snapshot_t s_pub_snapshot;
static bool s_has_published = false;

// --- Event ring (sim_snapshot.h) -------------------------------------------
// Multiple concurrent consumers (telemetry, future cmd_task) are expected
// (sim_snapshot.h: "single-producer/multi-consumer draining... sim_engine
// owns [the scheme]") -- a short FreeRTOS mutex around the ring is simplest
// and correct at this 10 Hz production rate; sim_engine.c is the sole
// producer and only ever takes it for the few instructions needed to append
// one event, so this never becomes a real contention point.
static SemaphoreHandle_t s_ring_mutex = NULL;
static sim_event_t s_ring[SIM_EVENT_RING_SIZE];
static uint32_t s_ring_next_seq = 0;

static void params_lock(void) { xSemaphoreTake(s_params_mutex, portMAX_DELAY); }
static void params_unlock(void) { xSemaphoreGive(s_params_mutex); }

static void ring_push(sim_event_type_t type, uint8_t a, uint8_t b, float f0)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    uint32_t seq = s_ring_next_seq++;
    sim_event_t *slot = &s_ring[seq % SIM_EVENT_RING_SIZE];
    slot->seq = seq;
    slot->sim_time_us = s_sim_time_us;
    slot->type = type;
    slot->a = a;
    slot->b = b;
    slot->f0 = f0;
    xSemaphoreGive(s_ring_mutex);
}

// --- Command application (task context only) --------------------------------

static void reset_zone_manual_overrides(void)
{
    memset(s_zone_manual, 0, sizeof(s_zone_manual));
    memset(s_zone_manual_temp_c, 0, sizeof(s_zone_manual_temp_c));
}

static void apply_reset(bool keep_params)
{
    if (!keep_params) {
        thermal_model_load_preset(s_current_preset, &s_params);
    }
    thermal_model_init(&s_state, &s_params);
    s_sim_time_us = 0;
    reset_zone_manual_overrides();

    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    s_ring_next_seq = 0;
    xSemaphoreGive(s_ring_mutex);
}

static void apply_pending_commands(void)
{
    sim_engine_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case SIM_ENGINE_CMD_SET_ZONE_PARAMS:
            if (cmd.u.zone_params.zone < s_params.zone_count) {
                params_lock();
                s_params.zones[cmd.u.zone_params.zone] = cmd.u.zone_params.params;
                params_unlock();
            }
            break;

        case SIM_ENGINE_CMD_LOAD_PRESET:
            params_lock();
            s_current_preset = cmd.u.preset.preset;
            thermal_model_load_preset(s_current_preset, &s_params);
            params_unlock();
            thermal_model_init(&s_state, &s_params);
            s_sim_time_us = 0;
            reset_zone_manual_overrides();
            break;

        case SIM_ENGINE_CMD_SET_AMBIENT:
            params_lock();
            s_params.T_ambient = cmd.u.ambient.ambient_c;
            params_unlock();
            break;

        case SIM_ENGINE_CMD_SET_TIMESCALE:
            s_timescale_x100 = cmd.u.timescale.timescale_x100;
            break;

        case SIM_ENGINE_CMD_SET_SEED:
            s_seed = cmd.u.seed.seed; // stored only -- see sim_engine.h's doc
            break;

        case SIM_ENGINE_CMD_RESET:
            apply_reset(cmd.u.reset.keep_params);
            break;

        case SIM_ENGINE_CMD_FORCE_ZONE_TEMP:
            if (cmd.u.force_temp.zone < THERMAL_MODEL_MAX_ZONES) {
                s_zone_manual[cmd.u.force_temp.zone] = true;
                s_zone_manual_temp_c[cmd.u.force_temp.zone] = cmd.u.force_temp.temp_c;
            }
            break;

        case SIM_ENGINE_CMD_CLEAR_ZONE_MANUAL:
            if (cmd.u.clear_manual.zone < THERMAL_MODEL_MAX_ZONES) {
                s_zone_manual[cmd.u.clear_manual.zone] = false;
            }
            break;
        }
    }
}

// --- One tick ----------------------------------------------------------------

static void sim_engine_tick(void)
{
    apply_pending_commands();

    i2c_owner_relay_states_t relay = i2c_owner_get_relay_states();
    bool estop_open = i2c_owner_get_estop_open();
    bool dut_power_on = i2c_owner_get_dut_power_on();

    uint16_t relay_mask = 0;
    if (relay.k1_closed) relay_mask |= (1u << SIM_RELAY_BIT_K1);
    if (relay.k2_closed) relay_mask |= (1u << SIM_RELAY_BIT_K2);
    if (relay.k3_closed) relay_mask |= (1u << SIM_RELAY_BIT_K3);
    if (relay.k5_closed) relay_mask |= (1u << SIM_RELAY_BIT_K5);
    if (relay.k4_closed) relay_mask |= (1u << SIM_RELAY_BIT_K4);

    // --- fault_sched, in lockstep (see file header + fault_sched.h) --------
    bool relay_bools[FAULT_ENGINE_MAX_RELAYS] = { false };
    relay_bools[SIM_RELAY_BIT_K1] = relay.k1_closed;
    relay_bools[SIM_RELAY_BIT_K2] = relay.k2_closed;
    relay_bools[SIM_RELAY_BIT_K3] = relay.k3_closed;
    relay_bools[SIM_RELAY_BIT_K5] = relay.k5_closed;
    relay_bools[SIM_RELAY_BIT_K4] = relay.k4_closed;

    fault_engine_snapshot_t fault_snap = {
        .sim_time_s = (double)s_sim_time_us / 1.0e6,
        .zone_temps = s_state.T_zone, // last tick's true temps -- one-tick
                                        // lag on AT_ZONE_TEMP triggers,
                                        // deterministic and documented here
        .zone_count = s_params.zone_count,
        .relay_states = relay_bools,
        .relay_count = SIM_RELAY_BIT_COUNT,
        .event_names = NULL, // named-event (ON_EVENT) wiring is future work
        .event_count = 0,
    };

    fault_event_t fault_events[FAULT_ENGINE_MAX_SLOTS * 2u];
    size_t fault_event_count = fault_sched_tick(&fault_snap, fault_events,
                                                 sizeof(fault_events) / sizeof(fault_events[0]));

    // --- duty[] : relay-derived base, then any fault override ---------------
    // Zone-to-relay mapping ASSUMPTION -- see sim_engine.h's header comment.
    float duty[THERMAL_MODEL_MAX_ZONES] = { 0 };
    static const sim_relay_bit_t zone_relay_bit[3] = { SIM_RELAY_BIT_K1, SIM_RELAY_BIT_K2, SIM_RELAY_BIT_K3 };
    for (uint8_t z = 0; z < s_params.zone_count && z < 3u; z++) {
        duty[z] = (relay_mask & (1u << zone_relay_bit[z])) ? 1.0f : 0.0f;
    }
    // zone 3 (if present): no relay in this mapping, base duty stays 0.

    for (uint8_t z = 0; z < s_params.zone_count; z++) {
        if (s_zone_duty_override_active[z]) {
            duty[z] = s_zone_duty_override_value[z];
        }
    }

    params_lock();
    thermal_model_params_t eff_params = s_params;
    params_unlock();
    for (uint8_t z = 0; z < eff_params.zone_count; z++) {
        if (s_zone_health_override_active[z]) {
            eff_params.zones[z].element_health = s_zone_health_override_value[z];
        }
    }

    uint32_t scale_int = s_timescale_x100 / 100u;
    if (scale_int == 0u) {
        scale_int = 1u;
    }
    float dt_s = ((float)SIMFW_PERIOD_SIM_ENGINE_MS / 1000.0f) * ((float)s_timescale_x100 / 100.0f);

    thermal_model_tick(&s_state, &eff_params, duty, dt_s, scale_int);

    // --- MANUAL zone-temp overrides, post-physics ---------------------------
    for (uint8_t z = 0; z < eff_params.zone_count; z++) {
        if (s_zone_manual[z]) {
            s_state.T_zone[z] = s_zone_manual_temp_c[z];
            s_state.T_tc[z] = s_zone_manual_temp_c[z];
        }
    }

    s_sim_time_us += (uint64_t)(dt_s * 1.0e6f);

    // --- current_a[] : PLAN.md 3.3's formula, using the *effective*
    // duty/health this tick actually used, so fault overrides show up in
    // current too. ---------------------------------------------------------
    float current_a[THERMAL_MODEL_MAX_ZONES] = { 0 };
    for (uint8_t z = 0; z < eff_params.zone_count; z++) {
        float r = eff_params.zones[z].R_element;
        if (r > 0.0f) {
            current_a[z] = (eff_params.V_mains / r) * duty[z] * eff_params.zones[z].element_health;
        }
    }

    // --- Publish -------------------------------------------------------------
    uint32_t seq = s_pub_seq;
    s_pub_seq = seq + 1u; // odd
    __asm volatile("" ::: "memory");

    s_pub_snapshot.sim_time_us = s_sim_time_us;
    s_pub_snapshot.timescale_x100 = s_timescale_x100;
    s_pub_snapshot.zone_count = eff_params.zone_count;
    for (uint8_t z = 0; z < eff_params.zone_count; z++) {
        s_pub_snapshot.zones[z].T_true_c = s_state.T_zone[z];
        s_pub_snapshot.zones[z].T_tc_reported_c = s_state.T_tc[z];
        s_pub_snapshot.zones[z].T_safety_reported_c = s_state.T_tc[z]; // default blend: zone0's
                                                                         // lagged reading stands in
                                                                         // for every zone until a
                                                                         // dedicated safety-blend
                                                                         // parameter exists
                                                                         // (PLAN.md 4.3) -- see
                                                                         // report note.
        s_pub_snapshot.zones[z].current_a = current_a[z];
    }
    s_pub_snapshot.relay_mask = relay_mask;
    s_pub_snapshot.estop_open = estop_open;
    s_pub_snapshot.dut_power_on = dut_power_on;

    __asm volatile("" ::: "memory");
    s_pub_seq = seq + 2u; // even
    s_has_published = true;

    // --- Relay edge events -----------------------------------------------
    uint16_t changed = relay_mask ^ s_relay_mask_prev;
    if (changed != 0u) {
        for (uint8_t bit = 0; bit < SIM_RELAY_BIT_COUNT; bit++) {
            if (changed & (1u << bit)) {
                bool now_closed = (relay_mask & (1u << bit)) != 0u;
                ring_push(SIM_EVENT_RELAY_EDGE, bit, now_closed ? 1u : 0u, 0.0f);
            }
        }
    }
    s_relay_mask_prev = relay_mask;

    // --- Fault fired/cleared events ----------------------------------------
    for (size_t i = 0; i < fault_event_count; i++) {
        sim_event_type_t type = (fault_events[i].kind == FAULT_EVENT_FIRED)
                                     ? SIM_EVENT_FAULT_FIRED
                                     : SIM_EVENT_FAULT_CLEARED;
        ring_push(type, (uint8_t)fault_events[i].slot_id, 0u, 0.0f);
    }
}

static void sim_engine_task_fn(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        sim_engine_tick();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SIMFW_PERIOD_SIM_ENGINE_MS));
    }
}

bool sim_engine_start(void)
{
    s_cmd_queue = xQueueCreate(SIM_ENGINE_CMD_QUEUE_DEPTH, sizeof(sim_engine_cmd_t));
    if (s_cmd_queue == NULL) {
        return false;
    }

    s_params_mutex = xSemaphoreCreateMutex();
    if (s_params_mutex == NULL) {
        return false;
    }

    s_ring_mutex = xSemaphoreCreateMutex();
    if (s_ring_mutex == NULL) {
        return false;
    }

    thermal_model_load_preset(THERMAL_PRESET_FAST_TEST, &s_params);
    s_current_preset = THERMAL_PRESET_FAST_TEST;
    thermal_model_init(&s_state, &s_params);

    BaseType_t ok = xTaskCreate(sim_engine_task_fn, "sim_engine", SIM_ENGINE_STACK_WORDS, NULL,
                                 SIMFW_PRIO_SIM_ENGINE, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}

// --- MODEL-group setters/getters ---------------------------------------------

bool sim_engine_set_zone_params(uint8_t zone, const thermal_zone_params_t *params)
{
    if (!s_cmd_queue || params == NULL || zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_SET_ZONE_PARAMS };
    cmd.u.zone_params.zone = zone;
    cmd.u.zone_params.params = *params;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_get_zone_params(uint8_t zone, thermal_zone_params_t *out)
{
    if (out == NULL || zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    params_lock();
    bool in_range = zone < s_params.zone_count;
    if (in_range) {
        *out = s_params.zones[zone];
    }
    params_unlock();
    return in_range;
}

bool sim_engine_load_preset(thermal_preset_id_t preset)
{
    if (!s_cmd_queue || preset >= THERMAL_PRESET_COUNT) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_LOAD_PRESET, .u.preset = { .preset = preset } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_set_ambient(float ambient_c)
{
    if (!s_cmd_queue) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_SET_AMBIENT, .u.ambient = { .ambient_c = ambient_c } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_set_timescale(uint32_t timescale_x100)
{
    if (!s_cmd_queue) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_SET_TIMESCALE,
                              .u.timescale = { .timescale_x100 = timescale_x100 } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_set_seed(uint32_t seed)
{
    if (!s_cmd_queue) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_SET_SEED, .u.seed = { .seed = seed } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_reset(bool keep_params)
{
    if (!s_cmd_queue) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_RESET, .u.reset = { .keep_params = keep_params } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_force_zone_temp(uint8_t zone, float temp_c)
{
    if (!s_cmd_queue || zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_FORCE_ZONE_TEMP };
    cmd.u.force_temp.zone = zone;
    cmd.u.force_temp.temp_c = temp_c;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool sim_engine_clear_zone_manual(uint8_t zone)
{
    if (!s_cmd_queue || zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    sim_engine_cmd_t cmd = { .type = SIM_ENGINE_CMD_CLEAR_ZONE_MANUAL };
    cmd.u.clear_manual.zone = zone;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

// --- fault_sched's direct (non-queued) power-path hook -----------------------

bool sim_engine_set_zone_duty_override(uint8_t zone, bool active, float duty)
{
    if (zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    s_zone_duty_override_active[zone] = active;
    s_zone_duty_override_value[zone] = duty;
    return true;
}

bool sim_engine_set_zone_health_override(uint8_t zone, bool active, float health)
{
    if (zone >= THERMAL_MODEL_MAX_ZONES) {
        return false;
    }
    s_zone_health_override_active[zone] = active;
    s_zone_health_override_value[zone] = health;
    return true;
}

// --- sim_snapshot.h reader API (implemented here -- sim_engine.c is the
// sole writer/producer, per that header's doctrine) --------------------------

bool sim_snapshot_read(sim_snapshot_t *out)
{
    if (out == NULL || !s_has_published) {
        return false;
    }

    for (;;) {
        uint32_t seq_before = s_pub_seq;
        if (seq_before & 1u) {
            continue;
        }
        __asm volatile("" ::: "memory");
        sim_snapshot_t copy = s_pub_snapshot;
        __asm volatile("" ::: "memory");
        uint32_t seq_after = s_pub_seq;

        if (seq_after == seq_before) {
            copy.seq = seq_before;
            *out = copy;
            return true;
        }
    }
}

uint32_t sim_event_ring_drain(sim_event_t *out, uint32_t max_out, uint32_t *inout_next_seq)
{
    if (out == NULL || max_out == 0u || inout_next_seq == NULL) {
        return 0;
    }

    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    uint32_t oldest_available = (s_ring_next_seq > SIM_EVENT_RING_SIZE)
                                     ? (s_ring_next_seq - SIM_EVENT_RING_SIZE)
                                     : 0u;
    uint32_t start_seq = *inout_next_seq;
    if (start_seq < oldest_available) {
        start_seq = oldest_available; // ring wrapped past what the caller last saw
    }

    uint32_t count = 0;
    while (start_seq + count < s_ring_next_seq && count < max_out) {
        out[count] = s_ring[(start_seq + count) % SIM_EVENT_RING_SIZE];
        count++;
    }

    if (count > 0u) {
        *inout_next_seq = start_seq + count;
    } else {
        *inout_next_seq = start_seq;
    }

    xSemaphoreGive(s_ring_mutex);
    return count;
}
