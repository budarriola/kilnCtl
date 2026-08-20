#include "kilnlink/kilnlink_announce_reboot.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x18)
 * (no other fields -- see kilnlink_announce_reboot.h's header comment)
 */

size_t kilnlink_announce_reboot_encode(const kilnlink_announce_reboot_t *msg, uint8_t *out, size_t out_cap,
                                        kilnlink_announce_reboot_status_t *status)
{
    (void)msg; /* nothing to read -- see kilnlink_announce_reboot.h */

    kilnlink_announce_reboot_status_t local_status = KILNLINK_ANNOUNCE_REBOOT_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_ANNOUNCE_REBOOT_OK;

    if (out_cap < KILNLINK_ANNOUNCE_REBOOT_LEN) {
        *status = KILNLINK_ANNOUNCE_REBOOT_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_ANNOUNCE_REBOOT_CMD;

    return KILNLINK_ANNOUNCE_REBOOT_LEN;
}

kilnlink_announce_reboot_status_t kilnlink_announce_reboot_decode(const uint8_t *payload, size_t len,
                                                                   kilnlink_announce_reboot_t *out)
{
    if (len != KILNLINK_ANNOUNCE_REBOOT_LEN) {
        return KILNLINK_ANNOUNCE_REBOOT_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_ANNOUNCE_REBOOT_CMD) {
        return KILNLINK_ANNOUNCE_REBOOT_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved0 = 0;
    }

    return KILNLINK_ANNOUNCE_REBOOT_OK;
}
