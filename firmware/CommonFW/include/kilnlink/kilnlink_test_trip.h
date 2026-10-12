#ifndef KILNLINK_TEST_TRIP_H
#define KILNLINK_TEST_TRIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_TEST_TRIP = 0x2E -- docs/LINK_PROTOCOL.md sec 4,
 * docs/TEST_TRIP_PLAN.md sec 2.1 (KILNLINK_PROTOCOL_VERSION 17 -> 18).
 * Asks the Pico to latch a dedicated test trip (SAFETY_TRIP_TEST = 4)
 * through its real trip path. The command can only CAUSE a trip; it never
 * clears or masks one. This codec only serializes the payload bytes; the
 * acceptance/refusal logic lives in SaftyFW, and the request is sent only
 * to a Pico known to be >= 18. The reply is SAFETY_CMD_TEST_TRIP_RESULT
 * (0x2F, kilnlink_test_trip_result.h), a separate id per LINK_PROTOCOL.md's
 * "request/reply ids must never be shared" rule.
 *
 * The magic byte is carried verbatim, NOT validated by the codec: a wrong
 * magic is a Pico-side REFUSED_BAD_FRAME outcome, so the decoder must hand
 * it up rather than fail.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_TEST_TRIP_CMD 0x2Eu
#define KILNLINK_TEST_TRIP_LEN 4u /* cmd(1) + pico_boot_id u8 + request_id u8 + magic u8 */
#define KILNLINK_TEST_TRIP_MAGIC 0xA5u

typedef enum {
    KILNLINK_TEST_TRIP_OK = 0,
    KILNLINK_TEST_TRIP_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_TEST_TRIP_LEN */
    KILNLINK_TEST_TRIP_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_TEST_TRIP_LEN (fixed-size frame) */
    KILNLINK_TEST_TRIP_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_TEST_TRIP_CMD */
} kilnlink_test_trip_status_t;

typedef struct {
    uint8_t pico_boot_id; /* the Pico boot_id the ESP read from the DIAG it checked */
    uint8_t request_id;   /* chosen by the ESP; echoed by the reply, dedup key for retries */
    uint8_t magic;        /* KILNLINK_TEST_TRIP_MAGIC on a well-formed request */
} kilnlink_test_trip_t;

/* Serializes `msg` (byte 0 = 0x2E included) into `out`. Always exactly
 * KILNLINK_TEST_TRIP_LEN bytes. Returns 4, or 0 on ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_test_trip_encode(const kilnlink_test_trip_t *msg, uint8_t *out, size_t out_cap,
                                 kilnlink_test_trip_status_t *status);

/* Parses a TEST_TRIP payload. `len` must be exactly KILNLINK_TEST_TRIP_LEN
 * -- untrusted input across an isolated link (README rule 6). */
kilnlink_test_trip_status_t kilnlink_test_trip_decode(const uint8_t *payload, size_t len,
                                                      kilnlink_test_trip_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_TEST_TRIP_H */
