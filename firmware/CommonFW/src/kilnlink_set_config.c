#include "kilnlink/kilnlink_set_config.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x16)
 *   1      u8  tc_type
 */
#define OFF_TC_TYPE 1u

size_t kilnlink_set_config_encode(const kilnlink_set_config_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_set_config_status_t *status)
{
    kilnlink_set_config_status_t local_status = KILNLINK_SET_CONFIG_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_SET_CONFIG_OK;

    if (out_cap < KILNLINK_SET_CONFIG_LEN) {
        *status = KILNLINK_SET_CONFIG_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_SET_CONFIG_CMD;
    out[OFF_TC_TYPE] = msg->tc_type;

    return KILNLINK_SET_CONFIG_LEN;
}

kilnlink_set_config_status_t kilnlink_set_config_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_set_config_t *out)
{
    if (len != KILNLINK_SET_CONFIG_LEN) {
        return KILNLINK_SET_CONFIG_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_SET_CONFIG_CMD) {
        return KILNLINK_SET_CONFIG_ERR_WRONG_CMD;
    }

    out->tc_type = payload[OFF_TC_TYPE];

    return KILNLINK_SET_CONFIG_OK;
}
