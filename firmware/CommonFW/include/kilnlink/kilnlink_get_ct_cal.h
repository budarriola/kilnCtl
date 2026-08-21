#ifndef KILNLINK_GET_CT_CAL_H
#define KILNLINK_GET_CT_CAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_CT_CAL = 0x1A -- docs/LINK_PROTOCOL.md sec 4.
 * One byte, no arguments -- same shape and same request/reply-share-an-id
 * convention as SAFETY_CMD_GET_FW_VERSION/kilnlink_get_fw_version.h: the
 * reply (kilnlink_ct_cal.h's SAFETY_CMD_CT_CAL) is sent under the SAME
 * command byte, distinguished by direction and length (this request is
 * always exactly 1 byte; the reply is always KILNLINK_CT_CAL_LEN). The Pico
 * answers every copy it sees and never tracks whether its answer arrived,
 * same as GET_FW_VERSION (LINK_PROTOCOL.md sec 2).
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. There is no payload beyond
 * the command byte, so this codec is a trivial fixed-length wrapper -- still
 * going through the encode/decode + status-enum shape every other kilnlink
 * codec uses, rather than a special case. */

#define KILNLINK_GET_CT_CAL_CMD 0x1Au
#define KILNLINK_GET_CT_CAL_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_GET_CT_CAL_OK = 0,
    KILNLINK_GET_CT_CAL_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_GET_CT_CAL_LEN */
    KILNLINK_GET_CT_CAL_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_GET_CT_CAL_LEN (fixed-size frame) */
    KILNLINK_GET_CT_CAL_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_GET_CT_CAL_CMD */
} kilnlink_get_ct_cal_status_t;

/* No fields -- the payload is the command byte alone. Kept as an (empty)
 * struct anyway so this codec's encode/decode signatures match every other
 * kilnlink_<name>_t codec's shape. */
typedef struct {
    uint8_t reserved; /* unused; always 0, not part of the wire payload */
} kilnlink_get_ct_cal_t;

/* Serializes `msg` (SAFETY_CMD_GET_CT_CAL payload, byte 0 = 0x1A) into
 * `out`. Always exactly KILNLINK_GET_CT_CAL_LEN (1) byte. `msg` may be NULL,
 * since there is nothing in it to read. Returns 1, or 0 on
 * KILNLINK_GET_CT_CAL_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_get_ct_cal_encode(const kilnlink_get_ct_cal_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_get_ct_cal_status_t *status);

/* Parses a GET_CT_CAL payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_GET_CT_CAL_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). `out` may be NULL, since there is nothing to
 * fill in. */
kilnlink_get_ct_cal_status_t kilnlink_get_ct_cal_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_get_ct_cal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_CT_CAL_H */
