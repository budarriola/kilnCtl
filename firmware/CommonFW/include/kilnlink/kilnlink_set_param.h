#ifndef KILNLINK_SET_PARAM_H
#define KILNLINK_SET_PARAM_H

#include <stddef.h>
#include <stdint.h>

#include "kilnlink/kilnlink_param_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_SET_PARAM = 0x1C -- docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. Field-addressed staging: one parameter per
 * frame, `param_id` naming which of CONFIG_REFERENCE.md secs 1-5's tunables
 * is being set. **Stages only -- nothing reaches flash from this frame
 * alone.** COMMIT_CONFIG (kilnlink_commit_config.h) is what validates the
 * staged set as a whole and writes it; that split exists because the rules
 * that matter (tc_placement_mode vs tc_source, sec 1) are cross-field and
 * cannot be checked one SET_PARAM at a time (COMMISSIONING.md sec 2).
 *
 * Variable-length payload: the value's wire length depends on `type`
 * (kilnlink_param_value.h) -- 1 byte for BOOL/U8, 2 for U16, 4 for F32. A
 * `type` this build does not recognise is a decode ERROR
 * (KILNLINK_SET_PARAM_ERR_BAD_TYPE), never a guess at how many bytes to
 * read next -- the whole reason kilnlink_param_value.h exists as a small
 * closed tag set rather than a length-prefixed blob.
 *
 * `param_id` itself is carried as an opaque uint16_t, same "this codec only
 * serializes bytes, the receiver assigns meaning" split kilnlink_set_config.c
 * documents for tc_type: which numeric id maps to which CONFIG_REFERENCE.md
 * field is SaftyFW's config_store.h's concern, not this codec's, exactly so
 * a version-tolerant peer (COMMISSIONING.md sec 2, "an ESP that knows fewer
 * parameters than the Pico still works") can refuse an id it doesn't
 * recognise without this codec needing to know the id table at all.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_SET_PARAM_CMD 0x1Cu
#define KILNLINK_SET_PARAM_HDR_LEN 4u /* cmd(1) + param_id u16(2) + type(1) */
#define KILNLINK_SET_PARAM_MAX_LEN (KILNLINK_SET_PARAM_HDR_LEN + 4u) /* + widest value (f32) */

typedef enum {
    KILNLINK_SET_PARAM_OK = 0,
    KILNLINK_SET_PARAM_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than the encoded length */
    KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH,  /* input length != header + this type's value length */
    KILNLINK_SET_PARAM_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_SET_PARAM_CMD */
    KILNLINK_SET_PARAM_ERR_BAD_TYPE,         /* byte 3 isn't a KILNLINK_PARAM_TYPE_* tag */
} kilnlink_set_param_status_t;

typedef struct {
    uint16_t param_id;
    uint8_t  type; /* KILNLINK_PARAM_TYPE_* */
    kilnlink_param_value_t value;
} kilnlink_set_param_t;

/* Serializes `msg` (SAFETY_CMD_SET_PARAM payload, byte 0 = 0x1C included)
 * into `out`. Length is KILNLINK_SET_PARAM_HDR_LEN plus
 * kilnlink_param_value_len(msg->type) -- 5..8 bytes. Returns 0 and
 * KILNLINK_SET_PARAM_ERR_BAD_TYPE if msg->type is unrecognised (checked
 * BEFORE anything is written), or 0 and ERR_BUFFER_TOO_SMALL if `out_cap`
 * is smaller than the resulting length. */
size_t kilnlink_set_param_encode(const kilnlink_set_param_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_set_param_status_t *status);

/* Parses a SET_PARAM payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly the header (4) plus the value length
 * implied by byte 3's type tag -- a short frame, an over-long frame, a
 * truncated-mid-value frame, and a frame with an unrecognised type tag are
 * all distinct rejected cases (see the status enum), and none of them ever
 * reads past `payload[len - 1]`. Untrusted input from another processor
 * across an isolated link (CommonFW/README.md rule 6). */
kilnlink_set_param_status_t kilnlink_set_param_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_set_param_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_SET_PARAM_H */
