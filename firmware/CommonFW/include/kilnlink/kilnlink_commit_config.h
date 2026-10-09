#ifndef KILNLINK_COMMIT_CONFIG_H
#define KILNLINK_COMMIT_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_COMMIT_CONFIG = 0x1D -- docs/LINK_PROTOCOL.md
 * sec 4, docs/COMMISSIONING.md sec 2. One byte, no arguments -- same trivial
 * fixed-length-wrapper shape as kilnlink_get_ct_cal.h. Tells SaftyFW to
 * validate everything staged by SET_PARAM (kilnlink_set_param.h) as a whole
 * and, if it passes, write one config_store record and bump config_crc.
 *
 * This codec carries no fields and does no validation of its own -- the
 * cross-field rules (e.g. CONFIG_REFERENCE.md sec 1's tc_placement_mode vs
 * tc_source) belong entirely to SaftyFW's config_store.c, per
 * COMMISSIONING.md sec 2: "Validation happens at COMMIT_CONFIG, not at
 * SET_PARAM, because the rules that matter are cross-field." Refusal (bad
 * cross-field combination, or the ARMED refusal every config write goes
 * through) is reported back on the existing diagnostic frame, not by this
 * codec, same convention as SET_CONFIG and SET_CT_CAL's fire-and-forget
 * refusal path.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_COMMIT_CONFIG_CMD 0x1Du
#define KILNLINK_COMMIT_CONFIG_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_COMMIT_CONFIG_OK = 0,
    KILNLINK_COMMIT_CONFIG_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_COMMIT_CONFIG_LEN */
    KILNLINK_COMMIT_CONFIG_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_COMMIT_CONFIG_LEN (fixed-size frame) */
    KILNLINK_COMMIT_CONFIG_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_COMMIT_CONFIG_CMD */
} kilnlink_commit_config_status_t;

/* No fields -- the payload is the command byte alone. Kept as an (empty)
 * struct anyway so this codec's encode/decode signatures match every other
 * kilnlink_<name>_t codec's shape. */
typedef struct {
    uint8_t reserved; /* unused; always 0, not part of the wire payload */
} kilnlink_commit_config_t;

/* Serializes `msg` (SAFETY_CMD_COMMIT_CONFIG payload, byte 0 = 0x1D) into
 * `out`. Always exactly KILNLINK_COMMIT_CONFIG_LEN (1) byte. `msg` may be
 * NULL, since there is nothing in it to read. Returns 1, or 0 on
 * KILNLINK_COMMIT_CONFIG_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_commit_config_encode(const kilnlink_commit_config_t *msg, uint8_t *out,
                                      size_t out_cap, kilnlink_commit_config_status_t *status);

/* Parses a COMMIT_CONFIG payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_COMMIT_CONFIG_LEN -- untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6). `out` may be NULL, since there is nothing to fill in. */
kilnlink_commit_config_status_t kilnlink_commit_config_decode(const uint8_t *payload, size_t len,
                                                                kilnlink_commit_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_COMMIT_CONFIG_H */
