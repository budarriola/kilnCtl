#ifndef KILNLINK_CEILING_H
#define KILNLINK_CEILING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RETIRED 2026-09-24: the ESP no longer sends 0x09 and SaftyFW decodes it
 * but never tightens S1 with it (owner decision: the Pico's limits must equal
 * the ESP's). The codec is kept so an older ESP's frame still decodes cleanly.
 *
 * ESP -> Pico, SAFETY_CMD_SET_FIRING_CEILING = 0x09 -- docs/LINK_PROTOCOL.md
 * sec 4. Sent when a profile starts, when it is edited, and repeated in
 * every context frame's shadow. Carries the highest target temperature this
 * firing will ever ask for, so the Pico can tighten S1's absolute limit to
 * `min(abs_max_temp_c, firing_max_c + firing_margin_c)` instead of running
 * every firing at the hottest-ever-allowed ceiling. This codec only
 * serializes the payload bytes; it has no opinion on the clamp math, which
 * lives in SaftyFW.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). Same conventions as kilnlink_trip.c / kilnlink_power.c. */

#define KILNLINK_CEILING_CMD 0x09u
#define KILNLINK_CEILING_LEN 5u /* cmd(1) + firing_max_c f32 LE(4) */

typedef enum {
    KILNLINK_CEILING_OK = 0,
    KILNLINK_CEILING_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_CEILING_LEN */
    KILNLINK_CEILING_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_CEILING_LEN (fixed-size frame) */
    KILNLINK_CEILING_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_CEILING_CMD */
} kilnlink_ceiling_status_t;

typedef struct {
    /* 0 or NaN = "no firing / no ceiling known" -- LINK_PROTOCOL.md sec 4.
     * This codec passes the value through unmodified either way; the
     * "0 or NaN means no ceiling" interpretation belongs to the caller. */
    float firing_max_c;
} kilnlink_ceiling_t;

/* Serializes `msg` (SAFETY_CMD_SET_FIRING_CEILING payload, byte 0 = 0x09
 * included) into `out`. Always exactly KILNLINK_CEILING_LEN (5) bytes --
 * this frame has no variable-length fields. Returns 5, or 0 on
 * KILNLINK_CEILING_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_ceiling_encode(const kilnlink_ceiling_t *msg, uint8_t *out, size_t out_cap,
                                kilnlink_ceiling_status_t *status);

/* Parses a SET_FIRING_CEILING payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_CEILING_LEN -- this is untrusted input from another processor
 * across an isolated link (CommonFW/README.md rule 6). */
kilnlink_ceiling_status_t kilnlink_ceiling_decode(const uint8_t *payload, size_t len,
                                                   kilnlink_ceiling_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CEILING_H */
