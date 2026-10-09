#ifndef KILNLINK_PARAM_H
#define KILNLINK_PARAM_H

#include <stddef.h>
#include <stdint.h>

#include "kilnlink/kilnlink_param_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_PARAM = 0x1E -- sent in reply to
 * SAFETY_CMD_GET_PARAM (kilnlink_get_param.h, its own id 0x23 since
 * KILNLINK_PROTOCOL_VERSION 7 -- see that header's comment). Before version
 * 7 this reply shared 0x1E with its own request, distinguished only by
 * direction and length; that scheme structurally blocked a length-different
 * refusal reply, which is why the request moved off this id. 0x1E itself is
 * unchanged and is now used ONLY by this reply.
 *
 * `found` is carried explicitly, never inferred from a sentinel value,
 * because COMMISSIONING.md sec 2 requires an unknown id to be "refused
 * individually and named in the reply, rather than the whole transfer
 * failing" -- a version-skewed ESP asking about a param_id this Pico build
 * does not have needs a real, distinguishable answer, not a decode error
 * and not a made-up zero. When found is 0, `type` and `value` are not part
 * of the wire payload at all (the frame is exactly 5 bytes); when found is
 * 1, `type` (kilnlink_param_value.h's tag set) and its value follow.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same "closed type-tag set,
 * unrecognised tag is a decode ERROR" discipline as kilnlink_set_param.h. */

#define KILNLINK_PARAM_CMD 0x1Eu
#define KILNLINK_PARAM_HDR_LEN 5u /* cmd(1) + param_id u16(2) + found(1) + type(1) */
#define KILNLINK_PARAM_MAX_LEN (KILNLINK_PARAM_HDR_LEN + 4u) /* + widest value (f32) */

typedef enum {
    KILNLINK_PARAM_OK = 0,
    KILNLINK_PARAM_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than the encoded length */
    KILNLINK_PARAM_ERR_LENGTH_MISMATCH,  /* input length != header (+ this type's value length when found) */
    KILNLINK_PARAM_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_PARAM_CMD */
    KILNLINK_PARAM_ERR_BAD_TYPE,         /* found=1 but byte 4 isn't a KILNLINK_PARAM_TYPE_* tag */
    KILNLINK_PARAM_ERR_BAD_FOUND,        /* byte 3 (found) is neither 0 nor 1 */
} kilnlink_param_status_t;

typedef struct {
    uint16_t param_id;
    uint8_t  found; /* 0 = param_id not recognised by this build; 1 = value below is meaningful */
    uint8_t  type;  /* KILNLINK_PARAM_TYPE_*; 0 (BOOL) when found == 0, ignore in that case */
    kilnlink_param_value_t value; /* meaningful only when found == 1 */
} kilnlink_param_t;

/* Serializes `msg` into `out`. When msg->found == 0, always exactly
 * KILNLINK_PARAM_HDR_LEN (5) bytes and msg->type/msg->value are not
 * inspected. When msg->found == 1, KILNLINK_PARAM_HDR_LEN plus
 * kilnlink_param_value_len(msg->type) bytes; returns 0 and
 * KILNLINK_PARAM_ERR_BAD_TYPE if msg->type is unrecognised. Returns 0 and
 * ERR_BUFFER_TOO_SMALL if `out_cap` is smaller than the resulting length. */
size_t kilnlink_param_encode(const kilnlink_param_t *msg, uint8_t *out, size_t out_cap,
                              kilnlink_param_status_t *status);

/* Parses a PARAM payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. Rejects a short frame, an over-long frame, a `found` byte that is
 * neither 0 nor 1, an unrecognised `type` tag when found == 1, and a
 * truncated-mid-value frame -- all without ever reading past
 * `payload[len - 1]`. Untrusted input from another processor across an
 * isolated link (CommonFW/README.md rule 6). */
kilnlink_param_status_t kilnlink_param_decode(const uint8_t *payload, size_t len,
                                               kilnlink_param_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_PARAM_H */
