// See safety_link_frame.h for why these functions live in their own
// dependency-light file. VERBATIM relocation out of safety_link.c -- no
// behavior change (see that header's own comment).
#include "safety_link_frame.h"

#include <string.h>

#include "esp_log.h"

#include "kilnlink/kilnlink_announce.h"

static const char *TAG = "safety_link_frame";

float safety_read_f32_le(const uint8_t *bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t safety_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

uint16_t safety_read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

void safety_put_f32_le(uint8_t *out, float value)
{
    memcpy(out, &value, sizeof(value));
}

void safety_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

void safety_put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

bool safety_link_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                      uint16_t peer_protocol, uint16_t peer_min_compatible)
{
    return (peer_protocol >= self_min_compatible) && (self_protocol >= peer_min_compatible);
}

bool safety_parse_fw_version(const uint8_t *p, uint8_t len, uint16_t *out_protocol,
                              uint16_t *out_min_compatible, uint8_t *out_boot_id,
                              bool *out_have_boot_id, bool *out_dirty,
                              uint8_t *out_commit, uint8_t *out_commit_len,
                              uint8_t *out_datetime, uint8_t *out_datetime_len,
                              uint8_t *out_config_version, uint16_t *out_config_crc,
                              bool *out_have_build)
{
    *out_have_boot_id = false;
    *out_have_build = false;
    if (len < 5u) {
        return false;
    }
    *out_protocol = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
    *out_min_compatible = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));

    if (len < 7u) {
        return true; /* no dirty/commit_len byte to even start the tail */
    }
    *out_dirty = (p[5] != 0u);
    size_t i = 6; /* byte5 = dirty, read above */
    uint8_t commit_len = p[i++];
    if ((size_t)commit_len + i > (size_t)len) {
        return true; /* truncated commit -- the fields already set stand */
    }
    /* STACK OVERFLOW FIX (audit 2026-08-27). The bound above checks only that
     * the SOURCE read stays inside the frame. commit_len is an untrusted wire
     * byte (0..255) and out_commit is a 64-byte buffer, so a CRC-valid frame
     * declaring commit_len=246 wrote 182 bytes past the caller's stack array,
     * over its saved return address, on safety_poll_task. Reachable from any
     * peer running mismatched or corrupted firmware -- exactly the case this
     * link exists to survive. The shared codec (kilnlink_announce.c) always
     * enforced this cap; only this hand-written parser ever lost it.
     *
     * Copy is capped; the PARSE OFFSET still advances by the full wire length,
     * so the datetime/boot_id/config fields after this one stay correctly
     * aligned instead of being read from the middle of an over-long commit
     * string. Truncating rather than rejecting is deliberate: protocol and
     * min_compatible were already parsed above and are what the compatibility
     * gate acts on, so a build string too long to store is a cosmetic loss,
     * not a reason to discard a frame that may be reporting a real version
     * mismatch. */
    uint8_t commit_copy = commit_len;
    if ((size_t)commit_copy > KILNLINK_ANNOUNCE_MAX_COMMIT_LEN) {
        ESP_LOGW(TAG, "FW_VERSION commit_len %u exceeds the %u-byte maximum -- storing a truncated "
                      "build string (peer firmware is mismatched or the frame is corrupt)",
                 (unsigned)commit_len, (unsigned)KILNLINK_ANNOUNCE_MAX_COMMIT_LEN);
        commit_copy = (uint8_t)KILNLINK_ANNOUNCE_MAX_COMMIT_LEN;
    }
    memcpy(out_commit, &p[i], commit_copy);
    *out_commit_len = commit_copy;
    i += commit_len; /* full wire length -- see the comment above */
    if (i >= (size_t)len) {
        return true;
    }
    uint8_t datetime_len = p[i++];
    if ((size_t)datetime_len + i > (size_t)len) {
        return true;
    }
    /* Same cap, same reasoning, same 32-byte destination -- see commit above. */
    uint8_t datetime_copy = datetime_len;
    if ((size_t)datetime_copy > KILNLINK_ANNOUNCE_MAX_DATETIME_LEN) {
        ESP_LOGW(TAG, "FW_VERSION datetime_len %u exceeds the %u-byte maximum -- storing a "
                      "truncated build datetime",
                 (unsigned)datetime_len, (unsigned)KILNLINK_ANNOUNCE_MAX_DATETIME_LEN);
        datetime_copy = (uint8_t)KILNLINK_ANNOUNCE_MAX_DATETIME_LEN;
    }
    memcpy(out_datetime, &p[i], datetime_copy);
    *out_datetime_len = datetime_copy;
    i += datetime_len; /* full wire length -- see the comment above */
    if (i >= (size_t)len) {
        return true;
    }
    *out_boot_id = p[i++];
    *out_have_boot_id = true;

    /* TODO.md owner-report item 5: config_version (u8) then config_crc (u16
     * LE), CommonFW/docs/LINK_PROTOCOL.md sec 4's Frame C table -- only
     * meaningful once both are actually present, hence the two-step length
     * check rather than reusing out_have_boot_id for this too (an older/
     * truncated Pico build that stops at boot_id must not report a fabricated
     * config_crc of 0 as "commissioned with a real CRC of zero"). */
    if (i >= (size_t)len) {
        return true;
    }
    *out_config_version = p[i++];
    if (i + 1u >= (size_t)len) {
        return true;
    }
    *out_config_crc = (uint16_t)(p[i] | ((uint16_t)p[i + 1] << 8));
    *out_have_build = true;
    return true;
}
