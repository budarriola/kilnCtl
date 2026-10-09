#ifndef KILNLINK_CT_CAL_H
#define KILNLINK_CT_CAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_CT_CAL = 0x1A -- docs/LINK_PROTOCOL.md sec 6. Sent
 * in reply to SAFETY_CMD_GET_CT_CAL (kilnlink_get_ct_cal.h, its own id
 * 0x22 since KILNLINK_PROTOCOL_VERSION 7 -- see that header's comment).
 * Before version 7 this reply shared 0x1A with its own request,
 * distinguished only by direction and length; that scheme structurally
 * blocked a length-different refusal reply, which is why the request moved
 * off this id. 0x1A itself is unchanged and is now used ONLY by this reply.
 *
 * Reports the three current-sense channels' stored CT amps calibration
 * exactly as config_store.c holds it (config_store_ct_channel_cal_t) -- the
 * GUI's way to show "what is SaftyFW actually correcting its current
 * readings with right now" without trusting a separate record of what a
 * bench tool last uploaded, same motivation as Frame C's config_crc field.
 *
 * `calibrated` is carried explicitly per channel, not inferred from
 * gain/offset -- an uncalibrated channel's gain/offset are meaningless
 * (current_sense.c never reads them) and must not be displayed or trusted
 * as if they were a real correction. See config_store.h's header comment.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_power.c: byte-for-byte little-endian packing via
 * kilnlink_bytes.h, fixed-size frame. */

#define KILNLINK_CT_CAL_CMD 0x1Au
#define KILNLINK_CT_CAL_NUM_CHANNELS 3u
#define KILNLINK_CT_CAL_CHANNEL_LEN 9u /* calibrated u8(1) + gain f32(4) + offset f32(4) */
#define KILNLINK_CT_CAL_LEN \
    (1u + KILNLINK_CT_CAL_NUM_CHANNELS * KILNLINK_CT_CAL_CHANNEL_LEN) /* 28 */

typedef enum {
    KILNLINK_CT_CAL_OK = 0,
    KILNLINK_CT_CAL_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_CT_CAL_LEN */
    KILNLINK_CT_CAL_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_CT_CAL_LEN (fixed-size frame) */
    KILNLINK_CT_CAL_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_CT_CAL_CMD */
} kilnlink_ct_cal_status_t;

typedef struct {
    uint8_t calibrated; /* 0/1 -- see this file's header comment */
    float   gain;
    float   offset;
} kilnlink_ct_cal_channel_t;

typedef struct {
    kilnlink_ct_cal_channel_t channels[KILNLINK_CT_CAL_NUM_CHANNELS];
} kilnlink_ct_cal_t;

/* Serializes `cal` (SAFETY_CMD_CT_CAL payload, byte 0 = 0x1A included) into
 * `out`. Always exactly KILNLINK_CT_CAL_LEN (28) bytes -- this frame has no
 * variable-length fields. Returns 28, or 0 on
 * KILNLINK_CT_CAL_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_ct_cal_encode(const kilnlink_ct_cal_t *cal, uint8_t *out, size_t out_cap,
                               kilnlink_ct_cal_status_t *status);

/* Parses a CT_CAL payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. `len` must be exactly KILNLINK_CT_CAL_LEN -- this is untrusted input
 * from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_ct_cal_status_t kilnlink_ct_cal_decode(const uint8_t *payload, size_t len,
                                                 kilnlink_ct_cal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CT_CAL_H */
