#include "kilnlink/kilnlink_reboot.h"

size_t kilnlink_reboot_encode(const kilnlink_reboot_t *msg, uint8_t *out, size_t out_cap,
                              kilnlink_reboot_status_t *status)
{
    kilnlink_reboot_status_t local_status = KILNLINK_REBOOT_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_REBOOT_OK;
    (void)msg; /* no fields -- see the header's kilnlink_reboot_t comment */

    if (out_cap < KILNLINK_REBOOT_LEN) {
        *status = KILNLINK_REBOOT_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_REBOOT_CMD;

    return KILNLINK_REBOOT_LEN;
}

kilnlink_reboot_status_t kilnlink_reboot_decode(const uint8_t *payload, size_t len,
                                                kilnlink_reboot_t *out)
{
    if (len != KILNLINK_REBOOT_LEN) {
        return KILNLINK_REBOOT_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_REBOOT_CMD) {
        return KILNLINK_REBOOT_ERR_WRONG_CMD;
    }
    if (out) {
        out->reserved0 = 0u;
    }
    return KILNLINK_REBOOT_OK;
}
