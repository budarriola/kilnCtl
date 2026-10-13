#ifndef KILNLINK_CLEAR_TRIP_H
#define KILNLINK_CLEAR_TRIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_CLEAR_TRIP = 0x0A -- docs/LINK_PROTOCOL.md sec 4.
 * The GUI's path to acknowledging a trip. Refused (with the reason reported
 * in the next diagnostic frame) if the tripping condition is still true or
 * if `trip_mask` doesn't match the trip currently latched -- echoing the
 * mask back is what stops a stale "clear" queued before a second, different
 * trip from clearing that one too. This codec only serializes the payload
 * bytes; the refusal logic lives in SaftyFW.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). Same conventions as kilnlink_trip.c / kilnlink_ceiling.c. */

#define KILNLINK_CLEAR_TRIP_CMD 0x0Au
#define KILNLINK_CLEAR_TRIP_LEN 3u /* cmd(1) + trip_mask u16 LE(2) */
/* KILNLINK_PROTOCOL_VERSION 16 -> 17 (kilnlink audit 2026-10-09 M4): the
 * occurrence-bound form appends the trip_seq (u8, offset 3) the ESP read from
 * a 31-byte DIAG (kilnlink_diag.h's trip_seq). The Pico refuses a bound clear
 * whose trip_seq is not the occurrence latched right now, so a duplicated or
 * stale frame meant for occurrence N can never clear occurrence N+1 of the
 * same reason. Encoded only when has_trip_seq is set; the decoder accepts
 * both lengths. */
#define KILNLINK_CLEAR_TRIP_LEN_V2 4u /* + trip_seq u8(1) */
/* KILNLINK_PROTOCOL_VERSION 17 -> 18 (docs/TEST_TRIP_PLAN.md sec 2.2/6, F6):
 * the boot-bound form appends the pico_boot_id (u8, offset 4) the ESP read
 * from a 32-byte DIAG, so a delayed clear from a previous Pico boot cannot
 * clear the new boot trip. Always carries trip_seq too (superset of V2).
 * Encoded when has_boot_id is set; the decoder accepts 3, 4 and 5 bytes. */
#define KILNLINK_CLEAR_TRIP_LEN_V3 5u /* + trip_seq u8(1) + pico_boot_id u8(1) */

typedef enum {
    KILNLINK_CLEAR_TRIP_OK = 0,
    KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than the encoded length */
    KILNLINK_CLEAR_TRIP_ERR_LENGTH_MISMATCH,  /* input length is none of KILNLINK_CLEAR_TRIP_LEN, _LEN_V2, _LEN_V3 */
    KILNLINK_CLEAR_TRIP_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_CLEAR_TRIP_CMD */
} kilnlink_clear_trip_status_t;

typedef struct {
    uint16_t trip_mask; /* the trip mask being acknowledged; must match the latched one */
    bool     has_trip_seq; /* true: 4-byte bound form, trip_seq is on the wire */
    uint8_t  trip_seq;     /* the occurrence being acknowledged (DIAG trip_seq); valid iff has_trip_seq */
    bool     has_boot_id;  /* true: 5-byte boot-bound form (protocol 18); implies has_trip_seq */
    uint8_t  pico_boot_id; /* the Pico boot being acknowledged (DIAG pico_boot_id); valid iff has_boot_id */
} kilnlink_clear_trip_t;

/* Serializes `msg` (SAFETY_CMD_CLEAR_TRIP payload, byte 0 = 0x0A included)
 * into `out`: KILNLINK_CLEAR_TRIP_LEN_V3 (5) bytes when msg->has_boot_id
 * (trip_seq is always written in that form), else
 * KILNLINK_CLEAR_TRIP_LEN_V2 (4) bytes when msg->has_trip_seq, else
 * KILNLINK_CLEAR_TRIP_LEN (3). Returns that length, or 0 on
 * KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_clear_trip_encode(const kilnlink_clear_trip_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_clear_trip_status_t *status);

/* Parses a CLEAR_TRIP payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_CLEAR_TRIP_LEN (legacy,
 * has_trip_seq = false), KILNLINK_CLEAR_TRIP_LEN_V2 (has_trip_seq = true) or
 * KILNLINK_CLEAR_TRIP_LEN_V3 (has_trip_seq and has_boot_id = true)
 * -- this is untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_clear_trip_status_t kilnlink_clear_trip_decode(const uint8_t *payload, size_t len,
                                                         kilnlink_clear_trip_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CLEAR_TRIP_H */
