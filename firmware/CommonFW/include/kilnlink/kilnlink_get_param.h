#ifndef KILNLINK_GET_PARAM_H
#define KILNLINK_GET_PARAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_PARAM = 0x23 -- docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. Fixed 3-byte request: cmd + the param_id
 * being asked about.
 *
 * Was 0x1E, sharing that command byte with its own reply (kilnlink_param.h's
 * SAFETY_CMD_PARAM), distinguished only by direction and length. Moved to
 * its own id (KILNLINK_PROTOCOL_VERSION 6 -> 7), same reasoning and same
 * pass as GET_CT_CAL/CT_CAL (kilnlink_get_ct_cal.h): a shared id structurally
 * blocks a length-different refusal reply. 0x1E is now used ONLY by the
 * reply (kilnlink_param.h) -- see docs/LINK_PROTOCOL.md's "Request/reply ids
 * must never be shared" rule. The Pico answers every copy it sees and never
 * tracks whether its answer arrived (LINK_PROTOCOL.md sec 2).
 *
 * `param_id` is opaque here, same split as kilnlink_set_param.h -- this
 * codec does not know the id table, only that one is being asked for.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_GET_PARAM_CMD 0x23u
#define KILNLINK_GET_PARAM_LEN 3u /* cmd(1) + param_id u16(2) */

typedef enum {
    KILNLINK_GET_PARAM_OK = 0,
    KILNLINK_GET_PARAM_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_GET_PARAM_LEN */
    KILNLINK_GET_PARAM_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_GET_PARAM_LEN (fixed-size frame) */
    KILNLINK_GET_PARAM_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_GET_PARAM_CMD */
} kilnlink_get_param_status_t;

typedef struct {
    uint16_t param_id;
} kilnlink_get_param_t;

/* Serializes `msg` (SAFETY_CMD_GET_PARAM payload, byte 0 = 0x23 included)
 * into `out`. Always exactly KILNLINK_GET_PARAM_LEN (3) bytes. Returns 3, or
 * 0 on KILNLINK_GET_PARAM_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_get_param_encode(const kilnlink_get_param_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_get_param_status_t *status);

/* Parses a GET_PARAM payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_GET_PARAM_LEN -- untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_get_param_status_t kilnlink_get_param_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_get_param_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_PARAM_H */
