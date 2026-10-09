#ifndef KILNLINK_CLEAR_TRIP_H
#define KILNLINK_CLEAR_TRIP_H

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

typedef enum {
    KILNLINK_CLEAR_TRIP_OK = 0,
    KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_CLEAR_TRIP_LEN */
    KILNLINK_CLEAR_TRIP_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_CLEAR_TRIP_LEN (fixed-size frame) */
    KILNLINK_CLEAR_TRIP_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_CLEAR_TRIP_CMD */
} kilnlink_clear_trip_status_t;

typedef struct {
    uint16_t trip_mask; /* the trip mask being acknowledged; must match the latched one */
} kilnlink_clear_trip_t;

/* Serializes `msg` (SAFETY_CMD_CLEAR_TRIP payload, byte 0 = 0x0A included)
 * into `out`. Always exactly KILNLINK_CLEAR_TRIP_LEN (3) bytes -- this frame
 * has no variable-length fields. Returns 3, or 0 on
 * KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_clear_trip_encode(const kilnlink_clear_trip_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_clear_trip_status_t *status);

/* Parses a CLEAR_TRIP payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_CLEAR_TRIP_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_clear_trip_status_t kilnlink_clear_trip_decode(const uint8_t *payload, size_t len,
                                                         kilnlink_clear_trip_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CLEAR_TRIP_H */
