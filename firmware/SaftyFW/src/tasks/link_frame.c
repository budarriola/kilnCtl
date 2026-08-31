// link_frame.c -- see link_frame.h. Pure byte packing, no RTOS/SDK
// dependency (host-testable).
#include "link_frame.h"

#include <math.h>
#include <string.h>

// Extracts the little-endian wire bytes of a 32-bit value. memcpy'ing the
// float into a uint32_t first (rather than assuming a union layout) is the
// same convention firmware/KilnFW/App/drivers/safety_link.c already uses for
// this exact frame (safety_link.c:58) -- reused here rather than reinvented.
// Once `bits` holds the value, shifting it apart always yields correct LE
// bytes regardless of the host's own endianness, because the shifts operate
// on the integer's numeric value, not its in-memory byte order.
static void pack_u32_le(uint8_t *out, uint32_t bits)
{
    out[0] = (uint8_t)(bits & 0xFFu);
    out[1] = (uint8_t)((bits >> 8) & 0xFFu);
    out[2] = (uint8_t)((bits >> 16) & 0xFFu);
    out[3] = (uint8_t)((bits >> 24) & 0xFFu);
}

static void pack_f32_le(uint8_t *out, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    pack_u32_le(out, bits);
}

static void pack_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

// Mirror-image reads of the pack_* helpers above, for the ESP->Pico
// direction (link_frame_unpack_context()). Same house style: no assumption
// about the host's own endianness, no union-based type punning.
static uint32_t unpack_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static float unpack_f32_le(const uint8_t *in)
{
    uint32_t bits = unpack_u32_le(in);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

bool link_frame_status_v2_supported(uint16_t peer_protocol_version)
{
    return peer_protocol_version >= LINK_FRAME_STATUS_V2_MIN_PROTOCOL;
}

bool link_frame_rollback_result_supported(uint16_t peer_protocol_version)
{
    return peer_protocol_version >= LINK_FRAME_ROLLBACK_RESULT_MIN_PROTOCOL;
}

uint8_t link_frame_saturate_tx_dropped(uint32_t tx_dropped)
{
    return (tx_dropped > LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX)
               ? (uint8_t)(LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX + 1u) // 255, "254 or more"
               : (uint8_t)tx_dropped;
}

size_t link_frame_pack_status(uint8_t out[LINK_FRAME_STATUS_LEN_V2], bool estop, bool relay_energized,
                               bool heating_enabled, bool temp_valid, float safety_tc_c, float cj_c,
                               uint8_t tc_fault_bits, float amps1, float amps2, float amps3,
                               bool tc_not_installed, bool tc_injected,
                               bool peer_supports_status_v2, uint8_t tx_dropped_sat)
{
    out[0] = LINK_FRAME_STATUS_CMD;

    // Bits 0 (LINK_UP) and 1 (FAULT) are the ESP's to own -- always 0 here,
    // deliberately not parameters (see link_frame.h).
    uint8_t flags = 0;
    if (estop) {
        flags |= LINK_FLAG_ESTOP;
    }
    if (relay_energized) {
        flags |= LINK_FLAG_RELAY;
    }
    if (heating_enabled) {
        flags |= LINK_FLAG_ENABLED;
    }
    if (temp_valid) {
        flags |= LINK_FLAG_TEMP_VALID;
    }
    if (tc_not_installed) {
        flags |= LINK_FLAG_TC_NOT_INSTALLED;
    }
    if (tc_injected) {
        flags |= LINK_FLAG_TC_INJECTED;
    }
    out[1] = flags;

    pack_f32_le(&out[2], safety_tc_c);
    pack_f32_le(&out[6], cj_c);
    out[10] = tc_fault_bits;
    pack_f32_le(&out[11], amps1);
    pack_f32_le(&out[15], amps2);
    pack_f32_le(&out[19], amps3);

    // See this function's own doc comment (link_frame.h) for the full
    // skew-safety argument -- the caller decides peer_supports_status_v2,
    // this function only ever acts on that verdict.
    if (peer_supports_status_v2) {
        out[23] = tx_dropped_sat;
        return LINK_FRAME_STATUS_LEN_V2;
    }
    return LINK_FRAME_STATUS_LEN_V1;
}

size_t link_frame_pack_fw_version(uint8_t *out, size_t out_cap, uint16_t protocol_version,
                                   uint16_t min_compatible, uint8_t dirty, const char *commit,
                                   uint8_t commit_len, const char *datetime, uint8_t datetime_len,
                                   uint8_t boot_id, uint8_t config_version, uint16_t config_crc)
{
    // 1 (cmd) + 2 (proto) + 2 (min_compat) + 1 (dirty) + 1 (commit_len) +
    // commit_len + 1 (datetime_len) + datetime_len + 1 (boot_id) +
    // 1 (config_version) + 2 (config_crc).
    size_t needed = 1u + 2u + 2u + 1u + 1u + commit_len + 1u + datetime_len + 1u + 1u + 2u;
    if (out == NULL || out_cap < needed) {
        return 0;
    }

    size_t i = 0;
    out[i++] = LINK_FRAME_FW_VERSION_CMD;
    pack_u16_le(&out[i], protocol_version);
    i += 2;
    pack_u16_le(&out[i], min_compatible);
    i += 2;
    out[i++] = dirty;
    out[i++] = commit_len;
    if (commit_len > 0 && commit != NULL) {
        memcpy(&out[i], commit, commit_len);
    }
    i += commit_len;
    out[i++] = datetime_len;
    if (datetime_len > 0 && datetime != NULL) {
        memcpy(&out[i], datetime, datetime_len);
    }
    i += datetime_len;
    out[i++] = boot_id;
    out[i++] = config_version;
    pack_u16_le(&out[i], config_crc);
    i += 2;

    return i;
}

bool link_frame_unpack_context(const uint8_t *payload, uint8_t length, context_snapshot_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (length < LINK_FRAME_CONTEXT_MIN_LEN) {
        return false;
    }

    // Safe to read: length >= LINK_FRAME_CONTEXT_MIN_LEN (15) guarantees
    // offset 14 is present.
    uint8_t zone_count = payload[14];
    if (zone_count > CONTEXT_SNAPSHOT_MAX_ZONES) {
        return false;
    }
    if (length != LINK_FRAME_CONTEXT_MIN_LEN + (uint16_t)zone_count * LINK_FRAME_CONTEXT_ZONE_LEN) {
        return false;
    }

    // Frame is well-formed -- only now do we touch `*out`. Deliberately not
    // touching out->timestamp_ms (see link_frame.h).
    out->valid = true;
    out->flags = payload[1];
    out->boot_id = payload[2];
    out->seq = unpack_u32_le(&payload[3]);
    out->uptime_ms = unpack_u32_le(&payload[7]);
    out->relay_now_mask = payload[11];
    out->relay_recent_mask = payload[12];
    out->recent_window_s = payload[13];
    out->zone_count = zone_count;

    for (uint8_t i = 0; i < zone_count; i++) {
        const uint8_t *zp = &payload[LINK_FRAME_CONTEXT_MIN_LEN + (uint16_t)i * LINK_FRAME_CONTEXT_ZONE_LEN];
        context_zone_t *z = &out->zones[i];
        z->zone_index = zp[0];
        z->flags = zp[1];
        z->setpoint_c = unpack_f32_le(&zp[2]);
        z->measured_c = unpack_f32_le(&zp[6]);
        z->sample_counter = zp[10];
        z->tc_type = zp[11];
        z->tc_fault = zp[12];
        // zp[13] reserved, ignored.
    }

    return true;
}

bool link_frame_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                     uint16_t peer_protocol, uint16_t peer_min_compatible)
{
    return (peer_protocol >= self_min_compatible) && (self_protocol >= peer_min_compatible);
}

uint16_t link_frame_trip_mask_for_reason(safety_trip_t reason)
{
    if (reason == SAFETY_TRIP_NONE) {
        return 0u;
    }
    return (uint16_t)(1u << ((uint8_t)reason - 1u));
}

link_clear_trip_decision_t link_frame_decide_clear_trip(safety_trip_t current_trip_reason,
                                                          uint16_t wire_trip_mask)
{
    if (current_trip_reason == SAFETY_TRIP_NONE) {
        return LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED;
    }
    if (current_trip_reason == SAFETY_TRIP_INEFFECTIVE) {
        return LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE;
    }
    uint16_t current_mask = link_frame_trip_mask_for_reason(current_trip_reason);
    if (wire_trip_mask != current_mask) {
        return LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH;
    }
    return LINK_CLEAR_TRIP_ACCEPT;
}

void link_frame_apply_set_config(const config_store_record_t *committed, uint8_t tc_type,
                                  config_store_record_t *out)
{
    if (committed == NULL || out == NULL) {
        return;
    }
    *out = *committed;
    bool tc_type_changed = (committed->tc_type != tc_type);
    out->tc_type = tc_type;
    if (tc_type_changed) {
        out->calibration_missing = true; // see this function's header comment
                                          // (link_frame.h) -- a REAL type
                                          // change invalidates any prior
                                          // calibration.
    }
    // else: leave calibration_missing exactly as committed. An idempotent
    // resend of the already-committed tc_type -- exactly what every
    // automatic link-reconnect resend is (safety_link.c) -- must not
    // re-arm it.
}

void link_frame_apply_set_ct_cal(const config_store_record_t *committed, uint8_t channel,
                                  bool calibrated, float gain, float offset,
                                  config_store_record_t *out)
{
    if (committed == NULL || out == NULL || channel >= CONFIG_STORE_CT_CAL_NUM_CHANNELS) {
        return;
    }
    *out = *committed;
    out->ct_cal[channel].calibrated = calibrated;
    out->ct_cal[channel].gain = gain;
    out->ct_cal[channel].offset = offset;
}

bool link_frame_ceiling_is_active(float firing_max_c)
{
    // isfinite() rejects NaN and +/-Infinity; the strict > 0.0f rejects zero
    // (the wire's own "no firing" convention) AND every negative value, which
    // is the case safety_guards.c's min() clamp cannot catch on its own -- see
    // this function's own header comment.
    return isfinite(firing_max_c) && firing_max_c > 0.0f;
}

bool link_frame_clock_epoch_is_plausible(uint64_t epoch_ms)
{
    // 2020-01-01T00:00:00Z and 2100-01-01T00:00:00Z in Unix epoch
    // milliseconds -- generous, documented software bounds (this function's
    // own header comment), not measured physical constants.
    const uint64_t min_epoch_ms = 1577836800000ULL;
    const uint64_t max_epoch_ms = 4102444800000ULL;
    return epoch_ms >= min_epoch_ms && epoch_ms <= max_epoch_ms;
}
