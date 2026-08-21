// fault_sched.c -- see fault_sched.h for the full design rationale
// (tick-lockstep with sim_engine instead of an independent task loop, the
// fault-type catalog and its target-kind mapping, and why fault_sched_tick()
// is the real trigger-evaluation entry point).
//
// --- Composition strategy: "recompute, don't patch" ------------------------
// DESIGN_NOTES.md 7.3: "Faults compose... genuinely conflicting pairs... resolve to
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
// --- Known catalog gaps (DESIGN_NOTES.md 7.1) ---------------------------------------
// Closed by this pass: "Drifting TC", "Shorted TC", and "CJ fault" now have
// real corruption knobs (max31856_regs.h's corruption.drift_offset_c/
// .shorted/.cj_fault_offset_c, extended in a backward-compatible way -- the
// struct is still copied wholesale by spi_emu_a/b, so no reader changed) and
// are wired below (FAULT_SCHED_TYPE_TC_DRIFT/_SHORTED/_CJ_FAULT). "Half-
// waving SSR" and "phase loss" are now wired too (FAULT_SCHED_TYPE_
// HALF_WAVE_SSR/_PHASE_LOSS), straight onto wave_owner's existing public API
// (ct_wave_set_distortion/_mode/_amps) -- wave_owner.c stopped being a stub
// since this gap note was last written, and its distortion knobs already
// covered "missing half-cycles" (DESIGN_NOTES.md 3.3's dropout_half_cycle) with no
// new sine_synth/wave_owner code required. "Broken (intermittent) TC" still
// needs no separate fault type -- it is TC_DISCONNECTED scheduled with an
// EVERY/FOR repeat+duration spec, which fault_engine.h already expresses.
//
// Gap-closure pass (this pass): "Main/safety disagree", "Welded K4 test
// support", "Thermal-mass surprise", and "Sensor-vs-element lag stress" are
// now wired too (FAULT_SCHED_TYPE_MAIN_SAFETY_DISAGREE/
// _WELDED_K4_CURRENT_PERSIST/_THERMAL_MASS_SURPRISE/_TC_LAG_STRESS), onto new
// sim_engine.h setters this same pass added
// (sim_engine_set_safety_tc_fault_override/_set_zone_thermal_override/
// _set_zone_tc_lag_override) plus wave_owner's existing CT API for the K4
// case (same edge-tracked pattern as HALF_WAVE_SSR/PHASE_LOSS -- no new
// wave_owner code needed). Also added: FAULT_SCHED_TYPE_DUT_POWER_CUT for
// scenarios/power_blip.yaml's `dut_power_cut`, even though it is explicitly
// NOT one of DESIGN_NOTES.md 7.1's three catalog tables (that scenario's own
// comment says so) -- its mechanism (i2c_owner_set_dut_power()) was already
// public and its trigger/duration shape already fits FAULT_SCHEDULE, so
// there was no reason to leave it as a PC-side-only capability. See each
// new enum value's doc comment in fault_sched.h for exact semantics.
//
// No remaining catalog gaps found in this pass's file list. Every other
// scenarios/*.yaml fault `type:` (grepped 2026-08-20) maps onto an
// already-implemented FAULT_SCHED_TYPE_* value; see this pass's report for
// the full mapping.
#include "fault_sched.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "sim/tc_fault_state.h"
#include "tasks/sim_engine.h"
#include "tasks/i2c_owner.h"
#include "tasks/wave_owner.h"

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
typedef enum { TARGET_KIND_TC, TARGET_KIND_ZONE, TARGET_KIND_CT, TARGET_KIND_SYSTEM, TARGET_KIND_INVALID } target_kind_t;

static target_kind_t target_kind_of(fault_sched_fault_type_t type)
{
    switch (type) {
    case FAULT_SCHED_TYPE_TC_DISCONNECTED:
    case FAULT_SCHED_TYPE_TC_NOISE:
    case FAULT_SCHED_TYPE_TC_STUCK:
    case FAULT_SCHED_TYPE_TC_DEAD_IC:
    case FAULT_SCHED_TYPE_TC_FLAKY_SPI:
    case FAULT_SCHED_TYPE_TC_SPURIOUS_FAULT_PIN:
    case FAULT_SCHED_TYPE_TC_SHORTED:
    case FAULT_SCHED_TYPE_TC_DRIFT:
    case FAULT_SCHED_TYPE_TC_CJ_FAULT:
        return TARGET_KIND_TC;
    case FAULT_SCHED_TYPE_WELDED_RELAY:
    case FAULT_SCHED_TYPE_STUCK_OPEN_RELAY:
    case FAULT_SCHED_TYPE_BROKEN_ELEMENT:
    case FAULT_SCHED_TYPE_PARTIAL_ELEMENT:
    case FAULT_SCHED_TYPE_RUNAWAY_ZONE:
    case FAULT_SCHED_TYPE_THERMAL_MASS_SURPRISE:
    case FAULT_SCHED_TYPE_TC_LAG_STRESS:
        return TARGET_KIND_ZONE;
    case FAULT_SCHED_TYPE_HALF_WAVE_SSR:
    case FAULT_SCHED_TYPE_PHASE_LOSS:
    case FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST:
        return TARGET_KIND_CT;
    case FAULT_SCHED_TYPE_ESTOP:
    case FAULT_SCHED_TYPE_AMBIENT_SHIFT:
    case FAULT_SCHED_TYPE_MAIN_SAFETY_DISAGREE:
    case FAULT_SCHED_TYPE_DUT_POWER_CUT:
        return TARGET_KIND_SYSTEM;
    }
    return TARGET_KIND_INVALID;
}

// Edge-tracking state for the two CT faults, fault_sched.c's own private
// memory -- NOT wave_owner state. Unlike the TC overrides (tc_fault_state.h)
// and the zone duty/health overrides (sim_engine.h), wave_owner.h has no
// dedicated "active bool + forced value, restores exactly what was there
// before" override concept for CT channels -- CT_WAVE_MODE_MANUAL/_MODEL is
// the same single switch an operator's own CT_SET_MODE command would use.
// So PHASE_LOSS's force-to-zero is only pushed to wave_owner on the
// inactive->active edge (ct_wave_set_mode(MANUAL) + ct_wave_set_amps(0)) and
// only handed back on the active->inactive edge (ct_wave_set_mode(MODEL)) --
// never unconditionally every tick -- specifically so a channel nobody has
// ever scheduled a fault against is never touched at all. This is a
// documented limitation (fault_sched.h's FAULT_SCHED_TYPE_PHASE_LOSS doc
// comment): while the fault is ACTIVE, it does compete with (and wins over)
// any operator-set MANUAL mode on the same channel, same as WELDED_RELAY
// already competes with the physical relay state. HALF_WAVE_SSR's
// distortion is edge-tracked the same way, for the same reason
// (ct_wave_set_distortion() "replaces channel's distortion config wholesale"
// per wave_owner.h -- pushing a zeroed struct every idle tick would also
// wipe any operator-set CT_SET_DISTORTION unrelated to this fault).
static bool s_ct_phase_loss_was_active[CT_WAVE_NUM_CHANNELS];
static bool s_ct_half_wave_was_active[CT_WAVE_NUM_CHANNELS];
// Same edge-tracking discipline, same reason, for
// FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST (fault_sched.h's doc comment).
static bool s_ct_k4_weld_was_active[CT_WAVE_NUM_CHANNELS];

// Recomputes every TC-channel, zone, and CT-channel override from the
// current ACTIVE-slot set and (re)publishes it, unconditionally (TC/zone) or
// edge-triggered (CT, see s_ct_phase_loss_was_active's comment above).
// Caller holds s_engine_mutex. See file header, "recompute, don't patch".
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

    bool ct_half_wave[CT_WAVE_NUM_CHANNELS];
    bool ct_half_wave_negative[CT_WAVE_NUM_CHANNELS];
    bool ct_phase_loss[CT_WAVE_NUM_CHANNELS];
    bool ct_k4_weld[CT_WAVE_NUM_CHANNELS];
    float ct_k4_weld_amps[CT_WAVE_NUM_CHANNELS];
    memset(ct_half_wave, 0, sizeof(ct_half_wave));
    memset(ct_half_wave_negative, 0, sizeof(ct_half_wave_negative));
    memset(ct_phase_loss, 0, sizeof(ct_phase_loss));
    memset(ct_k4_weld, 0, sizeof(ct_k4_weld));
    memset(ct_k4_weld_amps, 0, sizeof(ct_k4_weld_amps));

    bool thermal_active[THERMAL_MODEL_MAX_ZONES];
    float thermal_C[THERMAL_MODEL_MAX_ZONES];
    float thermal_k_loss[THERMAL_MODEL_MAX_ZONES];
    bool tc_lag_active[THERMAL_MODEL_MAX_ZONES];
    float tc_lag_value[THERMAL_MODEL_MAX_ZONES];
    memset(thermal_active, 0, sizeof(thermal_active));
    memset(thermal_C, 0, sizeof(thermal_C));
    memset(thermal_k_loss, 0, sizeof(thermal_k_loss));
    memset(tc_lag_active, 0, sizeof(tc_lag_active));
    memset(tc_lag_value, 0, sizeof(tc_lag_value));

    bool safety_tc_active = false;
    float safety_tc_offset = 0.0f;
    float safety_tc_gain = 1.0f; /* default -- see FAULT_SCHED_TYPE_MAIN_SAFETY_DISAGREE's
                                   * doc comment on why an unset params[1] means 1.0 */

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
        case FAULT_SCHED_TYPE_TC_SHORTED:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].corruption.shorted = true;
            }
            break;
        case FAULT_SCHED_TYPE_TC_DRIFT:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                /* Recomputed from scratch every tick, per fault_sched.h's
                 * doc comment: rate * elapsed sim seconds since this slot
                 * went ACTIVE, never an incremental accumulation. */
                double elapsed_s = s_last_sim_time_s - slot->active_since_s;
                if (elapsed_s < 0.0) {
                    elapsed_s = 0.0;
                }
                tc_ovr[target].corruption.drift_offset_c = slot->params[0] * (float)elapsed_s;
            }
            break;
        case FAULT_SCHED_TYPE_TC_CJ_FAULT:
            if (target < TC_FAULT_CHANNEL_COUNT) {
                tc_active[target] = true;
                tc_ovr[target].corruption.cj_fault_offset_c = slot->params[0];
            }
            break;
        case FAULT_SCHED_TYPE_HALF_WAVE_SSR:
            if (target < CT_WAVE_NUM_CHANNELS) {
                ct_half_wave[target] = true;
                ct_half_wave_negative[target] = (slot->params[0] != 0.0f);
            }
            break;
        case FAULT_SCHED_TYPE_PHASE_LOSS:
            if (target < CT_WAVE_NUM_CHANNELS) {
                ct_phase_loss[target] = true;
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
        case FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST:
            if (target < CT_WAVE_NUM_CHANNELS) {
                ct_k4_weld[target] = true;
                if (slot->params[0] > ct_k4_weld_amps[target]) {
                    ct_k4_weld_amps[target] = slot->params[0]; /* multiple slots targeting
                                                                 * the same channel: take
                                                                 * the larger forced
                                                                 * current, same
                                                                 * "more severe wins"
                                                                 * doctrine used elsewhere */
                }
            }
            break;
        case FAULT_SCHED_TYPE_THERMAL_MASS_SURPRISE:
            if (target < THERMAL_MODEL_MAX_ZONES) {
                thermal_active[target] = true;
                thermal_C[target] = slot->params[0];
                thermal_k_loss[target] = slot->params[1];
            }
            break;
        case FAULT_SCHED_TYPE_TC_LAG_STRESS:
            if (target < THERMAL_MODEL_MAX_ZONES) {
                tc_lag_active[target] = true;
                tc_lag_value[target] = slot->params[0];
            }
            break;
        case FAULT_SCHED_TYPE_MAIN_SAFETY_DISAGREE:
            safety_tc_active = true;
            safety_tc_offset += slot->params[0]; /* additive composition across concurrent
                                                    * slots, same doctrine as TC_DRIFT's
                                                    * "recompute from scratch" -- see
                                                    * fault_sched.h's doc comment */
            if (slot->params[1] != 0.0f) {
                safety_tc_gain = slot->params[1]; /* last slot in index order wins if more
                                                     * than one supplies an explicit gain --
                                                     * same "slot order is the deterministic
                                                     * tiebreak" doctrine DESIGN_NOTES.md 7.3 states
                                                     * for trigger evaluation order */
            }
            break;
        case FAULT_SCHED_TYPE_ESTOP:
        case FAULT_SCHED_TYPE_AMBIENT_SHIFT:
        case FAULT_SCHED_TYPE_DUT_POWER_CUT:
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
         * resolve to the more severe one" (DESIGN_NOTES.md 7.3); a heater that
         * cannot possibly be conducting is the more severe/definite claim. */
        if (duty_force0[z]) {
            sim_engine_set_zone_duty_override((uint8_t)z, true, 0.0f);
        } else if (duty_force1[z]) {
            sim_engine_set_zone_duty_override((uint8_t)z, true, 1.0f);
        } else {
            sim_engine_set_zone_duty_override((uint8_t)z, false, 0.0f);
        }

        sim_engine_set_zone_health_override((uint8_t)z, health_active[z], health_value[z]);
        sim_engine_set_zone_thermal_override((uint8_t)z, thermal_active[z], thermal_C[z], thermal_k_loss[z]);
        sim_engine_set_zone_tc_lag_override((uint8_t)z, tc_lag_active[z], tc_lag_value[z]);
    }

    sim_engine_set_safety_tc_fault_override(safety_tc_active, safety_tc_offset,
                                             safety_tc_active ? safety_tc_gain : 1.0f);

    for (unsigned c = 0; c < CT_WAVE_NUM_CHANNELS; c++) {
        /* PHASE_LOSS (force to zero) wins over HALF_WAVE_SSR if both somehow
         * target the same channel -- "genuinely conflicting pairs resolve
         * to the more severe one" (DESIGN_NOTES.md 7.3); a channel that cannot
         * possibly be conducting is the more severe/definite claim, same
         * doctrine as STUCK_OPEN_RELAY winning over WELDED_RELAY above. */
        if (ct_phase_loss[c]) {
            if (!s_ct_phase_loss_was_active[c]) {
                ct_wave_set_mode((uint8_t)c, CT_WAVE_MODE_MANUAL);
                ct_wave_set_amps((uint8_t)c, 0.0f);
            }
        } else if (s_ct_phase_loss_was_active[c]) {
            ct_wave_set_mode((uint8_t)c, CT_WAVE_MODE_MODEL);
        }
        s_ct_phase_loss_was_active[c] = ct_phase_loss[c];

        bool half_wave_now = ct_half_wave[c] && !ct_phase_loss[c]; /* phase loss already zeroes the channel */
        if (half_wave_now) {
            if (!s_ct_half_wave_was_active[c]) {
                ct_wave_distortion_t dist;
                memset(&dist, 0, sizeof(dist));
                dist.dropout_half_cycle = true;
                dist.dropout_negative_half = ct_half_wave_negative[c];
                ct_wave_set_distortion((uint8_t)c, &dist);
            }
        } else if (s_ct_half_wave_was_active[c]) {
            ct_wave_distortion_t dist;
            memset(&dist, 0, sizeof(dist));
            ct_wave_set_distortion((uint8_t)c, &dist);
        }
        s_ct_half_wave_was_active[c] = half_wave_now;

        /* WELDED_K4_CURRENT_PERSIST -- PHASE_LOSS (force to zero) wins if
         * both somehow target the same channel, same doctrine as above. */
        bool k4_weld_now = ct_k4_weld[c] && !ct_phase_loss[c];
        if (k4_weld_now) {
            if (!s_ct_k4_weld_was_active[c]) {
                ct_wave_set_mode((uint8_t)c, CT_WAVE_MODE_MANUAL);
                ct_wave_set_amps((uint8_t)c, ct_k4_weld_amps[c]);
            }
        } else if (s_ct_k4_weld_was_active[c]) {
            ct_wave_set_mode((uint8_t)c, CT_WAVE_MODE_MODEL);
        }
        s_ct_k4_weld_was_active[c] = k4_weld_now;
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
        } else if (ft == FAULT_SCHED_TYPE_DUT_POWER_CUT) {
            /* FIRED = power cut (relay opens), CLEARED = power restored --
             * see fault_sched.h's doc comment for why this type exists
             * despite not being one of DESIGN_NOTES.md 7.1's three catalog tables. */
            i2c_owner_set_dut_power(ev->kind != FAULT_EVENT_FIRED);
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
    if (kind == TARGET_KIND_CT && target >= CT_WAVE_NUM_CHANNELS) {
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
    // Deferred: fault_engine_cancel() only marks the request (immediately,
    // for a non-ACTIVE slot; via cancel_pending, for an ACTIVE one). Any
    // override write and any CLEARED event happen on the next
    // fault_sched_tick() call, from sim_engine's own task -- see this
    // file's header doc on fault_sched_cancel().
    engine_lock();
    bool ok = fault_engine_cancel(&s_engine, slot_id);
    engine_unlock();
    return ok;
}

bool fault_sched_fire_now(uint16_t slot_id)
{
    // Deferred: fault_engine_fire_now() only marks manual_fire_pending. The
    // actual ARMED->ACTIVE transition, its FIRED event, and the resulting
    // override writes/edge effects all happen on the next fault_sched_tick()
    // call, from sim_engine's own task -- see this file's header doc on
    // fault_sched_fire_now().
    engine_lock();
    bool ok = fault_engine_fire_now(&s_engine, slot_id);
    engine_unlock();
    return ok;
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
