#ifndef KILNLINK_TEST_TRIP_RESULT_H
#define KILNLINK_TEST_TRIP_RESULT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_TEST_TRIP_RESULT = 0x2F -- docs/LINK_PROTOCOL.md
 * sec 4, docs/TEST_TRIP_PLAN.md sec 2.1 (KILNLINK_PROTOCOL_VERSION 17 -> 18).
 * The reply to SAFETY_CMD_TEST_TRIP (0x2E, kilnlink_test_trip.h), sent as a
 * short repeated burst like REBOOT_RESULT; the Pico never waits for the ESP.
 * Request-triggered: a Pico emits it only in answer to a 0x2E, which only
 * an 18 ESP sends, so no peer-version gate is needed on this direction.
 *
 * The outcome is a CLOSED enum on the wire: any value outside it fails
 * decode (ERR_BAD_OUTCOME) rather than aliasing onto a real outcome
 * (CommonFW/README.md rule 6).
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_TEST_TRIP_RESULT_CMD 0x2Fu
#define KILNLINK_TEST_TRIP_RESULT_LEN 4u /* cmd(1) + request_id u8 + outcome u8 + trip_seq u8 */

typedef enum {
    KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED = 0,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_ALREADY_TRIPPED = 1,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BOOT_ID = 2,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_PEER_VERSION = 3,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_RATE_LIMIT = 4,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_UPDATING = 5,
    KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BAD_FRAME = 6,
    KILNLINK_TEST_TRIP_OUTCOME_DUPLICATE = 7,
} kilnlink_test_trip_outcome_t;

/* Highest valid outcome value (for range checks). */
#define KILNLINK_TEST_TRIP_OUTCOME_MAX 7u

typedef enum {
    KILNLINK_TEST_TRIP_RESULT_OK = 0,
    KILNLINK_TEST_TRIP_RESULT_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than the frame */
    KILNLINK_TEST_TRIP_RESULT_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_TEST_TRIP_RESULT_LEN */
    KILNLINK_TEST_TRIP_RESULT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_TEST_TRIP_RESULT_CMD */
    KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME,      /* outcome byte outside the closed enum */
} kilnlink_test_trip_result_status_t;

typedef struct {
    uint8_t request_id; /* echoed from the request */
    uint8_t outcome;    /* kilnlink_test_trip_outcome_t */
    uint8_t trip_seq;   /* latched test trip's trip_seq when ACCEPTED (or DUPLICATE of an accepted one), else 0 */
} kilnlink_test_trip_result_t;

/* Serializes `msg` into `out` (4 bytes, byte 0 = 0x2F). Returns 4, or 0 on
 * ERR_BUFFER_TOO_SMALL. An outcome outside the closed enum is refused with
 * ERR_BAD_OUTCOME (returns 0): the Pico must never put an unknown value on
 * the wire. */
size_t kilnlink_test_trip_result_encode(const kilnlink_test_trip_result_t *msg, uint8_t *out,
                                        size_t out_cap, kilnlink_test_trip_result_status_t *status);

/* Parses a TEST_TRIP_RESULT payload. `len` must be exactly
 * KILNLINK_TEST_TRIP_RESULT_LEN and the outcome inside the closed enum --
 * untrusted input across an isolated link (README rule 6). */
kilnlink_test_trip_result_status_t kilnlink_test_trip_result_decode(const uint8_t *payload, size_t len,
                                                                    kilnlink_test_trip_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_TEST_TRIP_RESULT_H */
