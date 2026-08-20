#ifndef KILNLINK_ANNOUNCE_REBOOT_H
#define KILNLINK_ANNOUNCE_REBOOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_ANNOUNCE_REBOOT = 0x18 -- docs/LINK_PROTOCOL.md
 * sec 4. Fire-and-forget, unsolicited notice sent immediately before the ESP
 * calls esp_restart() for a routine OTA self-update: "I am about to go
 * silent for a few seconds on purpose, this is not a crash." SaftyFW's
 * link_task.c records the arrival time and safety_guards.c's S6b (link-dead
 * guard, SAFETY_MODEL.md section 4) suppresses its OWN TRIP -- not the
 * underlying link_up fact, not any other guard -- for a bounded window after
 * receipt. See firmware/SaftyFW/src/safety_guards.c's S6b block and
 * firmware/SaftyFW/src/tasks/safety_core.c for the window arithmetic.
 *
 * This is NOT a permission grant. It has no path into relay_owner's
 * energize/ARM logic (safety_guards.c has no link/GPIO access at all, by
 * this codebase's own isolation rule -- see check_isolation.ps1) and if the
 * window expires while the link is still down, S6b trips exactly as if this
 * frame had never arrived.
 *
 * No payload at all -- 1 byte, cmd only, same shape as SAFETY_CMD_ROLLBACK/
 * SAFETY_CMD_PING. Freestanding C11, no allocation, no I/O, no globals,
 * every decoder bounds-checked -- CommonFW/README.md rules 1-6. Same
 * conventions as kilnlink_rollback.c / kilnlink_clear_trip.c. */

#define KILNLINK_ANNOUNCE_REBOOT_CMD 0x18u
#define KILNLINK_ANNOUNCE_REBOOT_LEN 1u /* cmd(1), no payload */

typedef enum {
    KILNLINK_ANNOUNCE_REBOOT_OK = 0,
    KILNLINK_ANNOUNCE_REBOOT_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_ANNOUNCE_REBOOT_LEN */
    KILNLINK_ANNOUNCE_REBOOT_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_ANNOUNCE_REBOOT_LEN (fixed-size frame) */
    KILNLINK_ANNOUNCE_REBOOT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_ANNOUNCE_REBOOT_CMD */
} kilnlink_announce_reboot_status_t;

/* No fields -- the frame carries nothing beyond its own command byte. Kept
 * as an (empty) struct rather than dropping encode/decode entirely so this
 * codec has the same call shape as every other kilnlink_*_encode/_decode
 * pair, same reasoning as kilnlink_rollback_t. */
typedef struct {
    uint8_t reserved0; /* unused; keeps the struct non-empty for C89-style
                           tooling that dislikes a zero-sized struct. Never
                           read or written for meaning. */
} kilnlink_announce_reboot_t;

/* Serializes `msg` (SAFETY_CMD_ANNOUNCE_REBOOT payload, byte 0 = 0x18
 * included) into `out`. Always exactly KILNLINK_ANNOUNCE_REBOOT_LEN (1)
 * byte. Returns 1, or 0 on KILNLINK_ANNOUNCE_REBOOT_ERR_BUFFER_TOO_SMALL.
 * `msg` may be NULL -- there is nothing in it to read. */
size_t kilnlink_announce_reboot_encode(const kilnlink_announce_reboot_t *msg, uint8_t *out, size_t out_cap,
                                        kilnlink_announce_reboot_status_t *status);

/* Parses an ANNOUNCE_REBOOT payload (as extracted from kilnlink_frame_t::
 * payload) into `out`. `len` must be exactly KILNLINK_ANNOUNCE_REBOOT_LEN --
 * this is untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). `out` may be NULL -- there is nothing to fill
 * beyond confirming the frame is well-formed. */
kilnlink_announce_reboot_status_t kilnlink_announce_reboot_decode(const uint8_t *payload, size_t len,
                                                                   kilnlink_announce_reboot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_ANNOUNCE_REBOOT_H */
