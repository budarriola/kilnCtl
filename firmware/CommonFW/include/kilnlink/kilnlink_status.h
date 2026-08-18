#ifndef KILNLINK_STATUS_H
#define KILNLINK_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame A: SAFETY_CMD_GET_STATUS = 0x01 --
 * docs/LINK_PROTOCOL.md sec 6. Byte-for-byte the existing 23-byte layout
 * KilnFW already parses (safety_link.h SAFETY_LINK_STATUS_FRAME_LEN = 23),
 * so this codec can be brought up against today's unmodified KilnFW.
 *
 * Only Frame A is implemented here (the one the protocol doc marks
 * "required" and gives a fully concrete, fixed-size layout for). Frames
 * B (DIAG), C (FW_VERSION), D (TRIP_EVENT), E (POWER) and the LOG relay
 * are documented in LINK_PROTOCOL.md sec 6 but not yet coded -- see
 * CommonFW/README.md's completion checklist.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_STATUS_CMD 0x01u
#define KILNLINK_STATUS_LEN 23u

/* Flags byte (offset 1). Bits 0-1 (LINK_UP, FAULT) are the ESP's own -- the
 * Pico always sends them as 0; they are defined here only so a decoder can
 * name them, not because the Pico ever sets them. */
typedef enum {
    KILNLINK_STATUS_FLAG_LINK_UP = 0x01u, /* ESP-owned; Pico always sends 0 */
    KILNLINK_STATUS_FLAG_FAULT = 0x02u,   /* ESP-owned; Pico always sends 0 */
    KILNLINK_STATUS_FLAG_ESTOP = 0x04u,
    KILNLINK_STATUS_FLAG_RELAY = 0x08u,
    KILNLINK_STATUS_FLAG_ENABLED = 0x10u,
    KILNLINK_STATUS_FLAG_TEMP_VALID = 0x20u,
} kilnlink_status_flag_t;

typedef enum {
    KILNLINK_STATUS_OK = 0,
    KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_STATUS_LEN */
    KILNLINK_STATUS_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_STATUS_LEN (fixed-size frame) */
    KILNLINK_STATUS_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_STATUS_CMD */
} kilnlink_status_status_t;

typedef struct {
    uint8_t flags; /* kilnlink_status_flag_t bits */
    float safety_tc_c;    /* NaN if invalid */
    float cold_junction_c; /* NaN if invalid */
    uint8_t tc_fault;      /* THERMO_FAULT_* bits */
    float current1_a;
    float current2_a;
    float current3_a;
} kilnlink_status_t;

/* Serializes `st` (SAFETY_CMD_GET_STATUS payload, byte 0 = 0x01 included)
 * into `out`. Always exactly KILNLINK_STATUS_LEN (23) bytes -- this frame
 * has no variable-length fields. Returns 23, or 0 on
 * KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_status_encode(const kilnlink_status_t *st, uint8_t *out, size_t out_cap,
                              kilnlink_status_status_t *status);

/* Parses a GET_STATUS payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_STATUS_LEN -- this is untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_status_status_t kilnlink_status_decode(const uint8_t *payload, size_t len,
                                                kilnlink_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_STATUS_H */
