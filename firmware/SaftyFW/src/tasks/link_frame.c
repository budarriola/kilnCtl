// link_frame.c -- see link_frame.h. Pure byte packing, no RTOS/SDK
// dependency (host-testable).
#include "link_frame.h"

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

void link_frame_pack_status(uint8_t out[LINK_FRAME_STATUS_LEN], bool estop, bool relay_energized,
                             bool heating_enabled, bool temp_valid, float safety_tc_c, float cj_c,
                             uint8_t tc_fault_bits, float amps1, float amps2, float amps3)
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
    out[1] = flags;

    pack_f32_le(&out[2], safety_tc_c);
    pack_f32_le(&out[6], cj_c);
    out[10] = tc_fault_bits;
    pack_f32_le(&out[11], amps1);
    pack_f32_le(&out[15], amps2);
    pack_f32_le(&out[19], amps3);
}

void link_frame_pack_diag(uint8_t out[LINK_FRAME_DIAG_LEN], uint8_t trip_reason,
                           uint16_t warn_mask, uint16_t trip_mask, uint32_t uptime_ms,
                           uint8_t boot_reason, uint8_t context_age_100ms,
                           uint32_t context_frames_ok, uint32_t context_frames_bad,
                           uint32_t tx_frames_dropped, uint8_t state, uint8_t flags)
{
    out[0] = LINK_FRAME_DIAG_CMD;
    out[1] = trip_reason;
    pack_u16_le(&out[2], warn_mask);
    pack_u16_le(&out[4], trip_mask);
    pack_u32_le(&out[6], uptime_ms);
    out[10] = boot_reason;
    out[11] = context_age_100ms;
    pack_u32_le(&out[12], context_frames_ok);
    pack_u32_le(&out[16], context_frames_bad);
    pack_u32_le(&out[20], tx_frames_dropped);
    out[24] = state;
    out[25] = flags;
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

bool link_frame_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                     uint16_t peer_protocol, uint16_t peer_min_compatible)
{
    return (peer_protocol >= self_min_compatible) && (self_protocol >= peer_min_compatible);
}
