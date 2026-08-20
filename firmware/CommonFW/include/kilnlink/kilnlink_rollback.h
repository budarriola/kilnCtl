#ifndef KILNLINK_ROLLBACK_H
#define KILNLINK_ROLLBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_ROLLBACK = 0x17 -- docs/LINK_PROTOCOL.md sec 4.
 * The explicit "go back to the previously-running bootloader slot, right
 * now" command -- the Pico-side half of tools/PcTools/TODO.md's
 * `ota_rollback(processor)` line (the ESP half, POST /api/ota/esp/rollback,
 * already exists in firmware/KilnFW/App/drivers/ota_http.c). Unlike CLEAR_
 * TRIP there is no mask to echo back and no cached ESP-side state to derive
 * anything from -- this frame carries no payload at all, cmd byte only, same
 * "no payload needed" shape as SAFETY_CMD_PING.
 *
 * The refuse/proceed decision (is the other bootloader slot even valid to
 * fall back to?) is entirely SaftyFW's, in bootloader/metadata.c's
 * bootloader_decide_rollback() -- this codec only serializes the one-byte
 * frame; it carries no opinion about whether a rollback should be allowed.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_clear_trip.c / kilnlink_ceiling.c. */

#define KILNLINK_ROLLBACK_CMD 0x17u
#define KILNLINK_ROLLBACK_LEN 1u /* cmd(1), no payload */

typedef enum {
    KILNLINK_ROLLBACK_OK = 0,
    KILNLINK_ROLLBACK_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_ROLLBACK_LEN */
    KILNLINK_ROLLBACK_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_ROLLBACK_LEN (fixed-size frame) */
    KILNLINK_ROLLBACK_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_ROLLBACK_CMD */
} kilnlink_rollback_status_t;

/* No fields -- the frame carries nothing beyond its own command byte. Kept
 * as an (empty) struct rather than dropping encode/decode entirely so this
 * codec has the same call shape as every other kilnlink_*_encode/_decode
 * pair (uniform enough that link_task.c's dispatch table doesn't need a
 * special case for "the payload-less ones"). */
typedef struct {
    uint8_t reserved0; /* unused; keeps the struct non-empty for C89-style
                           tooling that dislikes a zero-sized struct. Never
                           read or written for meaning. */
} kilnlink_rollback_t;

/* Serializes `msg` (SAFETY_CMD_ROLLBACK payload, byte 0 = 0x17 included)
 * into `out`. Always exactly KILNLINK_ROLLBACK_LEN (1) byte. Returns 1, or 0
 * on KILNLINK_ROLLBACK_ERR_BUFFER_TOO_SMALL. `msg` may be NULL -- there is
 * nothing in it to read. */
size_t kilnlink_rollback_encode(const kilnlink_rollback_t *msg, uint8_t *out, size_t out_cap,
                                 kilnlink_rollback_status_t *status);

/* Parses a ROLLBACK payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_ROLLBACK_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). `out` may be NULL -- there is nothing to
 * fill beyond confirming the frame is well-formed. */
kilnlink_rollback_status_t kilnlink_rollback_decode(const uint8_t *payload, size_t len,
                                                     kilnlink_rollback_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_ROLLBACK_H */
