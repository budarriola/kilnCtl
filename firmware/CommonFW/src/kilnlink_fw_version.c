#include "kilnlink/kilnlink_fw_version.h"

#include "kilnlink/kilnlink_bytes.h"

static size_t fw_version_wire_len(uint8_t commit_len, uint8_t datetime_len)
{
    return (size_t)KILNLINK_FW_VERSION_FIXED_LEN + (size_t)commit_len + (size_t)datetime_len;
}

size_t kilnlink_fw_version_encode(const kilnlink_fw_version_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_fw_version_status_t *status)
{
    kilnlink_fw_version_status_t local_status = KILNLINK_FW_VERSION_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_FW_VERSION_OK;

    if (msg->commit_len > KILNLINK_FW_VERSION_MAX_COMMIT_LEN ||
        msg->datetime_len > KILNLINK_FW_VERSION_MAX_DATETIME_LEN) {
        *status = KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG;
        return 0;
    }

    size_t needed = fw_version_wire_len(msg->commit_len, msg->datetime_len);
    if (out_cap < needed) {
        *status = KILNLINK_FW_VERSION_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_FW_VERSION_CMD;
    kilnlink_put_u16le(out, 1, msg->protocol_version);
    kilnlink_put_u16le(out, 3, msg->min_compatible);
    out[5] = msg->dirty;
    out[6] = msg->commit_len;

    size_t o = 7;
    for (uint8_t i = 0; i < msg->commit_len; ++i) {
        out[o + i] = msg->commit[i];
    }
    o += msg->commit_len;

    out[o] = msg->datetime_len;
    o += 1;
    for (uint8_t i = 0; i < msg->datetime_len; ++i) {
        out[o + i] = msg->datetime[i];
    }
    o += msg->datetime_len;

    out[o] = msg->boot_id;
    o += 1;
    out[o] = msg->config_version;
    o += 1;
    kilnlink_put_u16le(out, o, msg->config_crc);
    o += 2;

    return needed;
}

kilnlink_fw_version_status_t kilnlink_fw_version_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_fw_version_t *out)
{
    if (len < KILNLINK_FW_VERSION_FIXED_LEN) {
        return KILNLINK_FW_VERSION_ERR_TOO_SHORT;
    }
    if (payload[0] != KILNLINK_FW_VERSION_CMD) {
        return KILNLINK_FW_VERSION_ERR_WRONG_CMD;
    }

    uint8_t commit_len = payload[6];
    if (commit_len > KILNLINK_FW_VERSION_MAX_COMMIT_LEN) {
        return KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG;
    }

    /* datetime_len sits right after the commit bytes, whose own length
     * depends on commit_len -- so the fixed 12-byte prefix alone isn't
     * necessarily present yet; check incrementally rather than assuming
     * `len` already covers the byte at offset 7 + commit_len. */
    size_t datetime_len_off = (size_t)7 + commit_len;
    if (datetime_len_off >= len) {
        return KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH;
    }
    uint8_t datetime_len = payload[datetime_len_off];
    if (datetime_len > KILNLINK_FW_VERSION_MAX_DATETIME_LEN) {
        return KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG;
    }

    if (len != fw_version_wire_len(commit_len, datetime_len)) {
        return KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH;
    }

    out->protocol_version = kilnlink_get_u16le(payload, 1);
    out->min_compatible = kilnlink_get_u16le(payload, 3);
    out->dirty = payload[5];
    out->commit_len = commit_len;

    size_t o = 7;
    for (uint8_t i = 0; i < commit_len; ++i) {
        out->commit[i] = payload[o + i];
    }
    o += commit_len;

    /* o now equals datetime_len_off; payload[o] == datetime_len, already read above. */
    o += 1;
    out->datetime_len = datetime_len;
    for (uint8_t i = 0; i < datetime_len; ++i) {
        out->datetime[i] = payload[o + i];
    }
    o += datetime_len;

    out->boot_id = payload[o];
    o += 1;
    out->config_version = payload[o];
    o += 1;
    out->config_crc = kilnlink_get_u16le(payload, o);

    return KILNLINK_FW_VERSION_OK;
}
