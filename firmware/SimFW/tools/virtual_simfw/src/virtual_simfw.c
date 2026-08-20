// virtual_simfw.c -- a host-side "virtual SimFW": the REAL simulation logic
// (firmware/SimFW/src/sim/*.c, compiled unmodified) plus a thin harness that
// owns a sim clock, ticks the thermal model + fault engine, and speaks the
// REAL benchproto wire protocol (firmware/CommonFW/src/benchproto_*.c,
// compiled unmodified) + SimFW's own PROTOCOL.md command layouts, over a
// plain TCP socket instead of USB CDC.
//
// This is NOT a reimplementation of SimFW's firmware. The thermal model,
// MAX31856 register machine, fault engine, sine synth, and TC fault state
// are the exact same .c files firmware/SimFW/src/tasks/*.c calls -- compiled
// here for the host, not reinvented. What THIS file replaces is the
// FreeRTOS task scaffolding (sim_engine.c/fault_sched.c/i2c_owner.c/
// wave_owner.c/spi_emu_a.c/spi_emu_b.c/cmd_task.c/usb_owner.c/telemetry.c),
// all of which are FreeRTOS-task shaped and cannot run unmodified on a PC.
// The *logic* of sim_engine.c's tick order and fault_sched.c's
// recompute-overrides strategy is ported here near-verbatim (single
// threaded, so FreeRTOS queues/mutexes are simply unnecessary -- there is
// only one thread of execution, so "queue-then-apply-next-tick" collapses to
// "apply directly"); every command's byte layout is cross-referenced to
// firmware/SimFW/docs/PROTOCOL.md by section number so it cannot silently
// drift from that spec.
//
// KNOWN, DOCUMENTED DEVIATIONS from real SimFW firmware (see README.md in
// this directory for the full list):
//   - SYS RESET_SIM / SET_TIMESCALE / SET_SEED are RESERVED (stub,
//     ERR_NOT_IMPL) in real SimFW firmware today (PROTOCOL.md sec 4) -- this
//     virtual device implements them for real, since kilnsim's scenario
//     runner needs a way to set the seed/timescale before a run and no
//     other path exists yet. This is a virtual-device-only extension, not a
//     claim about real firmware behavior.
//   - No real SPI bytes ever flow (there is no DUT to drive CS/SCLK), so
//     TC_GET_REGS/TC_GET_MASTER_CONFIG's spi transaction/error/underrun
//     counters are always 0 and `configured` is always false -- this is the
//     honest, correct answer for "a channel no master has ever touched",
//     not a bug.
//   - Relay sense (K1/K2/K3/K5/K4) and the ESP-driven `Fault` line default
//     open/deasserted and stay there unless something explicitly reports
//     otherwise -- a real DUT has no wire to do that (real hardware only
//     ever senses a contact, PLAN.md sec 3.4), so RELAY_GET_STATES/
//     RELAY_GET_EDGES stay at their defaults against a real DUT. A
//     *virtual* DUT, with no physical relay coil to close in the first
//     place, can report a sensed state through a new virtual-only command,
//     SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE (see the RELAY-group section
//     below and README.md's "Known, virtual-only extensions") -- standing
//     in for the missing physical sense wire, never a claim that real
//     hardware works this way. Power-path faults (WELDED_RELAY etc.) act
//     at the duty-override level exactly as real fault_sched.c does, so
//     they still correctly show up in CT current independent of relay
//     sense either way.
//   - This file now serves MULTIPLE concurrent TCP clients (up to
//     SIMFW_MAX_CLIENTS), each with its own benchproto_link_t and EVT-ring
//     read cursor -- see the block comment above client_t's definition.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "benchproto/benchproto_frame.h"
#include "benchproto/benchproto_link.h"
#include "benchproto/benchproto_version.h"

#include "thermal_model.h"
#include "max31856_regs.h"
#include "fault_engine.h"
#include "tc_fault_state.h"
#include "sine_synth.h"
#include "sim_snapshot.h"

#include "cmd_ids.h"
#include "version.h"

#pragma comment(lib, "ws2_32.lib")

// ===========================================================================
// Fault-type / target catalog. Not defined in cmd_ids.h (that header only
// carries wire-level ids) -- fault_sched_fault_type_t (firmware/SimFW/src/
// tasks/fault_sched.h) is the real enum, reproduced verbatim here since
// fault_sched.h itself pulls in FreeRTOS-shaped fault_sched_start()/
// fault_sched_tick() declarations this harness does not use. The numeric
// values below are copied 1:1 from fault_sched.h's enum -- keep in sync if
// that header ever adds/reorders a value (it hasn't since M-F landed).
// ===========================================================================
typedef enum {
    FT_TC_DISCONNECTED = 0,
    FT_TC_NOISE,
    FT_TC_STUCK,
    FT_TC_DEAD_IC,
    FT_TC_FLAKY_SPI,
    FT_TC_SPURIOUS_FAULT_PIN,
    FT_TC_SHORTED,
    FT_TC_DRIFT,
    FT_TC_CJ_FAULT,
    FT_MAIN_SAFETY_DISAGREE,
    FT_WELDED_RELAY,
    FT_STUCK_OPEN_RELAY,
    FT_BROKEN_ELEMENT,
    FT_PARTIAL_ELEMENT,
    FT_HALF_WAVE_SSR,
    FT_PHASE_LOSS,
    FT_WELDED_K4_CURRENT_PERSIST,
    FT_ESTOP,
    FT_RUNAWAY_ZONE,
    FT_AMBIENT_SHIFT,
    FT_THERMAL_MASS_SURPRISE,
    FT_TC_LAG_STRESS,
    FT_DUT_POWER_CUT,
} fault_type_t;

#define CT_NUM_CHANNELS 3u
#define TC_NUM_CHANNELS TC_FAULT_CHANNEL_COUNT /* 4 */
#define TICK_PERIOD_MS 100.0f /* 10 Hz, PLAN.md 4.1's sim_engine tick */
#define TELEMETRY_RATE_HZ 2.0f /* PLAN.md 5.3 default */
#define EDGE_LOG_CAPACITY 64u

// ===========================================================================
// CT channel state (wave_owner.h-shaped, minus the real PWM/DMA hardware --
// see wave_owner.h's own TODO(M-D calibration) note: amps is treated
// directly as a 0..1 PWM-scale fraction until real calibration exists, and
// this harness inherits that same placeholder, unmodified).
// ===========================================================================
typedef struct {
    uint8_t mode; // ct_wave_mode_t: 0 MODEL, 1 MANUAL
    float amps;
    float manual_amps_pending;
    float phase_deg;
    float dc_offset;
    float clip_fraction;
    bool dropout_half_cycle;
    bool dropout_negative_half;
    bool apply_immediately;
    float last_pwm_scale;
    bool valid;
} ct_channel_t;

typedef struct {
    uint32_t seq;
    uint8_t signal; // i2c_owner_signal_t: 0 K1,1 K2,2 K3,3 K5,4 K4,5 FAULT_LINE
    bool level;
    uint64_t time_us;
} edge_entry_t;

// ===========================================================================
// The virtual device. Single-threaded: everything below is touched only
// from main()'s own loop, so none of sim_engine.c's seqlock/mutex machinery
// is needed -- there is nowhere for a torn read to come from.
// ===========================================================================
typedef struct {
    thermal_model_params_t params;
    thermal_model_state_t state;
    thermal_preset_id_t current_preset;
    uint32_t timescale_x100;
    uint64_t sim_time_us;
    uint32_t seed;

    bool zone_manual[THERMAL_MODEL_MAX_ZONES];
    float zone_manual_temp_c[THERMAL_MODEL_MAX_ZONES];

    float safety_weight[THERMAL_MODEL_MAX_ZONES];
    float safety_lag_s;
    float safety_tc_state_c;
    bool safety_manual;
    float safety_manual_temp_c;
    bool safety_fault_override_active;
    float safety_fault_offset_c;
    float safety_fault_gain;

    bool zone_thermal_override_active[THERMAL_MODEL_MAX_ZONES];
    float zone_thermal_override_C[THERMAL_MODEL_MAX_ZONES];
    float zone_thermal_override_k_loss[THERMAL_MODEL_MAX_ZONES];
    bool zone_tc_lag_override_active[THERMAL_MODEL_MAX_ZONES];
    float zone_tc_lag_override_value[THERMAL_MODEL_MAX_ZONES];
    bool zone_duty_override_active[THERMAL_MODEL_MAX_ZONES];
    float zone_duty_override_value[THERMAL_MODEL_MAX_ZONES];
    bool zone_health_override_active[THERMAL_MODEL_MAX_ZONES];
    float zone_health_override_value[THERMAL_MODEL_MAX_ZONES];

    // relay sense / discrete I/O -- never driven by a DUT in this harness
    // (see file header). k1..k5/k4 default false (open/not-sensed-closed);
    // estop_open/dut_power_on have real defaults (closed loop, powered) so
    // IO_ESTOP_GET/DUT_POWER_GET have a sane answer before any command.
    bool k1, k2, k3, k5, k4;
    bool fault_line_asserted;
    bool estop_open;
    bool dut_power_on;
    uint64_t relay_sample_time_us;
    uint16_t relay_mask_prev;

    edge_entry_t edges[EDGE_LOG_CAPACITY];
    uint32_t edge_write_idx; // next slot to write, wraps
    uint32_t edge_count;     // total ever written (monotonic, == next seq)

    fault_engine_t fault_engine;
    double last_sim_time_s;

    bool ct_phase_loss_was_active[CT_NUM_CHANNELS];
    bool ct_half_wave_was_active[CT_NUM_CHANNELS];
    bool ct_k4_weld_was_active[CT_NUM_CHANNELS];
    ct_channel_t ct[CT_NUM_CHANNELS];

    max31856_channel_t tc[TC_NUM_CHANNELS];

    // sim_snapshot.h-shaped published state + event ring, single-threaded
    // (no seqlock needed -- see block comment above).
    uint64_t snap_sim_time_us;
    uint32_t snap_zone_count;
    float snap_T_true[THERMAL_MODEL_MAX_ZONES];
    float snap_T_tc[THERMAL_MODEL_MAX_ZONES];
    float snap_T_safety[THERMAL_MODEL_MAX_ZONES];
    float snap_I_amps[THERMAL_MODEL_MAX_ZONES];
    uint16_t snap_relay_mask;
    bool snap_estop_open;
    bool snap_dut_power_on;
    bool has_published;

    sim_event_t ring[SIM_EVENT_RING_SIZE];
    uint32_t ring_next_seq; // next seq to be written (global; each client
                             // drains it through its OWN cursor -- see
                             // client_t::telemetry_next_evt_seq below)

    uint32_t active_fault_count;
} device_t;

static device_t g_dev;

// ===========================================================================
// Small helpers ported from thermal_model presets' consumer, fault_sched.c's
// severity doctrine, etc. See PROTOCOL.md section references inline.
// ===========================================================================

static void ring_push(device_t *d, sim_event_type_t type, uint8_t a, uint8_t b, float f0)
{
    uint32_t seq = d->ring_next_seq++;
    sim_event_t *slot = &d->ring[seq % SIM_EVENT_RING_SIZE];
    slot->seq = seq;
    slot->sim_time_us = d->sim_time_us;
    slot->type = type;
    slot->a = a;
    slot->b = b;
    slot->f0 = f0;
}

static void edge_push(device_t *d, uint8_t signal, bool level)
{
    uint32_t seq = d->edge_count++;
    edge_entry_t *slot = &d->edges[seq % EDGE_LOG_CAPACITY];
    slot->seq = seq;
    slot->signal = signal;
    slot->level = level;
    slot->time_us = d->sim_time_us;
}

// --- fault_sched.c's target_kind_of(), ported verbatim (see that file) -----
typedef enum { TK_TC, TK_ZONE, TK_CT, TK_SYSTEM, TK_INVALID } target_kind_t;

static target_kind_t target_kind_of(fault_type_t t)
{
    switch (t) {
    case FT_TC_DISCONNECTED:
    case FT_TC_NOISE:
    case FT_TC_STUCK:
    case FT_TC_DEAD_IC:
    case FT_TC_FLAKY_SPI:
    case FT_TC_SPURIOUS_FAULT_PIN:
    case FT_TC_SHORTED:
    case FT_TC_DRIFT:
    case FT_TC_CJ_FAULT:
        return TK_TC;
    case FT_WELDED_RELAY:
    case FT_STUCK_OPEN_RELAY:
    case FT_BROKEN_ELEMENT:
    case FT_PARTIAL_ELEMENT:
    case FT_RUNAWAY_ZONE:
    case FT_THERMAL_MASS_SURPRISE:
    case FT_TC_LAG_STRESS:
        return TK_ZONE;
    case FT_HALF_WAVE_SSR:
    case FT_PHASE_LOSS:
    case FT_WELDED_K4_CURRENT_PERSIST:
        return TK_CT;
    case FT_ESTOP:
    case FT_AMBIENT_SHIFT:
    case FT_MAIN_SAFETY_DISAGREE:
    case FT_DUT_POWER_CUT:
        return TK_SYSTEM;
    }
    return TK_INVALID;
}

// --- fault_sched.c's recompute_overrides_locked(), ported: single thread, --
// --- no mutex, direct field writes instead of tc_fault_state_write()/     --
// --- sim_engine_set_*_override() queued calls (they collapse to the same --
// --- thing here). tc_fault_state_write/_clear ARE still called for real, --
// --- reusing the exact same shared contract spi_emu_a/b read in firmware. -
static void recompute_overrides(device_t *d)
{
    tc_fault_override_t tc_ovr[TC_NUM_CHANNELS];
    bool tc_active[TC_NUM_CHANNELS];
    memset(tc_ovr, 0, sizeof(tc_ovr));
    memset(tc_active, 0, sizeof(tc_active));

    bool duty_force1[THERMAL_MODEL_MAX_ZONES] = {0};
    bool duty_force0[THERMAL_MODEL_MAX_ZONES] = {0};
    bool health_active[THERMAL_MODEL_MAX_ZONES] = {0};
    float health_value[THERMAL_MODEL_MAX_ZONES] = {0};

    bool ct_half_wave[CT_NUM_CHANNELS] = {0};
    bool ct_half_wave_negative[CT_NUM_CHANNELS] = {0};
    bool ct_phase_loss[CT_NUM_CHANNELS] = {0};
    bool ct_k4_weld[CT_NUM_CHANNELS] = {0};
    float ct_k4_weld_amps[CT_NUM_CHANNELS] = {0};

    bool thermal_active[THERMAL_MODEL_MAX_ZONES] = {0};
    float thermal_C[THERMAL_MODEL_MAX_ZONES] = {0};
    float thermal_k_loss[THERMAL_MODEL_MAX_ZONES] = {0};
    bool tc_lag_active[THERMAL_MODEL_MAX_ZONES] = {0};
    float tc_lag_value[THERMAL_MODEL_MAX_ZONES] = {0};

    bool safety_tc_active = false;
    float safety_tc_offset = 0.0f;
    float safety_tc_gain = 1.0f;

    for (uint16_t slot_id = 0; slot_id < FAULT_ENGINE_MAX_SLOTS; slot_id++) {
        const fault_slot_t *slot = &d->fault_engine.slots[slot_id];
        if (slot->state != FAULT_STATE_ACTIVE) continue;

        fault_type_t ft = (fault_type_t)slot->fault_type;
        uint16_t target = slot->target;

        switch (ft) {
        case FT_TC_DISCONNECTED:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].force_sr_bits |= MAX31856_FAULT_OPEN; }
            break;
        case FT_TC_NOISE:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; if (slot->params[0] > tc_ovr[target].corruption.noise_sigma_c) tc_ovr[target].corruption.noise_sigma_c = slot->params[0]; }
            break;
        case FT_TC_STUCK:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].corruption.stuck_ltcb = true; }
            break;
        case FT_TC_DEAD_IC:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].corruption.dead_mode = (max31856_dead_mode_t)(int)slot->params[0]; }
            break;
        case FT_TC_FLAKY_SPI:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; if (slot->params[0] > tc_ovr[target].corruption.bit_error_rate) tc_ovr[target].corruption.bit_error_rate = slot->params[0]; }
            break;
        case FT_TC_SPURIOUS_FAULT_PIN:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].corruption.spurious_fault_pin = true; }
            break;
        case FT_TC_SHORTED:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].corruption.shorted = true; }
            break;
        case FT_TC_DRIFT:
            if (target < TC_NUM_CHANNELS) {
                tc_active[target] = true;
                double elapsed = d->last_sim_time_s - slot->active_since_s;
                if (elapsed < 0.0) elapsed = 0.0;
                tc_ovr[target].corruption.drift_offset_c = slot->params[0] * (float)elapsed;
            }
            break;
        case FT_TC_CJ_FAULT:
            if (target < TC_NUM_CHANNELS) { tc_active[target] = true; tc_ovr[target].corruption.cj_fault_offset_c = slot->params[0]; }
            break;
        case FT_HALF_WAVE_SSR:
            if (target < CT_NUM_CHANNELS) { ct_half_wave[target] = true; ct_half_wave_negative[target] = (slot->params[0] != 0.0f); }
            break;
        case FT_PHASE_LOSS:
            if (target < CT_NUM_CHANNELS) ct_phase_loss[target] = true;
            break;
        case FT_WELDED_RELAY:
        case FT_RUNAWAY_ZONE:
            if (target < THERMAL_MODEL_MAX_ZONES) duty_force1[target] = true;
            break;
        case FT_STUCK_OPEN_RELAY:
            if (target < THERMAL_MODEL_MAX_ZONES) duty_force0[target] = true;
            break;
        case FT_BROKEN_ELEMENT:
            if (target < THERMAL_MODEL_MAX_ZONES) { health_active[target] = true; health_value[target] = 0.0f; }
            break;
        case FT_PARTIAL_ELEMENT:
            if (target < THERMAL_MODEL_MAX_ZONES && !health_active[target]) { health_active[target] = true; health_value[target] = slot->params[0]; }
            break;
        case FT_WELDED_K4_CURRENT_PERSIST:
            if (target < CT_NUM_CHANNELS) { ct_k4_weld[target] = true; if (slot->params[0] > ct_k4_weld_amps[target]) ct_k4_weld_amps[target] = slot->params[0]; }
            break;
        case FT_THERMAL_MASS_SURPRISE:
            if (target < THERMAL_MODEL_MAX_ZONES) { thermal_active[target] = true; thermal_C[target] = slot->params[0]; thermal_k_loss[target] = slot->params[1]; }
            break;
        case FT_TC_LAG_STRESS:
            if (target < THERMAL_MODEL_MAX_ZONES) { tc_lag_active[target] = true; tc_lag_value[target] = slot->params[0]; }
            break;
        case FT_MAIN_SAFETY_DISAGREE:
            safety_tc_active = true;
            safety_tc_offset += slot->params[0];
            if (slot->params[1] != 0.0f) safety_tc_gain = slot->params[1];
            break;
        case FT_ESTOP:
        case FT_AMBIENT_SHIFT:
        case FT_DUT_POWER_CUT:
            break; // edge-driven, see apply_edge_effects()
        }
    }

    for (unsigned ch = 0; ch < TC_NUM_CHANNELS; ch++) {
        if (tc_active[ch]) tc_fault_state_write((tc_fault_channel_t)ch, &tc_ovr[ch]);
        else tc_fault_state_clear((tc_fault_channel_t)ch);
    }

    for (unsigned z = 0; z < THERMAL_MODEL_MAX_ZONES; z++) {
        if (duty_force0[z]) { d->zone_duty_override_active[z] = true; d->zone_duty_override_value[z] = 0.0f; }
        else if (duty_force1[z]) { d->zone_duty_override_active[z] = true; d->zone_duty_override_value[z] = 1.0f; }
        else { d->zone_duty_override_active[z] = false; d->zone_duty_override_value[z] = 0.0f; }

        d->zone_health_override_active[z] = health_active[z];
        d->zone_health_override_value[z] = health_value[z];
        d->zone_thermal_override_active[z] = thermal_active[z];
        d->zone_thermal_override_C[z] = thermal_C[z];
        d->zone_thermal_override_k_loss[z] = thermal_k_loss[z];
        d->zone_tc_lag_override_active[z] = tc_lag_active[z];
        d->zone_tc_lag_override_value[z] = tc_lag_value[z];
    }

    d->safety_fault_override_active = safety_tc_active;
    d->safety_fault_offset_c = safety_tc_offset;
    d->safety_fault_gain = safety_tc_active ? safety_tc_gain : 1.0f;

    for (unsigned c = 0; c < CT_NUM_CHANNELS; c++) {
        if (ct_phase_loss[c]) {
            if (!d->ct_phase_loss_was_active[c]) { d->ct[c].mode = 1; d->ct[c].manual_amps_pending = 0.0f; }
        } else if (d->ct_phase_loss_was_active[c]) {
            d->ct[c].mode = 0;
        }
        d->ct_phase_loss_was_active[c] = ct_phase_loss[c];

        bool half_wave_now = ct_half_wave[c] && !ct_phase_loss[c];
        if (half_wave_now) {
            if (!d->ct_half_wave_was_active[c]) {
                d->ct[c].dropout_half_cycle = true;
                d->ct[c].dropout_negative_half = ct_half_wave_negative[c];
            }
        } else if (d->ct_half_wave_was_active[c]) {
            d->ct[c].dropout_half_cycle = false;
            d->ct[c].dropout_negative_half = false;
        }
        d->ct_half_wave_was_active[c] = half_wave_now;

        bool k4_weld_now = ct_k4_weld[c] && !ct_phase_loss[c];
        if (k4_weld_now) {
            if (!d->ct_k4_weld_was_active[c]) { d->ct[c].mode = 1; d->ct[c].manual_amps_pending = ct_k4_weld_amps[c]; }
        } else if (d->ct_k4_weld_was_active[c]) {
            d->ct[c].mode = 0;
        }
        d->ct_k4_weld_was_active[c] = k4_weld_now;
    }
}

static void apply_edge_effects(device_t *d, const fault_event_t *events, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const fault_event_t *ev = &events[i];
        if (ev->slot_id >= FAULT_ENGINE_MAX_SLOTS) continue;
        const fault_slot_t *slot = &d->fault_engine.slots[ev->slot_id];
        fault_type_t ft = (fault_type_t)slot->fault_type;

        if (ft == FT_ESTOP) {
            d->estop_open = (ev->kind == FAULT_EVENT_FIRED);
        } else if (ft == FT_AMBIENT_SHIFT && ev->kind == FAULT_EVENT_FIRED) {
            d->params.T_ambient = slot->params[0];
        } else if (ft == FT_DUT_POWER_CUT) {
            d->dut_power_on = (ev->kind != FAULT_EVENT_FIRED);
        }
    }
}

// --- sim_engine.c's sim_engine_tick(), ported: single tick, no queued ------
// --- MODEL commands (applied directly by the cmd handlers instead) --------
static void device_tick(device_t *d)
{
    uint16_t relay_mask = 0;
    if (d->k1) relay_mask |= (1u << SIM_RELAY_BIT_K1);
    if (d->k2) relay_mask |= (1u << SIM_RELAY_BIT_K2);
    if (d->k3) relay_mask |= (1u << SIM_RELAY_BIT_K3);
    if (d->k5) relay_mask |= (1u << SIM_RELAY_BIT_K5);
    if (d->k4) relay_mask |= (1u << SIM_RELAY_BIT_K4);

    bool relay_bools[FAULT_ENGINE_MAX_RELAYS] = {0};
    relay_bools[SIM_RELAY_BIT_K1] = d->k1;
    relay_bools[SIM_RELAY_BIT_K2] = d->k2;
    relay_bools[SIM_RELAY_BIT_K3] = d->k3;
    relay_bools[SIM_RELAY_BIT_K5] = d->k5;
    relay_bools[SIM_RELAY_BIT_K4] = d->k4;

    fault_engine_snapshot_t fsnap;
    fsnap.sim_time_s = (double)d->sim_time_us / 1.0e6;
    fsnap.zone_temps = d->state.T_zone;
    fsnap.zone_count = d->params.zone_count;
    fsnap.relay_states = relay_bools;
    fsnap.relay_count = SIM_RELAY_BIT_COUNT;
    fsnap.event_names = NULL;
    fsnap.event_count = 0;

    fault_event_t fault_events[FAULT_ENGINE_MAX_SLOTS * 2u];
    d->last_sim_time_s = fsnap.sim_time_s;
    size_t fault_event_count = fault_engine_tick(&d->fault_engine, &fsnap, fault_events,
                                                  sizeof(fault_events) / sizeof(fault_events[0]));
    apply_edge_effects(d, fault_events, fault_event_count);
    recompute_overrides(d);

    float duty[THERMAL_MODEL_MAX_ZONES] = {0};
    static const sim_relay_bit_t zone_relay_bit[3] = {SIM_RELAY_BIT_K1, SIM_RELAY_BIT_K2, SIM_RELAY_BIT_K3};
    for (uint8_t z = 0; z < d->params.zone_count && z < 3u; z++) {
        duty[z] = (relay_mask & (1u << zone_relay_bit[z])) ? 1.0f : 0.0f;
    }
    for (uint8_t z = 0; z < d->params.zone_count; z++) {
        if (d->zone_duty_override_active[z]) duty[z] = d->zone_duty_override_value[z];
    }

    thermal_model_params_t eff = d->params;
    for (uint8_t z = 0; z < eff.zone_count; z++) {
        if (d->zone_health_override_active[z]) eff.zones[z].element_health = d->zone_health_override_value[z];
        if (d->zone_thermal_override_active[z]) {
            if (d->zone_thermal_override_C[z] > 0.0f) eff.zones[z].C = d->zone_thermal_override_C[z];
            eff.zones[z].k_loss = d->zone_thermal_override_k_loss[z];
        }
        if (d->zone_tc_lag_override_active[z]) eff.zones[z].tc_lag_s = d->zone_tc_lag_override_value[z];
    }

    uint32_t scale_int = d->timescale_x100 / 100u;
    if (scale_int == 0u) scale_int = 1u;
    float dt_s = (TICK_PERIOD_MS / 1000.0f) * ((float)d->timescale_x100 / 100.0f);

    thermal_model_tick(&d->state, &eff, duty, dt_s, scale_int);

    for (uint8_t z = 0; z < eff.zone_count; z++) {
        if (d->zone_manual[z]) {
            d->state.T_zone[z] = d->zone_manual_temp_c[z];
            d->state.T_tc[z] = d->zone_manual_temp_c[z];
        }
    }

    d->sim_time_us += (uint64_t)(dt_s * 1.0e6f);

    float safety_blend_target = 0.0f;
    for (uint8_t z = 0; z < eff.zone_count; z++) safety_blend_target += d->safety_weight[z] * d->state.T_zone[z];
    if (d->safety_lag_s > 0.0f) {
        float dT_dt = (safety_blend_target - d->safety_tc_state_c) / d->safety_lag_s;
        d->safety_tc_state_c += dT_dt * dt_s;
    } else {
        d->safety_tc_state_c = safety_blend_target;
    }
    float safety_reported_c = d->safety_tc_state_c;
    if (d->safety_fault_override_active) safety_reported_c = safety_reported_c * d->safety_fault_gain + d->safety_fault_offset_c;
    if (d->safety_manual) safety_reported_c = d->safety_manual_temp_c;

    float current_a[THERMAL_MODEL_MAX_ZONES] = {0};
    for (uint8_t z = 0; z < eff.zone_count; z++) {
        float r = eff.zones[z].R_element;
        if (r > 0.0f) current_a[z] = (eff.V_mains / r) * duty[z] * eff.zones[z].element_health;
    }

    d->snap_sim_time_us = d->sim_time_us;
    d->snap_zone_count = eff.zone_count;
    for (uint8_t z = 0; z < eff.zone_count; z++) {
        d->snap_T_true[z] = d->state.T_zone[z];
        d->snap_T_tc[z] = d->state.T_tc[z];
        d->snap_T_safety[z] = safety_reported_c;
        d->snap_I_amps[z] = current_a[z];
    }
    d->snap_relay_mask = relay_mask;
    d->snap_estop_open = d->estop_open;
    d->snap_dut_power_on = d->dut_power_on;
    d->has_published = true;

    // CT channel amps: MODEL mode tracks current_a[c] (wave_owner.h: "MODEL
    // ... tracks the thermal model's current_a for this channel every
    // tick"); MANUAL holds the last-commanded/fault-forced value.
    for (uint8_t c = 0; c < CT_NUM_CHANNELS; c++) {
        if (d->ct[c].mode == 0) {
            d->ct[c].amps = (c < eff.zone_count) ? current_a[c] : 0.0f;
        } else {
            d->ct[c].amps = d->ct[c].manual_amps_pending;
        }
        float scale = d->ct[c].amps;
        if (scale < 0.0f) scale = 0.0f;
        if (scale > 1.0f) scale = 1.0f;
        d->ct[c].last_pwm_scale = scale;
        d->ct[c].valid = true;
    }

    uint16_t changed = relay_mask ^ d->relay_mask_prev;
    if (changed != 0u) {
        for (uint8_t bit = 0; bit < SIM_RELAY_BIT_COUNT; bit++) {
            if (changed & (1u << bit)) {
                bool now_closed = (relay_mask & (1u << bit)) != 0u;
                ring_push(d, SIM_EVENT_RELAY_EDGE, bit, now_closed ? 1u : 0u, 0.0f);
                edge_push(d, bit, now_closed);
            }
        }
    }
    d->relay_mask_prev = relay_mask;
    d->relay_sample_time_us = d->sim_time_us;

    for (size_t i = 0; i < fault_event_count; i++) {
        sim_event_type_t type = (fault_events[i].kind == FAULT_EVENT_FIRED) ? SIM_EVENT_FAULT_FIRED : SIM_EVENT_FAULT_CLEARED;
        ring_push(d, type, (uint8_t)fault_events[i].slot_id, 0u, 0.0f);
    }

    // active_fault_count for telemetry (PROTOCOL.md sec 6).
    uint32_t active = 0;
    for (uint16_t s = 0; s < FAULT_ENGINE_MAX_SLOTS; s++) {
        if (d->fault_engine.slots[s].state == FAULT_STATE_ACTIVE) active++;
    }
    d->active_fault_count = active;

    // Advance TC channels' emulated conversions from the (now current)
    // thermal-model truth, honoring whatever tc_fault_state.h override is
    // active -- exactly what spi_emu_a/b's write-back loop does before
    // calling max31856_regs_advance_conversion() (tc_fault_state.h's own
    // doc comment on the expected call site).
    for (unsigned ch = 0; ch < TC_NUM_CHANNELS; ch++) {
        tc_fault_override_t ovr = {0};
        (void)tc_fault_state_read((tc_fault_channel_t)ch, &ovr);
        d->tc[ch].corruption = ovr.corruption;
        float true_tc_c, true_cj_c;
        if (ch < TC_FAULT_CHANNEL_SAFETY) {
            true_tc_c = (ch < eff.zone_count) ? d->state.T_tc[ch] : 0.0f;
        } else {
            true_tc_c = safety_reported_c;
        }
        true_cj_c = 25.0f; // fixed simulated cold-junction ambient (PLAN.md 3.2:
                            // "default: slow ambient drift" -- a constant is a
                            // documented simplification; nothing in the
                            // required scenarios needs CJ drift over time)
        max31856_regs_advance_conversion(&d->tc[ch], true_tc_c, true_cj_c);
        d->tc[ch].regs[MAX31856_REG_SR] |= (ovr.force_sr_bits & (MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV));
    }
}

// ===========================================================================
// benchproto wire glue -- socket framing + dispatch. See CommonFW/docs/
// BENCHPROTO.md and firmware/SimFW/docs/PROTOCOL.md.
// ===========================================================================
#define SIMFW_HOST_DEVICE 0u
#define SIMFW_TARGET_DEVICE 1u

typedef struct {
    uint8_t buf[8192];
    size_t len;
} rx_buf_t;

// ===========================================================================
// Multi-client support. Real SimFW hardware is a single USB CDC endpoint --
// there is exactly one wire, so real firmware's usb_owner.c never had a
// "which client" question to answer. This harness stands in for that one
// wire over TCP, but Task 2 of this pass's brief requires it to serve TWO
// independent PC-side processes at once (kilnsim's own CLI/MCP surface and
// virtual_dut's run_dut_scenarios.py), neither of which is willing to be a
// relay for the other. Each accepted TCP connection gets its own
// benchproto_link_t (dedup/task-registration state is a per-connection
// concept -- BENCHPROTO.md sec 4/6 -- so two clients must not share one),
// its own RX reassembly buffer, and its own EVT-ring read cursor
// (telemetry_next_evt_seq) so `report.py`'s per-client sequence-gap check
// stays honest: seq numbers are global (assigned once, in ring_push(), the
// single source of truth every client reads from), but each client is owed
// its OWN unbroken 0,1,2,... sub-sequence of frames actually delivered to
// IT, not a shared cursor that would silently skip frames for whichever
// client didn't happen to be connected when they were written. A simpler
// design (one global cursor, replicated to whichever clients are present)
// was considered and rejected: it would make two clients that connect at
// different times see different gaps in the SAME seq numbers, which is
// exactly the kind of thing report.py exists to catch -- see this file's
// header and README.md for the fuller writeup.
// ===========================================================================
#define SIMFW_MAX_CLIENTS 4u

typedef struct {
    bool in_use;
    SOCKET sock;
    rx_buf_t rx;
    benchproto_link_t link;
    uint32_t telemetry_next_evt_seq; // next ring seq owed to THIS client
} client_t;

static client_t g_clients[SIMFW_MAX_CLIENTS];

static void client_register_tasks(client_t *c)
{
    benchproto_link_init(&c->link, SIMFW_TARGET_DEVICE);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_SYS);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_MODEL);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_TC);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_CT);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_RELAY);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_IO);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_FAULT);
    benchproto_link_register_task(&c->link, SIMFW_TASK_ID_EVT);
}

// Called whenever the event ring's write sequence resets to 0 (SYS
// RESET_SIM, MODEL LOAD_PRESET) so every already-connected client's read
// cursor resets in lockstep with it -- otherwise a client's cursor (e.g.
// 500) would sit above the freshly-zeroed ring_next_seq and drain_events()
// would simply stop delivering to it until the ring counter climbed back
// past its old value.
static void reset_client_evt_cursors(void)
{
    for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use) g_clients[i].telemetry_next_evt_seq = 0;
    }
}

static bool socket_send_all(SOCKET s, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR) return false;
        sent += (size_t)n;
    }
    return true;
}

static void client_drop(client_t *c)
{
    if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    c->sock = INVALID_SOCKET;
    c->in_use = false;
}

static void send_frame_to(client_t *c, const benchproto_frame_t *frame)
{
    if (c == NULL || !c->in_use || c->sock == INVALID_SOCKET) return;
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t st;
    size_t raw_len = benchproto_frame_encode_raw(frame, raw, sizeof(raw), &st);
    if (raw_len == 0) return;
    uint8_t stuffed[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t stuffed_len = benchproto_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0) return;
    if (!socket_send_all(c->sock, stuffed, stuffed_len)) {
        client_drop(c);
    }
}

// Broadcast helper (telemetry/EVT): same frame content, delivered to every
// currently-connected client, each over its own socket.
static void send_frame_broadcast(const benchproto_frame_t *frame)
{
    for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use) send_frame_to(&g_clients[i], frame);
    }
}

// --- Reply/arg helpers, mirroring cmd_task.c's reply_writer_t/arg_reader_t -
typedef struct { uint8_t *buf; uint8_t cap; uint8_t len; bool overflow; } rw_t;
static void rw_init(rw_t *w, uint8_t *buf, uint8_t cap) { w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false; }
static void rw_bytes(rw_t *w, const uint8_t *data, uint8_t n) {
    if (w->overflow || (uint16_t)w->len + (uint16_t)n > (uint16_t)w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, data, n); w->len = (uint8_t)(w->len + n);
}
static void rw_u8(rw_t *w, uint8_t v) { rw_bytes(w, &v, 1); }
static void rw_u16le(rw_t *w, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; rw_bytes(w, b, 2); }
static void rw_u32le(rw_t *w, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; rw_bytes(w, b, 4); }
static void rw_u64le(rw_t *w, uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); rw_bytes(w, b, 8); }
static void rw_f32le(rw_t *w, float v) { union { float f; uint32_t u; } c; c.f = v; rw_u32le(w, c.u); }

typedef struct { const uint8_t *buf; uint8_t len; uint8_t pos; bool overflow; } ar_t;
static void ar_init(ar_t *r, const uint8_t *buf, uint8_t len) { r->buf = buf; r->len = len; r->pos = 0; r->overflow = false; }
static bool ar_bytes(ar_t *r, uint8_t *out, uint8_t n) {
    if (r->overflow || (uint16_t)r->pos + (uint16_t)n > (uint16_t)r->len) { r->overflow = true; if (out) memset(out, 0, n); return false; }
    memcpy(out, r->buf + r->pos, n); r->pos = (uint8_t)(r->pos + n); return true;
}
static uint8_t ar_u8(ar_t *r) { uint8_t v = 0; ar_bytes(r, &v, 1); return v; }
static uint16_t ar_u16le(ar_t *r) { uint8_t b[2] = {0}; ar_bytes(r, b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t ar_u32le(ar_t *r) { uint8_t b[4] = {0}; ar_bytes(r, b, 4); return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint64_t ar_u64le(ar_t *r) { uint8_t b[8] = {0}; ar_bytes(r, b, 8); uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | b[i]; return v; }
static float ar_f32le(ar_t *r) { union { float f; uint32_t u; } c; c.u = ar_u32le(r); return c.f; }
static double ar_f64le(ar_t *r) { uint8_t b[8] = {0}; ar_bytes(r, b, 8); union { double d; uint64_t u; } c; c.u = 0; for (int i = 7; i >= 0; i--) c.u = (c.u << 8) | b[i]; return c.d; }

// ===========================================================================
// SYS group (PROTOCOL.md sec 4). RESET_SIM/SET_TIMESCALE/SET_SEED are real
// handlers here -- see file header's "known deviations".
// ===========================================================================
static void write_version_block(rw_t *w)
{
    rw_u16le(w, (uint16_t)BENCHPROTO_PROTOCOL_VERSION);
    rw_u16le(w, (uint16_t)BENCHPROTO_MIN_COMPATIBLE);
    rw_u8(w, SIMFW_FW_VERSION_MAJOR);
    rw_u8(w, SIMFW_FW_VERSION_MINOR);
    rw_u8(w, SIMFW_FW_VERSION_PATCH);
    rw_u8(w, 0); // git_dirty: n/a for a host harness
    const char *hash = "virtual";
    uint8_t hlen = (uint8_t)strlen(hash);
    rw_u8(w, hlen);
    rw_bytes(w, (const uint8_t *)hash, hlen);
}

static void reset_device(device_t *d, bool keep_params);

static bool dispatch_sys(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_SYS_PING:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    case SIMFW_CMD_SYS_GET_VERSION:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        write_version_block(w);
        return true;
    case SIMFW_CMD_SYS_GET_CAPS:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        write_version_block(w);
        rw_u8(w, SIMFW_CAPS_ZONE_COUNT_MIN);
        rw_u8(w, SIMFW_CAPS_ZONE_COUNT_MAX);
        rw_u8(w, SIMFW_CAPS_ZONE_COUNT_DEFAULT);
        rw_u8(w, SIMFW_CAPS_TC_MAIN_CHANNELS);
        rw_u8(w, SIMFW_CAPS_TC_SAFETY_CHANNELS);
        rw_u8(w, SIMFW_CAPS_CT_CHANNELS);
        rw_u8(w, SIMFW_CAPS_RELAY_CHANNELS);
        rw_u32le(w, SIMFW_CAPS_FEATURE_BITMASK);
        return true;
    case SIMFW_CMD_SYS_RESET_SIM: {
        // Virtual-device extension (real firmware: reserved/ERR_NOT_IMPL,
        // see file header). Request: [u8 keep_params].
        uint8_t keep = ar_u8(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        reset_device(d, keep != 0);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_SYS_SET_TIMESCALE: {
        // Virtual-device extension. Request: [f32 timescale].
        float v = ar_f32le(r);
        if (r->overflow || v <= 0.0f) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->timescale_x100 = (uint32_t)(v * 100.0f + 0.5f);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_SYS_SET_SEED: {
        // Virtual-device extension. Request: [u32 seed]. Re-seeds the fault
        // engine's PRNG immediately (fault_engine_init() -- PLAN.md 7.2's
        // determinism contract is anchored on this seed).
        uint32_t v = ar_u32le(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->seed = v;
        fault_engine_init(&d->fault_engine, v);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_SYS_GET_SIM_STATE: {
        // Read-back companion to SET_TIMESCALE/SET_SEED (PROTOCOL.md sec 4;
        // real firmware's handle_sys_get_sim_state()). No args. Reply:
        // {status, u32 seed, u8 snapshot_valid, u32 timescale_x100,
        // u64 sim_time_us} -- timescale_x100/sim_time_us report 0 before the
        // first tick's snapshot exists, same as real firmware.
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u32le(w, d->seed);
        rw_u8(w, d->has_published ? 1u : 0u);
        rw_u32le(w, d->has_published ? d->timescale_x100 : 0u);
        rw_u64le(w, d->has_published ? d->snap_sim_time_us : 0u);
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// MODEL group (PROTOCOL.md sec 5.1)
// ===========================================================================
static bool dispatch_model(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_MODEL_SET_ZONE_PARAMS: {
        uint8_t zone = ar_u8(r);
        thermal_zone_params_t p;
        p.C = ar_f32le(r);
        p.k_loss = ar_f32le(r);
        for (uint8_t i = 0; i < THERMAL_MODEL_MAX_ZONES; i++) p.k_couple[i] = ar_f32le(r);
        p.R_element = ar_f32le(r);
        p.element_health = ar_f32le(r);
        p.tc_lag_s = ar_f32le(r);
        p.T0 = ar_f32le(r);
        if (r->overflow || zone >= d->params.zone_count) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->params.zones[zone] = p;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_MODEL_GET_ZONE_PARAMS: {
        uint8_t zone = ar_u8(r);
        if (r->overflow || zone >= d->params.zone_count) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        const thermal_zone_params_t *p = &d->params.zones[zone];
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, zone);
        rw_f32le(w, p->C);
        rw_f32le(w, p->k_loss);
        for (uint8_t i = 0; i < THERMAL_MODEL_MAX_ZONES; i++) rw_f32le(w, p->k_couple[i]);
        rw_f32le(w, p->R_element);
        rw_f32le(w, p->element_health);
        rw_f32le(w, p->tc_lag_s);
        rw_f32le(w, p->T0);
        return true;
    }
    case SIMFW_CMD_MODEL_SET_AMBIENT: {
        float v = ar_f32le(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->params.T_ambient = v;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_MODEL_LOAD_PRESET: {
        uint8_t preset = ar_u8(r);
        if (r->overflow || preset >= THERMAL_PRESET_COUNT) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->current_preset = (thermal_preset_id_t)preset;
        thermal_model_load_preset(d->current_preset, &d->params);
        thermal_model_init(&d->state, &d->params);
        d->sim_time_us = 0;
        memset(d->zone_manual, 0, sizeof(d->zone_manual));
        float blend = 0.0f;
        for (uint8_t z = 0; z < d->params.zone_count; z++) blend += d->safety_weight[z] * d->state.T_zone[z];
        d->safety_tc_state_c = blend;
        d->safety_manual = false;
        d->ring_next_seq = 0;
        reset_client_evt_cursors();
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_MODEL_SET_TEMP: {
        uint8_t zone = ar_u8(r);
        uint8_t mode = ar_u8(r);
        float temp_c = ar_f32le(r);
        if (r->overflow || zone >= THERMAL_MODEL_MAX_ZONES || mode > 1u) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (mode == 1u) { d->zone_manual[zone] = true; d->zone_manual_temp_c[zone] = temp_c; }
        else d->zone_manual[zone] = false;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_MODEL_SET_TC_LAG: {
        uint8_t zone = ar_u8(r);
        float lag = ar_f32le(r);
        if (r->overflow || zone >= d->params.zone_count) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->params.zones[zone].tc_lag_s = lag;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// TC group (PROTOCOL.md sec 5.2)
// ===========================================================================
static bool dispatch_tc(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_TC_GET_REGS: {
        uint8_t channel = ar_u8(r);
        if (r->overflow || channel >= TC_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        tc_fault_override_t ovr = {0};
        (void)tc_fault_state_read((tc_fault_channel_t)channel, &ovr);
        float true_c = 0.0f, reported_c = 0.0f;
        if (channel < TC_FAULT_CHANNEL_SAFETY) {
            if (channel < d->snap_zone_count) { true_c = d->snap_T_true[channel]; reported_c = d->snap_T_tc[channel]; }
        } else {
            if (d->snap_zone_count > 0) { true_c = d->snap_T_true[0]; reported_c = d->snap_T_safety[0]; }
        }
        uint8_t flags = 0x01u; // reg_image_valid: always valid, single-threaded, no PIO busy window
        if (d->has_published) flags |= 0x02u;
        if (ovr.corruption.stuck_ltcb) flags |= 0x04u;
        if (ovr.corruption.spurious_fault_pin) flags |= 0x08u;
        if (ovr.force_sr_bits & MAX31856_FAULT_OPEN) flags |= 0x10u;
        if (ovr.force_sr_bits & MAX31856_FAULT_OVUV) flags |= 0x20u;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, channel);
        rw_u8(w, flags);
        rw_bytes(w, d->tc[channel].regs, MAX31856_REG_COUNT);
        rw_f32le(w, true_c);
        rw_f32le(w, reported_c);
        rw_u8(w, (uint8_t)ovr.corruption.dead_mode);
        rw_f32le(w, ovr.corruption.noise_sigma_c);
        rw_f32le(w, ovr.corruption.bit_error_rate);
        rw_u32le(w, 0); // spi transactions -- always 0, no DUT (file header)
        rw_u32le(w, 0);
        rw_u32le(w, 0);
        return true;
    }
    case SIMFW_CMD_TC_FORCE_TEMP: {
        uint8_t channel = ar_u8(r);
        float temp_c = ar_f32le(r);
        if (r->overflow || channel >= TC_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (channel == TC_FAULT_CHANNEL_SAFETY) { d->safety_manual = true; d->safety_manual_temp_c = temp_c; }
        else { d->zone_manual[channel] = true; d->zone_manual_temp_c[channel] = temp_c; }
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_TC_SET_MODE: {
        uint8_t channel = ar_u8(r);
        uint8_t mode = ar_u8(r);
        float temp_c = ar_f32le(r);
        if (r->overflow || channel >= TC_NUM_CHANNELS || mode > 1u) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        bool is_safety = (channel == TC_FAULT_CHANNEL_SAFETY);
        if (mode == 1u) {
            if (is_safety) { d->safety_manual = true; d->safety_manual_temp_c = temp_c; }
            else { d->zone_manual[channel] = true; d->zone_manual_temp_c[channel] = temp_c; }
        } else {
            if (is_safety) d->safety_manual = false; else d->zone_manual[channel] = false;
        }
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_TC_INJECT_FAULT: {
        uint16_t slot_id = ar_u16le(r);
        uint8_t channel = ar_u8(r);
        uint8_t fault_kind = ar_u8(r);
        float param0 = ar_f32le(r);
        if (r->overflow || channel >= TC_NUM_CHANNELS || fault_kind > (uint8_t)FT_TC_CJ_FAULT) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        fault_trigger_t trig = {0}; trig.kind = FAULT_TRIGGER_MANUAL;
        fault_duration_t dur = {0}; dur.kind = FAULT_DURATION_PERMANENT;
        fault_repeat_t rep = {0}; rep.kind = FAULT_REPEAT_ONCE;
        float params[4] = {param0, 0, 0, 0};
        uint16_t sid = fault_engine_schedule(&d->fault_engine, slot_id, fault_kind, channel, &trig, &dur, &rep, params);
        if (sid == FAULT_ENGINE_INVALID_SLOT) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        fault_event_t events[2];
        size_t n = fault_engine_fire_now(&d->fault_engine, sid, d->last_sim_time_s, events, 2);
        if (n == 0) { rw_u8(w, SIMFW_CMD_STATUS_ERR_INTERNAL); return true; }
        apply_edge_effects(d, events, n);
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u16le(w, sid);
        return true;
    }
    case SIMFW_CMD_TC_CLEAR_FAULT: {
        uint16_t slot_id = ar_u16le(r);
        if (r->overflow || !fault_engine_cancel(&d->fault_engine, slot_id)) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_TC_GET_MASTER_CONFIG: {
        uint8_t channel = ar_u8(r);
        if (r->overflow || channel >= TC_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        uint8_t flags = 0x02u; // reg_image_valid always true here; configured stays 0 (no DUT ever wrote, file header)
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, channel);
        rw_u8(w, flags);
        rw_u8(w, d->tc[channel].regs[MAX31856_REG_CR0]);
        rw_u8(w, d->tc[channel].regs[MAX31856_REG_CR1]);
        rw_u8(w, d->tc[channel].regs[MAX31856_REG_MASK]);
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// CT group (PROTOCOL.md sec 5.3)
// ===========================================================================
static bool dispatch_ct(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_CT_SET_MODE: {
        uint8_t ch = ar_u8(r); uint8_t mode = ar_u8(r);
        if (r->overflow || ch >= CT_NUM_CHANNELS || mode > 1u) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->ct[ch].mode = mode;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_CT_SET_AMPS: {
        uint8_t ch = ar_u8(r); float amps = ar_f32le(r);
        if (r->overflow || ch >= CT_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->ct[ch].manual_amps_pending = amps;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_CT_SET_PHASE: {
        uint8_t ch = ar_u8(r); float p = ar_f32le(r);
        if (r->overflow || ch >= CT_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->ct[ch].phase_deg = p;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_CT_SET_DISTORTION: {
        uint8_t ch = ar_u8(r);
        float dc = ar_f32le(r), clip = ar_f32le(r);
        uint8_t dh = ar_u8(r), dn = ar_u8(r), ai = ar_u8(r);
        if (r->overflow || ch >= CT_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->ct[ch].dc_offset = dc; d->ct[ch].clip_fraction = clip;
        d->ct[ch].dropout_half_cycle = dh != 0; d->ct[ch].dropout_negative_half = dn != 0;
        d->ct[ch].apply_immediately = ai != 0;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_CT_GET_STATE: {
        uint8_t ch = ar_u8(r);
        if (r->overflow || ch >= CT_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        ct_channel_t *c = &d->ct[ch];
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, c->mode);
        rw_f32le(w, c->amps);
        rw_f32le(w, c->phase_deg);
        rw_f32le(w, c->dc_offset);
        rw_f32le(w, c->clip_fraction);
        rw_u8(w, c->dropout_half_cycle ? 1u : 0u);
        rw_u8(w, c->dropout_negative_half ? 1u : 0u);
        rw_u8(w, c->apply_immediately ? 1u : 0u);
        rw_f32le(w, c->last_pwm_scale);
        rw_u8(w, c->valid ? 1u : 0u);
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// RELAY group (PROTOCOL.md sec 5.4) -- PLUS one VIRTUAL-ONLY extension.
//
// SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE (0xF0) is NOT part of PROTOCOL.md and
// deliberately NOT added to firmware/SimFW/src/tasks/cmd_ids.h (that header
// is real firmware's numeric source of truth and is read-only for this
// pass regardless). On real hardware the fixture only ever SENSES relay
// contacts through the MCP23017 (PLAN.md sec 3.4) -- there is no wire, no
// GPIO, no physical mechanism by which a DUT could ever tell the fixture
// "I closed this contact"; the fixture watches the contact, it does not
// take dictation from the board. cmd_ids.h's own comment on this group
// already says as much: "RELAY_SET_CONTACT_FAULT ... deliberately NOT
// allocated ... relay sense is read-only from this task's perspective by
// design." Adding a real SET command to that header, or to real firmware,
// would therefore misrepresent the hardware and must never happen.
//
// This command exists ONLY because a *virtual* DUT (firmware/SimFW/tools/
// virtual_dut/) has no physical relay coil to close in the first place --
// there is no contact for a virtual fixture to sense, so nothing at all
// would ever move without some substitute for that missing wire. This id
// is that substitute, scoped as narrowly as possible: it lets a connected
// client report what a real relay's sensed contact WOULD read if the
// (nonexistent, virtual) coil it represents were in that state, standing
// in for the physical sense wire that a real fixture would use instead.
// Once told, this harness treats the value exactly as it treats any other
// sensed contact -- same relay_mask bit, same duty[]/edge-log/telemetry
// path device_tick() already runs for K1/K2/K3/K5 (PLAN.md sec 2 loop 1)
// -- see this file's own README.md "Known, virtual-only extensions"
// section for the full writeup, including the verified finding that
// device_tick()'s duty[]/current_a[] math does not gate on K4 today (a
// property of REAL, unmodified sim_engine.c too -- not something this
// pass could or should paper over here).
//
// Request: [u8 signal, u8 level]. signal uses the SAME 0..4 numbering as
// RELAY_GET_STATES's reply order / edge_entry_t.signal (0 K1, 1 K2, 2 K3,
// 3 K5, 4 K4) -- 5 (FAULT_LINE) is refused (ERR_BAD_ARGS): that line is
// ESP-driven, not a relay, and has no coil for a virtual DUT to represent.
// level: 0 = open/not sensed closed, nonzero = sensed closed. Reply:
// [status] only.
// ===========================================================================
#define SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE 0xF0u

static bool dispatch_relay(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE: {
        uint8_t signal = ar_u8(r);
        uint8_t level = ar_u8(r);
        if (r->overflow || signal > 4u) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        bool closed = level != 0u;
        switch (signal) {
        case 0u: d->k1 = closed; break;
        case 1u: d->k2 = closed; break;
        case 2u: d->k3 = closed; break;
        case 3u: d->k5 = closed; break;
        case 4u: d->k4 = closed; break;
        default: break;
        }
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_RELAY_GET_STATES:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, d->k1 ? 1u : 0u);
        rw_u8(w, d->k2 ? 1u : 0u);
        rw_u8(w, d->k3 ? 1u : 0u);
        rw_u8(w, d->k5 ? 1u : 0u);
        rw_u8(w, d->k4 ? 1u : 0u);
        rw_u8(w, d->fault_line_asserted ? 1u : 0u);
        rw_u64le(w, d->relay_sample_time_us);
        rw_u8(w, 1u); // valid
        return true;
    case SIMFW_CMD_RELAY_GET_EDGES: {
        uint32_t since_seq = ar_u32le(r);
        uint8_t max_count = ar_u8(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (max_count > 8u) max_count = 8u;
        uint32_t oldest = (d->edge_count > EDGE_LOG_CAPACITY) ? (d->edge_count - EDGE_LOG_CAPACITY) : 0u;
        uint32_t start = since_seq < oldest ? oldest : since_seq;
        uint8_t n = 0;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        uint8_t *count_pos = w->buf + w->len;
        rw_u8(w, 0); // placeholder count, patched below
        while (start + n < d->edge_count && n < max_count) {
            const edge_entry_t *e = &d->edges[(start + n) % EDGE_LOG_CAPACITY];
            rw_u32le(w, e->seq);
            rw_u8(w, e->signal);
            rw_u8(w, e->level ? 1u : 0u);
            rw_u64le(w, e->time_us);
            n++;
        }
        if (!w->overflow) *count_pos = n;
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// IO group (PROTOCOL.md sec 5.5)
// ===========================================================================
static bool dispatch_io(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_IO_SET_DIR:
    case SIMFW_CMD_IO_WRITE:
        // Generic expander I/O: no MCP23017 emulated here (nothing in the
        // required scenario set exercises J20/spare pins) -- ack only.
        (void)ar_u8(r); (void)ar_u8(r); (void)ar_u8(r); if (cmd == SIMFW_CMD_IO_SET_DIR) (void)ar_u8(r);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    case SIMFW_CMD_IO_READ:
        (void)ar_u8(r); (void)ar_u8(r);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, 0);
        return true;
    case SIMFW_CMD_IO_ESTOP_SET: {
        uint8_t open = ar_u8(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->estop_open = open != 0;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_IO_ESTOP_GET:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, d->estop_open ? 1u : 0u);
        return true;
    case SIMFW_CMD_IO_FAULT_LINE_GET:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, d->fault_line_asserted ? 1u : 0u);
        rw_u64le(w, d->relay_sample_time_us);
        rw_u8(w, 1u);
        return true;
    case SIMFW_CMD_IO_DUT_POWER_SET: {
        uint8_t on = ar_u8(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        d->dut_power_on = on != 0;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_IO_DUT_POWER_GET:
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u8(w, d->dut_power_on ? 1u : 0u);
        return true;
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

// ===========================================================================
// FAULT group (PROTOCOL.md sec 5.6)
// ===========================================================================

// --- cmd_task.c's decode_fault_trigger(), ported verbatim: decodes the
// wire's compact fault_trigger_t encoding (u8 trigger_kind, f64 trigger_a,
// f64 trigger_b, u16 trigger_ref, u8 trigger_edge, char[24] event_name),
// shared by FAULT_SCHEDULE's own ARM trigger and FAULT_SET_UNTIL_TRIGGER's
// release trigger (PROTOCOL.md sec 5.6: "byte-identical to FAULT_SCHEDULE's
// own ARM-trigger fields"). Returns false (leaving *out zeroed) if
// trigger_kind is out of range; does not itself inspect r->overflow, same as
// cmd_task.c's version -- callers check that separately.
static bool decode_fault_trigger_wire(ar_t *r, fault_trigger_t *out)
{
    uint8_t trigger_kind = ar_u8(r);
    double trigger_a = ar_f64le(r);
    double trigger_b = ar_f64le(r);
    uint16_t trigger_ref = ar_u16le(r);
    uint8_t trigger_edge = ar_u8(r);
    char event_name[24];
    ar_bytes(r, (uint8_t *)event_name, 24);

    memset(out, 0, sizeof(*out));
    if (trigger_kind > (uint8_t)FAULT_TRIGGER_MANUAL) return false;

    out->kind = (fault_trigger_kind_t)trigger_kind;
    switch (out->kind) {
    case FAULT_TRIGGER_AT_SIM_TIME:
        out->at_sim_time_s = trigger_a;
        break;
    case FAULT_TRIGGER_AT_ZONE_TEMP:
        out->zone = (uint8_t)trigger_ref;
        out->temp_c = (float)trigger_a;
        out->temp_edge = (fault_temp_edge_t)trigger_edge;
        break;
    case FAULT_TRIGGER_ON_RELAY_EDGE:
        out->relay = (uint8_t)trigger_ref;
        out->delay_s = trigger_a;
        out->relay_edge = (fault_relay_edge_t)trigger_edge;
        break;
    case FAULT_TRIGGER_ON_EVENT:
        out->delay_s = trigger_a;
        memcpy(out->event_name, event_name, sizeof(out->event_name));
        break;
    case FAULT_TRIGGER_AFTER_FAULT:
        out->delay_s = trigger_a;
        out->after_fault_slot = trigger_ref;
        break;
    case FAULT_TRIGGER_RANDOM_IN:
        out->random_t0_s = trigger_a;
        out->random_t1_s = trigger_b;
        break;
    case FAULT_TRIGGER_MANUAL:
    default:
        break;
    }
    return true;
}

// --- cmd_task.c's s_pending_until[]/pending_until_trigger_t, ported
// verbatim: FAULT_SCHEDULE(duration_kind==UNTIL_TRIGGER) parks everything
// but the release trigger here, indexed directly by slot_id (0..
// FAULT_ENGINE_MAX_SLOTS-1, the same range the slot pool itself uses).
// FAULT_SET_UNTIL_TRIGGER supplies the release trigger and performs the
// actual fault_engine_schedule() call combining it with these parked
// fields -- see PROTOCOL.md sec 5.6 for the full two-frame design.
typedef struct {
    bool pending;
    uint8_t fault_type;
    uint16_t target;
    fault_trigger_t trigger;
    fault_repeat_t repeat;
    float params[4];
} pending_until_trigger_t;

static pending_until_trigger_t s_pending_until[FAULT_ENGINE_MAX_SLOTS];

static bool dispatch_fault(device_t *d, uint8_t cmd, ar_t *r, rw_t *w)
{
    switch (cmd) {
    case SIMFW_CMD_FAULT_SCHEDULE: {
        uint16_t slot_id = ar_u16le(r);
        uint8_t fault_type = ar_u8(r);
        uint16_t target = ar_u16le(r);
        fault_trigger_t trig;
        bool trigger_ok = decode_fault_trigger_wire(r, &trig);
        uint8_t duration_kind = ar_u8(r);
        double duration_for_s = ar_f64le(r);
        uint8_t repeat_kind = ar_u8(r);
        double repeat_period_s = ar_f64le(r);
        double repeat_jitter_s = ar_f64le(r);
        uint16_t repeat_n = ar_u16le(r);
        float params[4];
        for (int i = 0; i < 4; i++) params[i] = ar_f32le(r);

        if (r->overflow || !trigger_ok) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }

        target_kind_t kind = target_kind_of((fault_type_t)fault_type);
        if (kind == TK_INVALID) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (kind == TK_TC && target >= TC_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (kind == TK_ZONE && target >= THERMAL_MODEL_MAX_ZONES) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (kind == TK_CT && target >= CT_NUM_CHANNELS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }

        fault_repeat_t rep = {0};
        rep.kind = (fault_repeat_kind_t)repeat_kind;
        rep.period_s = repeat_period_s;
        rep.jitter_s = repeat_jitter_s;
        rep.n = repeat_n;

        if (duration_kind == (uint8_t)FAULT_DURATION_UNTIL_TRIGGER) {
            // Two-frame path (cmd_task.c's handle_fault_schedule(), PROTOCOL.md
            // sec 5.6): park everything but the release trigger, do not arm
            // yet. duration_for_s is ignored for this duration_kind.
            if (slot_id >= FAULT_ENGINE_MAX_SLOTS) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
            s_pending_until[slot_id].pending = true;
            s_pending_until[slot_id].fault_type = fault_type;
            s_pending_until[slot_id].target = target;
            s_pending_until[slot_id].trigger = trig;
            s_pending_until[slot_id].repeat = rep;
            memcpy(s_pending_until[slot_id].params, params, sizeof(params));
            rw_u8(w, SIMFW_CMD_STATUS_OK);
            rw_u16le(w, slot_id);
            return true;
        }

        // A direct (PERMANENT/FOR) schedule on this slot_id supersedes any
        // still-pending UNTIL_TRIGGER parked on it (frame 1 sent, frame 2
        // never arrived) -- discard the stale entry so a later, unrelated
        // SET_UNTIL_TRIGGER cannot resurrect it against this new schedule.
        if (slot_id < FAULT_ENGINE_MAX_SLOTS) s_pending_until[slot_id].pending = false;

        fault_duration_t dur = {0};
        dur.kind = (fault_duration_kind_t)duration_kind;
        dur.for_s = duration_for_s;

        uint16_t sid = fault_engine_schedule(&d->fault_engine, slot_id, fault_type, target, &trig, &dur, &rep, params);
        if (sid == FAULT_ENGINE_INVALID_SLOT) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u16le(w, sid);
        return true;
    }
    case SIMFW_CMD_FAULT_SET_UNTIL_TRIGGER: {
        // Frame 2 of the UNTIL_TRIGGER two-frame design (PROTOCOL.md sec
        // 5.6, cmd_task.c's handle_fault_set_until_trigger()): supplies the
        // RELEASE trigger and performs the actual fault_engine_schedule()
        // call, combining it with the fields FAULT_SCHEDULE parked in
        // s_pending_until[slot_id].
        uint16_t slot_id = ar_u16le(r);
        fault_trigger_t release_trig;
        bool trigger_ok = decode_fault_trigger_wire(r, &release_trig);
        if (r->overflow || !trigger_ok || slot_id >= FAULT_ENGINE_MAX_SLOTS || !s_pending_until[slot_id].pending) {
            rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS);
            return true;
        }

        pending_until_trigger_t *pend = &s_pending_until[slot_id];
        fault_duration_t dur = {0};
        dur.kind = FAULT_DURATION_UNTIL_TRIGGER;
        dur.until_trigger = release_trig;

        uint16_t sid = fault_engine_schedule(&d->fault_engine, slot_id, pend->fault_type, pend->target,
                                              &pend->trigger, &dur, &pend->repeat, pend->params);
        pend->pending = false; // consumed regardless of outcome -- resend FAULT_SCHEDULE to retry
        if (sid == FAULT_ENGINE_INVALID_SLOT) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        rw_u16le(w, sid);
        return true;
    }
    case SIMFW_CMD_FAULT_CANCEL: {
        uint16_t slot_id = ar_u16le(r);
        // Discard any still-pending UNTIL_TRIGGER schedule on this slot too
        // (frame 1 sent, frame 2 never arrived) -- nothing is armed in the
        // fault engine yet to cancel for that case, but the parked intent
        // should not survive an explicit cancel (PROTOCOL.md sec 5.6).
        if (slot_id < FAULT_ENGINE_MAX_SLOTS) s_pending_until[slot_id].pending = false;
        if (r->overflow || !fault_engine_cancel(&d->fault_engine, slot_id)) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_FAULT_FIRE_NOW: {
        uint16_t slot_id = ar_u16le(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        fault_event_t events[2];
        size_t n = fault_engine_fire_now(&d->fault_engine, slot_id, d->last_sim_time_s, events, 2);
        if (n == 0) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BUSY); return true; }
        apply_edge_effects(d, events, n);
        recompute_overrides(d);
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        return true;
    }
    case SIMFW_CMD_FAULT_LIST: {
        uint8_t start_index = ar_u8(r);
        uint8_t max_count = ar_u8(r);
        if (r->overflow) { rw_u8(w, SIMFW_CMD_STATUS_ERR_BAD_ARGS); return true; }
        if (max_count > 8u) max_count = 8u;
        rw_u8(w, SIMFW_CMD_STATUS_OK);
        uint8_t *count_pos = w->buf + w->len;
        rw_u8(w, 0);
        uint8_t n = 0;
        for (uint16_t i = start_index; i < FAULT_ENGINE_MAX_SLOTS && n < max_count; i++) {
            const fault_slot_t *s = &d->fault_engine.slots[i];
            rw_u16le(w, s->slot_id);
            rw_u8(w, (uint8_t)s->state);
            rw_u16le(w, s->fault_type);
            rw_u16le(w, s->target);
            rw_u32le(w, s->fire_count);
            rw_f32le(w, (float)s->active_since_s);
            n++;
        }
        if (!w->overflow) *count_pos = n;
        return true;
    }
    default:
        rw_u8(w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        return true;
    }
}

static uint8_t dispatch_command(device_t *d, uint8_t task_id, const uint8_t *payload, uint8_t payload_len,
                                 uint8_t *out, uint8_t out_cap)
{
    if (payload_len < 1) return 0;
    uint8_t cmd = payload[0];
    ar_t r; ar_init(&r, payload + 1, (uint8_t)(payload_len - 1));
    rw_t w; rw_init(&w, out, out_cap);

    bool handled;
    switch (task_id) {
    case SIMFW_TASK_ID_SYS: handled = dispatch_sys(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_MODEL: handled = dispatch_model(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_TC: handled = dispatch_tc(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_CT: handled = dispatch_ct(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_RELAY: handled = dispatch_relay(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_IO: handled = dispatch_io(d, cmd, &r, &w); break;
    case SIMFW_TASK_ID_FAULT: handled = dispatch_fault(d, cmd, &r, &w); break;
    default:
        rw_u8(&w, SIMFW_CMD_STATUS_ERR_NOT_IMPL);
        handled = true;
        break;
    }
    (void)handled;
    return w.len;
}

// ===========================================================================
// Telemetry / EVT broadcast (PROTOCOL.md sec 6)
// ===========================================================================
static void send_telemetry(device_t *d)
{
    uint8_t payload[128];
    rw_t w; rw_init(&w, payload, sizeof(payload));
    rw_u8(&w, SIMFW_EVT_FRAME_KIND_TELEMETRY);
    rw_u64le(&w, d->snap_sim_time_us);
    rw_u32le(&w, d->timescale_x100);
    rw_u32le(&w, d->seed);
    rw_u8(&w, (uint8_t)d->snap_zone_count);
    for (uint8_t z = 0; z < d->snap_zone_count; z++) {
        rw_f32le(&w, d->snap_T_true[z]);
        rw_f32le(&w, d->snap_T_tc[z]);
        rw_f32le(&w, d->snap_T_safety[z]);
        rw_f32le(&w, d->snap_I_amps[z]);
    }
    rw_u16le(&w, d->snap_relay_mask);
    rw_u8(&w, d->snap_estop_open ? 1u : 0u);
    rw_u8(&w, d->fault_line_asserted ? 1u : 0u);
    rw_u16le(&w, (uint16_t)d->active_fault_count);
    rw_u32le(&w, 0); // spi_transactions_total
    rw_u32le(&w, 0); // spi_underruns_total
    rw_u32le(&w, 0); // evt_ring_high_water_mark
    rw_u32le(&w, 0); // evt_seq_gap_count -- single-process ring, never drops
    rw_u32le(&w, 0); // evt_send_drop_count

    benchproto_frame_t frame;
    frame.msg_type = BENCHPROTO_MSG_BROADCAST;
    frame.msg_index = 0;
    frame.src_device = SIMFW_TARGET_DEVICE;
    frame.src_task = SIMFW_TASK_ID_EVT;
    frame.dst_device = SIMFW_HOST_DEVICE;
    frame.dst_task = 0;
    frame.length = w.len;
    frame.payload = payload;
    send_frame_broadcast(&frame);
}

// Per-client EVT drain: each client owns its own telemetry_next_evt_seq
// cursor into the shared, globally-sequenced ring (see the multi-client
// block comment above client_t's definition for why this must be per-
// client rather than one shared cursor).
static void drain_events_for(device_t *d, client_t *c)
{
    while (c->telemetry_next_evt_seq < d->ring_next_seq) {
        uint32_t oldest = (d->ring_next_seq > SIM_EVENT_RING_SIZE) ? (d->ring_next_seq - SIM_EVENT_RING_SIZE) : 0u;
        if (c->telemetry_next_evt_seq < oldest) c->telemetry_next_evt_seq = oldest;
        const sim_event_t *e = &d->ring[c->telemetry_next_evt_seq % SIM_EVENT_RING_SIZE];

        uint8_t payload[20];
        rw_t w; rw_init(&w, payload, sizeof(payload));
        rw_u8(&w, SIMFW_EVT_FRAME_KIND_EVENT);
        rw_u32le(&w, e->seq);
        rw_u64le(&w, e->sim_time_us);
        rw_u8(&w, (uint8_t)e->type);
        rw_u8(&w, e->a);
        rw_u8(&w, e->b);
        rw_f32le(&w, e->f0);

        benchproto_frame_t frame;
        frame.msg_type = BENCHPROTO_MSG_BROADCAST;
        frame.msg_index = 0;
        frame.src_device = SIMFW_TARGET_DEVICE;
        frame.src_task = SIMFW_TASK_ID_EVT;
        frame.dst_device = SIMFW_HOST_DEVICE;
        frame.dst_task = 0;
        frame.length = w.len;
        frame.payload = payload;
        send_frame_to(c, &frame);

        c->telemetry_next_evt_seq++;
    }
}

static void drain_events_all(device_t *d)
{
    for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use) drain_events_for(d, &g_clients[i]);
    }
}

// ===========================================================================
// device (re)initialization
// ===========================================================================
static void reset_device(device_t *d, bool keep_params)
{
    if (!keep_params) {
        thermal_model_load_preset(THERMAL_PRESET_FAST_TEST, &d->params);
        d->current_preset = THERMAL_PRESET_FAST_TEST;
    }
    thermal_model_init(&d->state, &d->params);
    d->sim_time_us = 0;
    memset(d->zone_manual, 0, sizeof(d->zone_manual));

    float blend = 0.0f;
    for (uint8_t z = 0; z < d->params.zone_count; z++) blend += d->safety_weight[z] * d->state.T_zone[z];
    d->safety_tc_state_c = blend;
    d->safety_manual = false;

    d->ring_next_seq = 0;
    reset_client_evt_cursors();
    d->edge_write_idx = 0;
    d->edge_count = 0;
    d->relay_mask_prev = 0;

    fault_engine_init(&d->fault_engine, d->seed);
    memset(d->zone_duty_override_active, 0, sizeof(d->zone_duty_override_active));
    memset(d->zone_health_override_active, 0, sizeof(d->zone_health_override_active));
    memset(d->zone_thermal_override_active, 0, sizeof(d->zone_thermal_override_active));
    memset(d->zone_tc_lag_override_active, 0, sizeof(d->zone_tc_lag_override_active));
    d->safety_fault_override_active = false;
    memset(d->ct_phase_loss_was_active, 0, sizeof(d->ct_phase_loss_was_active));
    memset(d->ct_half_wave_was_active, 0, sizeof(d->ct_half_wave_was_active));
    memset(d->ct_k4_weld_was_active, 0, sizeof(d->ct_k4_weld_was_active));

    for (unsigned ch = 0; ch < TC_NUM_CHANNELS; ch++) {
        max31856_regs_init(&d->tc[ch], d->seed ? d->seed + ch : 1u + ch);
        tc_fault_state_clear((tc_fault_channel_t)ch);
    }
}

static void device_init(device_t *d, uint32_t seed)
{
    memset(d, 0, sizeof(*d));
    d->timescale_x100 = 100; // 1.00x default
    d->seed = seed;
    d->safety_weight[0] = 1.0f; // PLAN.md 4.3 default: zone 0
    d->safety_lag_s = 5.0f;
    d->safety_fault_gain = 1.0f;
    d->dut_power_on = true; // default: board powered (PLAN.md 3.4 -- fixture
                             // does not itself pick an E-stop default, but a
                             // DUT power relay defaulting OFF would make
                             // every scenario start with a dead board)
    d->estop_open = false;
    for (unsigned c = 0; c < CT_NUM_CHANNELS; c++) d->ct[c].apply_immediately = true;
    reset_device(d, false);
}

// ===========================================================================
// Networking + main loop
// ===========================================================================
static void handle_wire_frame(device_t *d, client_t *c, const uint8_t *stuffed, size_t stuffed_len)
{
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t ust;
    size_t raw_len = benchproto_unstuff(stuffed, stuffed_len, raw, sizeof(raw), &ust);
    if (raw_len == 0 && ust != BENCHPROTO_FRAME_OK) return;

    benchproto_frame_t frame;
    benchproto_frame_status_t dst = benchproto_frame_decode(raw, raw_len, &frame);
    if (dst != BENCHPROTO_FRAME_OK) return;

    if (frame.msg_type != BENCHPROTO_MSG_DATA) return; // host never sends us ACK/NACK/BROADCAST
    if (frame.dst_device != SIMFW_TARGET_DEVICE) return;

    benchproto_link_action_t action = benchproto_link_on_frame(&c->link, NULL, &frame);

    uint8_t reply_payload[BENCHPROTO_FRAME_MAX_PAYLOAD];
    benchproto_frame_t reply;
    reply.msg_index = frame.msg_index;
    reply.src_device = SIMFW_TARGET_DEVICE;
    reply.src_task = frame.dst_task;
    reply.dst_device = frame.src_device;
    reply.dst_task = frame.src_task;

    if (action == BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE) {
        reply.msg_type = BENCHPROTO_MSG_NACK;
        reply.length = 0;
        reply.payload = NULL;
        send_frame_to(c, &reply);
        return;
    }
    if (action != BENCHPROTO_LINK_ACTION_DELIVER && action != BENCHPROTO_LINK_ACTION_DUPLICATE_REACK) {
        return; // IGNORE
    }

    uint8_t len = dispatch_command(d, frame.dst_task, frame.payload, frame.length, reply_payload, sizeof(reply_payload));
    benchproto_link_mark_delivered(&c->link, frame.dst_task, frame.src_device, frame.src_task, frame.msg_index);

    reply.msg_type = BENCHPROTO_MSG_ACK;
    reply.length = len;
    reply.payload = reply_payload;
    send_frame_to(c, &reply);
}

static void drain_rx(device_t *d, client_t *c)
{
    for (;;) {
        // Find first DELIM.
        size_t first = (size_t)-1;
        for (size_t i = 0; i < c->rx.len; i++) if (c->rx.buf[i] == BENCHPROTO_FRAME_DELIM) { first = i; break; }
        if (first == (size_t)-1) return;
        if (first > 0) { memmove(c->rx.buf, c->rx.buf + first, c->rx.len - first); c->rx.len -= first; }

        size_t second = (size_t)-1;
        for (size_t i = 1; i < c->rx.len; i++) if (c->rx.buf[i] == BENCHPROTO_FRAME_DELIM) { second = i; break; }
        if (second == (size_t)-1) return; // frame not complete yet

        if (second == 1) { memmove(c->rx.buf, c->rx.buf + 1, c->rx.len - 1); c->rx.len -= 1; continue; }

        handle_wire_frame(d, c, c->rx.buf, second + 1);
        memmove(c->rx.buf, c->rx.buf + second, c->rx.len - second);
        c->rx.len -= second;
    }
}

static double now_seconds(void)
{
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

int main(int argc, char **argv)
{
    uint16_t port = 8765;
    uint32_t seed = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return 1; }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d\n", WSAGetLastError());
        return 1;
    }
    if (listen(listener, (int)SIMFW_MAX_CLIENTS) == SOCKET_ERROR) { fprintf(stderr, "listen() failed\n"); return 1; }

    // Report the actual bound port (useful when --port 0 asks the OS to
    // pick an ephemeral one -- tests do exactly this).
    struct sockaddr_in bound;
    int bound_len = sizeof(bound);
    getsockname(listener, (struct sockaddr *)&bound, &bound_len);
    uint16_t actual_port = ntohs(bound.sin_port);
    printf("VIRTUAL_SIMFW_LISTENING port=%u seed=%u\n", (unsigned)actual_port, (unsigned)seed);
    fflush(stdout);

    for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
        g_clients[i].in_use = false;
        g_clients[i].sock = INVALID_SOCKET;
    }

    device_init(&g_dev, seed);

    double last_tick = now_seconds();
    double tick_accum_s = 0.0;
    double last_telemetry = now_seconds();

    for (;;) {
        // Poll the listener for a new connection every iteration --
        // regardless of how many clients are already connected, up to
        // SIMFW_MAX_CLIENTS (Task 2: kilnsim and virtual_dut both need to
        // be connected directly at once; see the multi-client block
        // comment above client_t's definition).
        {
            u_long nb = 1;
            ioctlsocket(listener, FIONBIO, &nb);
            fd_set rfds; FD_ZERO(&rfds); FD_SET(listener, &rfds);
            struct timeval tv = {0, 0};
            int r = select(0, &rfds, NULL, NULL, &tv);
            if (r > 0) {
                SOCKET c = accept(listener, NULL, NULL);
                if (c != INVALID_SOCKET) {
                    int slot = -1;
                    for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
                        if (!g_clients[i].in_use) { slot = (int)i; break; }
                    }
                    if (slot < 0) {
                        // Server full -- accept-then-close so the pending
                        // connection doesn't sit in the backlog forever.
                        closesocket(c);
                    } else {
                        client_t *nc = &g_clients[slot];
                        memset(nc, 0, sizeof(*nc));
                        nc->in_use = true;
                        nc->sock = c;
                        u_long nbc = 1;
                        ioctlsocket(nc->sock, FIONBIO, &nbc);
                        int nodelay = 1;
                        setsockopt(nc->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
                        client_register_tasks(nc);
                        // New clients start draining from "now", not from
                        // the full event history -- matches the original
                        // single-client harness's behavior on reconnect.
                        nc->telemetry_next_evt_seq = g_dev.ring_next_seq;
                        printf("VIRTUAL_SIMFW_CLIENT_CONNECTED slot=%d\n", slot);
                        fflush(stdout);
                    }
                }
            }
        }

        for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (!c->in_use) continue;
            char buf[4096];
            int n = recv(c->sock, buf, sizeof(buf), 0);
            if (n > 0) {
                if (c->rx.len + (size_t)n <= sizeof(c->rx.buf)) {
                    memcpy(c->rx.buf + c->rx.len, buf, (size_t)n);
                    c->rx.len += (size_t)n;
                } else {
                    c->rx.len = 0; // overflow -- drop and resync on next delimiter
                }
                drain_rx(&g_dev, c);
            } else if (n == 0) {
                client_drop(c);
                printf("VIRTUAL_SIMFW_CLIENT_DISCONNECTED slot=%u\n", i);
                fflush(stdout);
            } else {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    client_drop(c);
                    printf("VIRTUAL_SIMFW_CLIENT_DISCONNECTED slot=%u\n", i);
                    fflush(stdout);
                }
            }
        }

        double t = now_seconds();
        double wall_dt = t - last_tick;
        last_tick = t;
        if (wall_dt > 0.25) wall_dt = 0.25; // clamp a stall (debugger pause, etc.)
        tick_accum_s += wall_dt * ((double)g_dev.timescale_x100 / 100.0);
        while (tick_accum_s >= TICK_PERIOD_MS / 1000.0) {
            device_tick(&g_dev);
            tick_accum_s -= TICK_PERIOD_MS / 1000.0;
        }

        bool any_client = false;
        for (unsigned i = 0; i < SIMFW_MAX_CLIENTS; i++) if (g_clients[i].in_use) { any_client = true; break; }
        if (any_client) {
            drain_events_all(&g_dev);
            if (t - last_telemetry >= 1.0 / TELEMETRY_RATE_HZ) {
                send_telemetry(&g_dev);
                last_telemetry = t;
            }
        }

        Sleep(2);
    }
}
