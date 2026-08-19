#ifndef KILNLINK_SET_CLOCK_H
#define KILNLINK_SET_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_SET_CLOCK = 0x0C, optional -- docs/LINK_PROTOCOL.md
 * sec 4. The Pico has no RTC; without this every trip is timestamped only in
 * milliseconds-since-boot. Purely diagnostic -- LINK_PROTOCOL.md is explicit
 * that no guard may ever read this clock, so a bad time from the ESP never
 * becomes a safety input. This codec only serializes the payload bytes; it
 * has no opinion on how (or whether) SaftyFW uses the value.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). Same conventions as kilnlink_trip.c / kilnlink_ceiling.c. */

#define KILNLINK_SET_CLOCK_CMD 0x0Cu
#define KILNLINK_SET_CLOCK_LEN 9u /* cmd(1) + epoch_ms u64 LE(8) */

typedef enum {
    KILNLINK_SET_CLOCK_OK = 0,
    KILNLINK_SET_CLOCK_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_SET_CLOCK_LEN */
    KILNLINK_SET_CLOCK_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_SET_CLOCK_LEN (fixed-size frame) */
    KILNLINK_SET_CLOCK_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_SET_CLOCK_CMD */
} kilnlink_set_clock_status_t;

typedef struct {
    uint64_t epoch_ms; /* Unix epoch milliseconds */
} kilnlink_set_clock_t;

/* Serializes `msg` (SAFETY_CMD_SET_CLOCK payload, byte 0 = 0x0C included)
 * into `out`. Always exactly KILNLINK_SET_CLOCK_LEN (9) bytes -- this frame
 * has no variable-length fields. Returns 9, or 0 on
 * KILNLINK_SET_CLOCK_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_set_clock_encode(const kilnlink_set_clock_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_set_clock_status_t *status);

/* Parses a SET_CLOCK payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_SET_CLOCK_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_set_clock_status_t kilnlink_set_clock_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_set_clock_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_SET_CLOCK_H */
