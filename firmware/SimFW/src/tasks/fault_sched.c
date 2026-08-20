// fault_sched.c -- see fault_sched.h for the full design rationale
// (tick-lockstep with sim_engine instead of an independent task loop, the
// fault-type catalog and its target-kind mapping, and why fault_sched_tick()
// is the real trigger-evaluation entry point).
//
// --- Composition strategy: "recompute, don't patch" ------------------------
// PLAN.md 7.3: "Faults compose... genuinely conflicting pairs... resolve to
// the more severe one." Rather than tracking, per slot, which specific
// fields of a shared target's override state it is responsible for (so a
// CLEARED event can surgically undo exactly its own contribution), this
// pass recomputes each target's *entire* effective override from the
// current ACTIVE slot set from scratch, every tick (recompute_overrides_locked()
// below). This is simpler, has no missed-clear/stale-field failure mode
// (a slot that expires just stops contributing to the next recompute, full
// stop), and naturally implements a reasonable severity order for the one
// pass documents explicitly (TC_DEAD_IC-style total takeover would beat a
// lesser fault if both target the same channel -- though this pass's
// per-field merge, see below, mostly just accumulates independent knobs
// rather than needing a strict total ordering). Cost: a handful of extra
// writes into tc_fault_state/sim_engine every 100 ms tick even when nothing
// changed -- negligible at this rate.
//
// --- Known catalog gaps (PLAN.md 7.1) ---------------------------------------
// max31856_corruption_t (max31856_regs.h, locked contract, not modified by
// this pass) has no drift/offset, no "shorted-to-ambient", and no CJ-fault
// knob -- so "Drifting TC", "Shorted TC", and "CJ fault" from PLAN.md 7.1's
// catalog are not implemented; they would need a new field in that locked
// struct, out of this pass's scope (a genuine future addition, not a bug in
// what exists today). "Broken (intermittent) TC" needs no separate fault
// type -- it is TC_DISCONNECTED scheduled with an EVERY/FOR repeat+duration
// spec, which fault_engine.h already expresses. "Half-waving SSR" and
// "phase loss" are wave_owner/CT-side distortion knobs (PLAN.md 3.3), not
// this pass's target modules (wave_owner.c is still a stub another agent
// owns) -- out of scope here, left for a future pass once wave_owner has a
// real body and its own fault-state contract to write against.
#include "fault_sched.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "sim/tc_fault_state.h"
#include "tasks/sim_engine.h"
#include "tasks/i2c_owner.h"

#define FAULT_SCHED_STACK_WORDS   configMINIMAL_STACK_SIZE
#define FAULT_SCHED_IDLE_DELAY_MS 1000u

static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_engine_mutex = NULL;
static fault_engine_t s_engine;
static double s_last_sim_time_s = 0.0; /* updated each fault_sched_tick(), used
                                         * as fire_now()'s best-effort sim time
                                         * for an out-of-band manual fire */

static void engine_lock(void)
{
    xSemaphoreTake(s_engine_mutex, portMAX_DELAY);
}

static void engine_unlock(void)
{
    xSemaphoreGive(s_engine_mutex);
}

// Which kind of target a fault_type expects. Unknown/out-of-range values
// fall through to "reject" at the schedule() call site.
typedef enum { TARGET_KIND_TC, TARGET_KIND_ZONE, TARGET_KIND_SYSTEM, TARGET_KIND_INVALID } target_kind_t;

static target_kind_t target_kind_of(fault_sched_fault_type_t type)
{
    switch (type) {
    case FAULT_SCHED_TYPE_TC_DISCONNECTED:
    case FAULT_SCHED_TYPE_TC_NOISE:
    case FAULT_SCHED_TYPE_TC_STUCK:
    case FAULT_SCHED_TYPE_TC_DEAD_IC:
    case FAULT_SCHED_TYPE_TC_FLAKY_SPI:
    case FAULT_SCHED_TYPE_TC_SPURIOUS_FAULT_PIN:
        return TARGET_KIND_TC;
    case FAULT_SCHED_TYPE_WELDED_RELAY:
    case FAULT_SCHED_TYPE_STUCK_OPEN_RELAY:
    case FAULT_SCHED_TYPE_BROKEN_ELEMENT:
    case FAULT_SCHED_TYPE_PARTIAL_ELEMENT:
    case FAULT_SCHED_TYPE_RUNAWAY_ZONE:
        return TARGET_KIND_ZONE;
    case FAULT_SCHED_TYPE_ESTOP:
    case FAULT_SCHED_TYPE_AMBIENT_SHIFT:
        return TARGET_KIND_SYSTEM;
    }
    return TARGET_KIND_INVALID;
}

// Recomputes every TC-channel and zone override from the current
// ACTIVE-slot set and (re)publishes it, unconditionally. Caller holds
// s_engine_mutex. See file header, "recompute, don't patch".
static void recompute_overrides_locked(void)
{
    tc_fault_override_t tc_ovr[TC_FAULT_CHANNEL_COUNT];
    bool tc_active[TC_FAULT_CHANNEL_COUNT];
    memset(tc_ovr, 0, sizeof(tc_ovr));
    memset(tc_active, 0, sizeof(tc_active));

    bool duty_force1[THERMAL_MODEL_MAX_ZONES];
    bool duty_force0[THERMAL_MODEL_MAX_ZONES];
    bool health_active[THERMAL_MODEL_MAX_ZONES];
    float health_value[THERMAL_MODEL_MAX_ZONES];
    memset(duty_force1, 0, sizeof(duty_force1));
    memset(duty_force0, 0, sizeof(duty_force0));
    memset(health_active, 0, sizeof(health_active));
    memset(health_value, 0, sizeof(health_value));

    for (uint16_t slot_id = 0; slot_id < FAULT_ENGINE_MAX_SLOTS; slot_id++) {
        const fault_slot_t *slot = &s_engine.slots[slot_id];
        if (slot->state != FAULT_STATE_ACTIVE) {
            continue;
        }

        fault_sched_fault_type_t ft = (fault_sched_fault_type_t)slot->fault_type;
        uint16_t target = slot->target;

        switch (ft) {
        case FAULT_SCHED_TYPE_TC_DISCONNECTED:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].force_sr_bits |= MAX31856_FAULT_OPEN;
            }
            break;
        case FAULT_SCHED_TYPE_TC_NOISE:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                if (slot->params[0] > tc_ovr[target].corruption.noise_sigma_c) {
                    tc_ovr[target].corruption.noise_sigma_c = slot->params[0];
                }
            }
            break;
        case FAULT_SCHED_TYPE_TC_STUCK:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].corruption.stuck_ltcb = true;
            }
            break;
        case FAULT_SCHED_TYPE_TC_DEAD_IC:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].corruption.dead_mode = (max31856_dead_mode_t)(int)slot->params[0];
            }
            break;
        case FAULT_SCHED_TYPE_TC_FLAKY_SPI:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                if (slot->params[0] > tc_ovr[target].corruption.bit_error_rate) {
                    tc_ovr[target].corruption.bit_error_rate = slot->params[0];
                }
            }
            break;
        case FAULT_SCHED_TYPE_TC_SPURIOUS_FAULT_PIN:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].corruption.spurious_fault_pin = true;
            }
            break;
        case FAULT_SCHED_TYPE_WELDED_RELAY:
        case FAULT_SCHED_TYPE_RUNAWAY_ZONE:
            if (target < THERMAL_MODEL_MAX_ZONES) {
                duty_force1[target] = true;
            }
            break;
        case FAULT_SCHED_TYPE_STUCK_OPEN_RELAY:
            if (target < THERMAL_MODEL_MAX_ZONES) {
                duty_force0[target] = true;
            }
            break;
        case FAULT_SCHED_TYPE_BROKEN_ELEMENT:
            if (target < THERMAL_MODEL_MAX_ZONES) {
                health_active[target] = true;
                health_value[target] = 0.0f;
            }
            break;
        case FAULT_SCHED_TYPE_PARTIAL_ELEMENT:
            if (target < THERMAL_MODEL_MAX_ZONES && !health_active[target]) {
                health_active[target] = true;
                health_value[target] = slot->params[0];
            }
            break;
        case FAULT_SCHED_TYPE_ESTOP:
        case FAULT_SCHED_TYPE_AMBIENT_SHIFT:
            break; /* edge-driven in apply_edge_effects(), not level-recomputed here */
        }
    }

    for (unsigned ch = 0; ch < TC_FAULT_CHANNEL_COUNT; ch++) {
        if (tc_active[ch]) {
            tc_fault_state_write((tc_fault_channel_t)ch, &tc_ovr[ch]);
        } else {
            tc_fault_state_clear((tc_fault_channel_t)ch);
        }
    }

    for (unsigned z = 0; z < THERMAL_MODEL_MAX_ZONES; z++) {
        /* STUCK_OPEN_RELAY (force 0) wins over WELDED/RUNAWAY (force 1) if
         * both somehow target the same zone -- "genuinely conflicting pairs
         * resolve to the more severe one" (PLAN.md 7.3); a heater that
         * cannot possibly be conducting is the more severe/definite claim. */
        if (duty_force0[z]) {
            sim_engine_set_zone_duty_override((uint8_t)z, true, 0.0f);
        } else if (duty_force1[z]) {
            sim_engine_set_zone_duty_override((uint8_t)z, true, 1.0f);
        } else {
            sim_engine_set_zone_duty_override((uint8_t)z, false, 0.0f);
        }

        sim_engine_set_zone_health_override((uint8_t)z, health_active[z], health_value[z]);
    }
}

// Applies the one-shot, edge-triggered effects (ESTOP / AMBIENT_SHIFT) for
// every FIRED/CLEARED event this call produced. Caller holds s_engine_mutex.
static void apply_edge_effects(const fault_event_t *events, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const fault_event_t *ev = &events[i];
        if (ev->slot_id >= FAULT_ENGINE_MAX_SLOTS) {
            continue;
        }
        const fault_slot_t *slot = &s_engine.slots[ev->slot_id];
        fault_sched_fault_type_t ft = (fault_sched_fault_type_t)slot->fault_type;

        if (ft == FAULT_SCHED_TYPE_ESTOP) {
            i2c_owner_set_estop(ev->kind == FAULT_EVENT_FIRED);
        } else if (ft == FAULT_SCHED_TYPE_AMBIENT_SHIFT && ev->kind == FAULT_EVENT_FIRED) {
            /* Step-change simplification -- see fault_sched.h's catalog doc
             * for FAULT_SCHED_TYPE_AMBIENT_SHIFT. Not reverted on CLEAR. */
            sim_engine_set_ambient(slot->params[0]);
        }
    }
}

static void fault_sched_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        // Trigger evaluation now happens in fault_sched_tick(), called by
        // sim_engine.c once per its own 10 Hz tick (see this file's/
        // fault_sched.h's header comment for why). This task's own loop is
        // reserved for future event-driven work and just idles.
        vTaskDelay(pdMS_TO_TICKS(FAULT_SCHED_IDLE_DELAY_MS));
    }
}

bool fault_sched_start(void)
{
    s_engine_mutex = xSemaphoreCreateMutex();
    if (s_engine_mutex == NULL) {
        return false;
    }

    fault_engine_init(&s_engine, 0);

    BaseType_t ok = xTaskCreate(fault_sched_task_fn, "fault_sched", FAULT_SCHED_STACK_WORDS, NULL,
                                 SIMFW_PRIO_FAULT_SCHED, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}

uint16_t fault_sched_schedule(uint16_t slot_id,
                               fault_sched_fault_type_t fault_type,
                               uint16_t target,
                               const fault_trigger_t *trigger,
                               const fault_duration_t *duration,
                               const fault_repeat_t *repeat,
                               const float params[4])
{
    target_kind_t kind = target_kind_of(fault_type);
    if (kind == TARGET_KIND_TC && target >= TC_FAULT_CHANNEL_COUNT) {
        return FAULT_ENGINE_INVALID_SLOT;
    }
    if (kind == TARGET_KIND_ZONE && target >= THERMAL_MODEL_MAX_ZONES) {
        return FAULT_ENGINE_INVALID_SLOT;
    }
    if (kind == TARGET_KIND_INVALID) {
        return FAULT_ENGINE_INVALID_SLOT;
    }

    engine_lock();
    uint16_t result = fault_engine_schedule(&s_engine, slot_id, (uint16_t)fault_type, target,
                                             trigger, duration, repeat, params);
    engine_unlock();
    return result;
}

bool fault_sched_cancel(uint16_t slot_id)
{
    engine_lock();
    bool ok = fault_engine_cancel(&s_engine, slot_id);
    if (ok) {
        recompute_overrides_locked();
    }
    engine_unlock();
    return ok;
}

bool fault_sched_fire_now(uint16_t slot_id)
{
    fault_event_t events[2];

    engine_lock();
    size_t n = fault_engine_fire_now(&s_engine, slot_id, s_last_sim_time_s, events, 2);
    if (n > 0) {
        apply_edge_effects(events, n);
        recompute_overrides_locked();
    }
    engine_unlock();

    return n > 0;
}

size_t fault_sched_list(fault_slot_t *out, size_t max_out)
{
    if (out == NULL) {
        return 0;
    }

    size_t n = (max_out < FAULT_ENGINE_MAX_SLOTS) ? max_out : FAULT_ENGINE_MAX_SLOTS;

    engine_lock();
    memcpy(out, s_engine.slots, n * sizeof(fault_slot_t));
    engine_unlock();

    return n;
}

size_t fault_sched_tick(const fault_engine_snapshot_t *snapshot,
                         fault_event_t *out_events, size_t max_events)
{
    engine_lock();
    s_last_sim_time_s = snapshot->sim_time_s;
    size_t n = fault_engine_tick(&s_engine, snapshot, out_events, max_events);
    apply_edge_effects(out_events, n);
    recompute_overrides_locked();
    engine_unlock();

    return n;
}
