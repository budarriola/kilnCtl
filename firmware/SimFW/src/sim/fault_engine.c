// fault_engine.c -- see fault_engine.h for the trigger/duration/repeat
// contract and the determinism guarantee (DESIGN_NOTES.md sections 7.1-7.3).
#include "fault_engine.h"

#include <string.h>

/* xorshift32 -- same algorithm as firmware/CommonFW/test/test_fuzz.c and
 * max31856_regs.c. Deterministic from a fixed seed; not cryptographic. */
static uint32_t xorshift32_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float xorshift32_float01(uint32_t *state)
{
    return (float)(xorshift32_next(state) >> 8) * (1.0f / 16777216.0f);
}

void fault_engine_init(fault_engine_t *eng, uint32_t seed)
{
    memset(eng, 0, sizeof(*eng));
    eng->rng_state = (seed != 0u) ? seed : 0x9E3779B9u;
    for (uint16_t i = 0; i < FAULT_ENGINE_MAX_SLOTS; i++) {
        eng->slots[i].slot_id = i;
        eng->slots[i].state = FAULT_STATE_IDLE;
    }
}

void fault_engine_reseed(fault_engine_t *eng, uint32_t seed)
{
    eng->rng_state = (seed != 0u) ? seed : 0x9E3779B9u;
}

uint16_t fault_engine_schedule(fault_engine_t *eng,
                                uint16_t slot_id,
                                uint16_t fault_type,
                                uint16_t target,
                                const fault_trigger_t *trigger,
                                const fault_duration_t *duration,
                                const fault_repeat_t *repeat,
                                const float params[4])
{
    if (slot_id >= FAULT_ENGINE_MAX_SLOTS) {
        return FAULT_ENGINE_INVALID_SLOT;
    }
    fault_slot_t *slot = &eng->slots[slot_id];
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = slot_id;
    slot->state = FAULT_STATE_ARMED;
    slot->fault_type = fault_type;
    slot->target = target;
    slot->trigger = *trigger;
    slot->duration = *duration;
    slot->repeat = *repeat;
    if (params) {
        memcpy(slot->params, params, sizeof(slot->params));
    }
    return slot_id;
}

bool fault_engine_cancel(fault_engine_t *eng, uint16_t slot_id)
{
    if (slot_id >= FAULT_ENGINE_MAX_SLOTS) {
        return false;
    }
    fault_slot_t *slot = &eng->slots[slot_id];
    if (slot->state == FAULT_STATE_ACTIVE) {
        /* Deferred -- see fault_engine.h's doc. The next fault_engine_tick()
         * call emits CLEARED and finishes the reset to IDLE. */
        slot->cancel_pending = true;
        return true;
    }
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = slot_id;
    slot->state = FAULT_STATE_IDLE;
    return true;
}

bool fault_engine_fire_now(fault_engine_t *eng, uint16_t slot_id)
{
    if (slot_id >= FAULT_ENGINE_MAX_SLOTS) {
        return false;
    }
    fault_slot_t *slot = &eng->slots[slot_id];
    if (slot->state != FAULT_STATE_ARMED) {
        return false;
    }
    /* Deferred -- see fault_engine.h's doc. The next fault_engine_tick()
     * call fires the slot through the normal ARMED-slot path and emits
     * FIRED there. */
    slot->manual_fire_pending = true;
    return true;
}

/* Evaluates one trigger (either a slot's own trigger, or an
 * UNTIL_TRIGGER's nested trigger while the slot is ACTIVE -- both reuse the
 * slot's scheduling fields, which is safe because the two are never
 * evaluated in the same state: a slot's own trigger is only checked while
 * ARMED, and those fields are freshly cleared at fire time before the slot
 * goes ACTIVE and any UNTIL_TRIGGER checking begins). */
static bool evaluate_trigger(fault_engine_t *eng, fault_slot_t *slot, const fault_trigger_t *trig,
                              const fault_engine_snapshot_t *snap)
{
    switch (trig->kind) {
    case FAULT_TRIGGER_AT_SIM_TIME:
        return snap->sim_time_s >= trig->at_sim_time_s;

    case FAULT_TRIGGER_AT_ZONE_TEMP: {
        if (trig->zone >= snap->zone_count || trig->zone >= FAULT_ENGINE_MAX_ZONES) {
            return false;
        }
        if (!eng->has_prev_snapshot) {
            return false;
        }
        float prev = eng->prev_zone_temps[trig->zone];
        float cur = snap->zone_temps[trig->zone];
        if (trig->temp_edge == FAULT_TEMP_EDGE_RISING) {
            return prev < trig->temp_c && cur >= trig->temp_c;
        }
        return prev > trig->temp_c && cur <= trig->temp_c;
    }

    case FAULT_TRIGGER_ON_RELAY_EDGE: {
        if (trig->relay >= snap->relay_count || trig->relay >= FAULT_ENGINE_MAX_RELAYS) {
            return false;
        }
        if (slot->has_scheduled_fire) {
            return snap->sim_time_s >= slot->scheduled_fire_at_s;
        }
        if (!eng->has_prev_snapshot) {
            return false;
        }
        bool prev = eng->prev_relay_states[trig->relay];
        bool cur = snap->relay_states[trig->relay];
        bool edge = (trig->relay_edge == FAULT_RELAY_EDGE_CLOSE) ? (!prev && cur) : (prev && !cur);
        if (!edge) {
            return false;
        }
        slot->has_scheduled_fire = true;
        slot->scheduled_fire_at_s = snap->sim_time_s + trig->delay_s;
        return snap->sim_time_s >= slot->scheduled_fire_at_s;
    }

    case FAULT_TRIGGER_ON_EVENT: {
        if (slot->has_scheduled_fire) {
            return snap->sim_time_s >= slot->scheduled_fire_at_s;
        }
        for (uint32_t k = 0; k < snap->event_count; k++) {
            if (strncmp(snap->event_names[k], trig->event_name, FAULT_ENGINE_MAX_NAME_LEN) == 0) {
                slot->has_scheduled_fire = true;
                slot->scheduled_fire_at_s = snap->sim_time_s + trig->delay_s;
                return snap->sim_time_s >= slot->scheduled_fire_at_s;
            }
        }
        return false;
    }

    case FAULT_TRIGGER_AFTER_FAULT: {
        if (trig->after_fault_slot >= FAULT_ENGINE_MAX_SLOTS) {
            return false;
        }
        if (slot->has_scheduled_fire) {
            return snap->sim_time_s >= slot->scheduled_fire_at_s;
        }
        fault_slot_t *ref = &eng->slots[trig->after_fault_slot];
        if (ref->fire_count > slot->after_fault_last_seen_count) {
            slot->after_fault_last_seen_count = ref->fire_count;
            slot->has_scheduled_fire = true;
            slot->scheduled_fire_at_s = snap->sim_time_s + trig->delay_s;
            return snap->sim_time_s >= slot->scheduled_fire_at_s;
        }
        return false;
    }

    case FAULT_TRIGGER_RANDOM_IN: {
        if (!slot->random_picked) {
            double t0 = trig->random_t0_s;
            double t1 = trig->random_t1_s;
            float frac = xorshift32_float01(&eng->rng_state);
            slot->random_pick_s = t0 + (double)frac * (t1 - t0);
            slot->random_picked = true;
        }
        return snap->sim_time_s >= slot->random_pick_s;
    }

    case FAULT_TRIGGER_MANUAL:
    default:
        /* MANUAL only fires via fault_engine_fire_now(); tick() never
         * auto-fires it. As an UNTIL_TRIGGER nested trigger it behaves as
         * "never expires on its own" -- equivalent to PERMANENT duration,
         * documented here since it is an unusual combination. */
        return false;
    }
}

static bool check_duration_expired(fault_engine_t *eng, fault_slot_t *slot, const fault_engine_snapshot_t *snap)
{
    switch (slot->duration.kind) {
    case FAULT_DURATION_PERMANENT:
        return false;
    case FAULT_DURATION_FOR:
        return snap->sim_time_s >= slot->active_since_s + slot->duration.for_s;
    case FAULT_DURATION_UNTIL_TRIGGER:
        return evaluate_trigger(eng, slot, &slot->duration.until_trigger, snap);
    default:
        return true;
    }
}

static bool check_fire_condition(fault_engine_t *eng, fault_slot_t *slot, const fault_engine_snapshot_t *snap)
{
    /* fault_engine_fire_now() (FAULT_FIRE_NOW / TC_INJECT_FAULT's MANUAL-
     * trigger path) requested an immediate fire -- take priority over
     * whatever trigger/repeat timing would otherwise apply, so this tick
     * fires the slot through the normal path below regardless. */
    if (slot->manual_fire_pending) {
        return true;
    }
    /* After the first firing, EVERY/N_TIMES re-arm on a pure period timer
     * decoupled from the original trigger (DESIGN_NOTES.md 7.2's "EVERY t [jitter
     * j]" reads as a periodic timer, not "re-wait for the same event") --
     * this also keeps re-arm evaluation independent of whatever transient
     * per-trigger bookkeeping (has_scheduled_fire, etc.) the first firing
     * left behind. */
    if (slot->fire_count > 0 && slot->repeat.kind != FAULT_REPEAT_ONCE) {
        return snap->sim_time_s >= slot->repeat_ready_at_s;
    }
    return evaluate_trigger(eng, slot, &slot->trigger, snap);
}

static void rearm_for_repeat(fault_engine_t *eng, fault_slot_t *slot, double clear_time_s)
{
    double jitter = 0.0;
    if (slot->repeat.jitter_s > 0.0) {
        float u = xorshift32_float01(&eng->rng_state); /* [0,1) */
        jitter = ((double)u * 2.0 - 1.0) * slot->repeat.jitter_s; /* [-jitter, +jitter) */
    }
    slot->repeat_ready_at_s = clear_time_s + slot->repeat.period_s + jitter;
    slot->random_picked = false;
    slot->has_scheduled_fire = false;
    slot->state = FAULT_STATE_ARMED;
}

static void update_prev_snapshot(fault_engine_t *eng, const fault_engine_snapshot_t *snap)
{
    uint8_t zc = snap->zone_count;
    if (zc > FAULT_ENGINE_MAX_ZONES) zc = FAULT_ENGINE_MAX_ZONES;
    for (uint8_t i = 0; i < zc; i++) {
        eng->prev_zone_temps[i] = snap->zone_temps[i];
    }
    uint8_t rc = snap->relay_count;
    if (rc > FAULT_ENGINE_MAX_RELAYS) rc = FAULT_ENGINE_MAX_RELAYS;
    for (uint8_t i = 0; i < rc; i++) {
        eng->prev_relay_states[i] = snap->relay_states[i];
    }
    eng->has_prev_snapshot = true;
}

size_t fault_engine_tick(fault_engine_t *eng, const fault_engine_snapshot_t *snap,
                          fault_event_t *out, size_t max_events)
{
    size_t n = 0;

    for (uint16_t i = 0; i < FAULT_ENGINE_MAX_SLOTS; i++) {
        fault_slot_t *slot = &eng->slots[i];

        if (slot->state == FAULT_STATE_ARMED) {
            if (check_fire_condition(eng, slot, snap)) {
                slot->state = FAULT_STATE_ACTIVE;
                slot->active_since_s = snap->sim_time_s;
                slot->fire_count++;
                slot->has_scheduled_fire = false;
                slot->manual_fire_pending = false;
                if (n < max_events) {
                    out[n].kind = FAULT_EVENT_FIRED;
                    out[n].slot_id = slot->slot_id;
                    out[n].sim_time_s = snap->sim_time_s;
                    out[n].fire_count = slot->fire_count;
                    n++;
                }
            }
        } else if (slot->state == FAULT_STATE_ACTIVE) {
            /* An operator-requested cancel (fault_engine_cancel() while
             * ACTIVE) takes priority over duration/repeat -- it always
             * clears the slot outright (IDLE), never rearms it, regardless
             * of what check_duration_expired() would have said this tick. */
            if (slot->cancel_pending) {
                if (n < max_events) {
                    out[n].kind = FAULT_EVENT_CLEARED;
                    out[n].slot_id = slot->slot_id;
                    out[n].sim_time_s = snap->sim_time_s;
                    out[n].fire_count = slot->fire_count;
                    n++;
                }
                uint16_t slot_id = slot->slot_id;
                memset(slot, 0, sizeof(*slot));
                slot->slot_id = slot_id;
                slot->state = FAULT_STATE_IDLE;
            } else if (check_duration_expired(eng, slot, snap)) {
                if (n < max_events) {
                    out[n].kind = FAULT_EVENT_CLEARED;
                    out[n].slot_id = slot->slot_id;
                    out[n].sim_time_s = snap->sim_time_s;
                    out[n].fire_count = slot->fire_count;
                    n++;
                }
                bool exhausted = (slot->repeat.kind == FAULT_REPEAT_ONCE) ||
                                  (slot->repeat.kind == FAULT_REPEAT_N_TIMES && slot->fire_count >= slot->repeat.n);
                if (exhausted) {
                    slot->state = FAULT_STATE_EXPIRED;
                } else {
                    rearm_for_repeat(eng, slot, snap->sim_time_s);
                }
            }
        }
        /* IDLE, EXPIRED: nothing to evaluate. */
    }

    update_prev_snapshot(eng, snap);
    return n;
}
