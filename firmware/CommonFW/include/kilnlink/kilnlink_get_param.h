#ifndef KILNLINK_GET_PARAM_H
#define KILNLINK_GET_PARAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_PARAM = 0x1E -- docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. Fixed 3-byte request: cmd + the param_id
 * being asked about. Same request/reply-share-an-id convention as
 * GET_CT_CAL/CT_CAL and GET_FW_VERSION/FW_VERSION: the reply
 * (kilnlink_param.h's SAFETY_CMD_PARAM) is sent under this SAME command
 * byte, distinguished by direction and by carrying more than 3 bytes. The
 * Pico answers every copy it sees and never tracks whether its answer
 * arrived (LINK_PROTOCOL.md sec 2).
 *
 * `param_id` is opaque here, same split as kilnlink_set_param.h -- this
 * codec does not know the id table, only that one is being asked for.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_GET_PARAM_CMD 0x1Eu
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

/* Serializes `msg` (SAFETY_CMD_GET_PARAM payload, byte 0 = 0x1E included)
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
