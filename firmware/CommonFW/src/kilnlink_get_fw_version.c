#include "kilnlink/kilnlink_get_fw_version.h"

size_t kilnlink_get_fw_version_encode(const kilnlink_get_fw_version_t *msg, uint8_t *out,
                                       size_t out_cap, kilnlink_get_fw_version_status_t *status)
{
    kilnlink_get_fw_version_status_t local_status = KILNLINK_GET_FW_VERSION_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_FW_VERSION_OK;
    (void)msg; /* no fields to read */

    if (out_cap < KILNLINK_GET_FW_VERSION_LEN) {
        *status = KILNLINK_GET_FW_VERSION_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_GET_FW_VERSION_CMD;

    return KILNLINK_GET_FW_VERSION_LEN;
}

kilnlink_get_fw_version_status_t kilnlink_get_fw_version_decode(const uint8_t *payload, size_t len,
                                                                 kilnlink_get_fw_version_t *out)
{
    if (len != KILNLINK_GET_FW_VERSION_LEN) {
        return KILNLINK_GET_FW_VERSION_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_FW_VERSION_CMD) {
        return KILNLINK_GET_FW_VERSION_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved = 0;
    }

    return KILNLINK_GET_FW_VERSION_OK;
}
