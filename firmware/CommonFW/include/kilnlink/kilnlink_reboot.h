#ifndef KILNLINK_REBOOT_H
#define KILNLINK_REBOOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_REBOOT = 0x29 -- docs/LINK_PROTOCOL.md sec 4.
 * "Reboot yourself, in place, into the SAME firmware slot you are running
 * now." The Pico half of KilnFW's POST /api/sw_reset (sw_reset_http.c,
 * owner request 2026-09-08: a reset-menu item that reboots BOTH processors
 * without touching configuration).
 *
 * NOT SAFETY_CMD_ROLLBACK (0x17, kilnlink_rollback.h): that command marks
 * the running slot BAD, writes a new bootloader metadata record to flash,
 * and comes back up on the OTHER image -- a different, possibly-refused
 * firmware. This one writes NOTHING: SaftyFW's update_task_request_reboot()
 * reads the relay state, logs, and calls hal_wdt_reboot(). No flash write,
 * no config_store call, no metadata record, no slot change.
 *
 * NOT SAFETY_CMD_ANNOUNCE_REBOOT (0x18, kilnlink_announce_reboot.h) either:
 * that frame is a courtesy notice about the ESP's OWN imminent reboot and
 * instructs the Pico to do nothing at all.
 *
 * Refusable, exactly like ROLLBACK: SaftyFW refuses while the relay is
 * energized (ARMED), the same gate config writes and rollback already use.
 * The refusal -- and, unlike ROLLBACK, the ACCEPTANCE too -- is reported on
 * the wire by SAFETY_CMD_REBOOT_RESULT (0x2A, kilnlink_reboot_result.h);
 * see that header for why this one CAN honestly ACK its acceptance where
 * ROLLBACK structurally cannot.
 *
 * No payload at all -- 1 byte, cmd only, same shape as SAFETY_CMD_ROLLBACK/
 * SAFETY_CMD_ANNOUNCE_REBOOT. Freestanding C11, no allocation, no I/O, no
 * globals, every decoder bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_REBOOT_CMD 0x29u
#define KILNLINK_REBOOT_LEN 1u /* cmd(1), no payload */

typedef enum {
    KILNLINK_REBOOT_OK = 0,
    KILNLINK_REBOOT_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_REBOOT_LEN */
    KILNLINK_REBOOT_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_REBOOT_LEN (fixed-size frame) */
    KILNLINK_REBOOT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_REBOOT_CMD */
} kilnlink_reboot_status_t;

/* No fields -- the frame carries nothing beyond its own command byte. Kept
 * as an (empty) struct for call-shape uniformity, same reasoning as
 * kilnlink_rollback_t. */
typedef struct {
    uint8_t reserved0; /* unused; never read or written for meaning. */
} kilnlink_reboot_t;

/* Serializes `msg` (SAFETY_CMD_REBOOT payload, byte 0 = 0x29 included) into
 * `out`. Always exactly KILNLINK_REBOOT_LEN (1) byte. Returns 1, or 0 on
 * KILNLINK_REBOOT_ERR_BUFFER_TOO_SMALL. `msg` may be NULL. */
size_t kilnlink_reboot_encode(const kilnlink_reboot_t *msg, uint8_t *out, size_t out_cap,
                              kilnlink_reboot_status_t *status);

/* Parses a REBOOT payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. `len` must be exactly KILNLINK_REBOOT_LEN -- untrusted input from
 * another processor across an isolated link (CommonFW/README.md rule 6).
 * `out` may be NULL. */
kilnlink_reboot_status_t kilnlink_reboot_decode(const uint8_t *payload, size_t len,
                                                kilnlink_reboot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_REBOOT_H */
