#include "kilnlink/kilnlink_apply_config_volatile.h"

size_t kilnlink_apply_config_volatile_encode(const kilnlink_apply_config_volatile_t *msg, uint8_t *out,
                                              size_t out_cap, kilnlink_apply_config_volatile_status_t *status)
{
    kilnlink_apply_config_volatile_status_t local_status = KILNLINK_APPLY_CONFIG_VOLATILE_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_APPLY_CONFIG_VOLATILE_OK;
    (void)msg; /* no fields to read */

    if (out_cap < KILNLINK_APPLY_CONFIG_VOLATILE_LEN) {
        *status = KILNLINK_APPLY_CONFIG_VOLATILE_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_APPLY_CONFIG_VOLATILE_CMD;

    return KILNLINK_APPLY_CONFIG_VOLATILE_LEN;
}

kilnlink_apply_config_volatile_status_t kilnlink_apply_config_volatile_decode(
    const uint8_t *payload, size_t len, kilnlink_apply_config_volatile_t *out)
{
    if (len != KILNLINK_APPLY_CONFIG_VOLATILE_LEN) {
        return KILNLINK_APPLY_CONFIG_VOLATILE_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_APPLY_CONFIG_VOLATILE_CMD) {
        return KILNLINK_APPLY_CONFIG_VOLATILE_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved = 0;
    }

    return KILNLINK_APPLY_CONFIG_VOLATILE_OK;
}
