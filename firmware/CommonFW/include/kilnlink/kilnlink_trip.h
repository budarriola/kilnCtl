#ifndef KILNLINK_TRIP_H
#define KILNLINK_TRIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame D: SAFETY_CMD_TRIP_EVENT = 0x0D --
 * docs/LINK_PROTOCOL.md sec 6. Pushed immediately the moment a trip
 * latches (not on the 500 ms cadence), repeated a few times over the next
 * second since there is no ACK; the ESP dedups on `trip_seq`.
 * "Capturing the deciding values at the instant of the trip is the whole
 * point" -- the answer to "why did the kiln stop."
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_status.c / kilnlink_power.c / kilnlink_diag.c. */

#define KILNLINK_TRIP_CMD 0x0Du
#define KILNLINK_TRIP_CHANNELS 3u
#define KILNLINK_TRIP_LEN 29u

typedef enum {
    KILNLINK_TRIP_OK = 0,
    KILNLINK_TRIP_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_TRIP_LEN */
    KILNLINK_TRIP_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_TRIP_LEN (fixed-size frame) */
    KILNLINK_TRIP_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_TRIP_CMD */
} kilnlink_trip_status_t;

typedef struct {
    uint8_t  trip_seq;              /* increments per trip event; the ESP's dedup key */
    uint8_t  trip_reason;           /* SAFETY_TRIP_* (SaftyFW's safety_guards.h) */
    uint32_t uptime_ms;             /* Pico uptime at trip */
    float    safety_tc_c;           /* safety thermocouple, degC, at trip */
    float    deciding_threshold;    /* the threshold value that decided the trip */
    float    current_a[KILNLINK_TRIP_CHANNELS]; /* current sense 1..3, amps, at trip */
    uint8_t  relay_recent_mask;     /* relay_recent_mask last received from the ESP */
    uint8_t  context_age_100ms;     /* context_age_100ms at trip */
} kilnlink_trip_t;

/* Serializes `tr` (SAFETY_CMD_TRIP_EVENT payload, byte 0 = 0x0D included)
 * into `out`. Always exactly KILNLINK_TRIP_LEN (29) bytes -- this frame has
 * no variable-length fields. Returns 29, or 0 on
 * KILNLINK_TRIP_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_trip_encode(const kilnlink_trip_t *tr, uint8_t *out, size_t out_cap,
                            kilnlink_trip_status_t *status);

/* Parses a TRIP_EVENT payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_TRIP_LEN -- this is untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_trip_status_t kilnlink_trip_decode(const uint8_t *payload, size_t len,
                                            kilnlink_trip_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_TRIP_H */
