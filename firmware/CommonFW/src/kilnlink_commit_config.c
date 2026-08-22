#include "kilnlink/kilnlink_commit_config.h"

size_t kilnlink_commit_config_encode(const kilnlink_commit_config_t *msg, uint8_t *out,
                                      size_t out_cap, kilnlink_commit_config_status_t *status)
{
    kilnlink_commit_config_status_t local_status = KILNLINK_COMMIT_CONFIG_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_COMMIT_CONFIG_OK;
    (void)msg; /* no fields to read */

    if (out_cap < KILNLINK_COMMIT_CONFIG_LEN) {
        *status = KILNLINK_COMMIT_CONFIG_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_COMMIT_CONFIG_CMD;

    return KILNLINK_COMMIT_CONFIG_LEN;
}

kilnlink_commit_config_status_t kilnlink_commit_config_decode(const uint8_t *payload, size_t len,
                                                                kilnlink_commit_config_t *out)
{
    if (len != KILNLINK_COMMIT_CONFIG_LEN) {
        return KILNLINK_COMMIT_CONFIG_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_COMMIT_CONFIG_CMD) {
        return KILNLINK_COMMIT_CONFIG_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved = 0;
    }

    return KILNLINK_COMMIT_CONFIG_OK;
}
