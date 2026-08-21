#ifndef KILNLINK_SET_CT_CAL_H
#define KILNLINK_SET_CT_CAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_SET_CT_CAL = 0x19 -- docs/LINK_PROTOCOL.md sec 4.
 * The GUI/bench-tool's path to commissioning one channel of SaftyFW's
 * config_store.h ct_cal record (firmware/SimFW/tools/ct_calibration/'s
 * report: "no path exists to push calibration constants back into SaftyFW's
 * own flash"). Same fire-and-forget shape as SAFETY_CMD_SET_CONFIG: never
 * ACKed on the wire, the PC observes the outcome via the next diag poll.
 *
 * One channel per frame, not all three at once -- a bench calibration run
 * sweeps and fits one CT channel at a time
 * (firmware/SimFW/tools/ct_calibration/calibrate_ct.py), and per-channel
 * independence (setting channel 0 must never disturb channel 1's stored
 * constants) is a load-bearing property here, not an implementation detail:
 * S3/S4/S9 read all three channels' current, and silently corrupting one
 * channel's calibration while commissioning another would be exactly the
 * kind of quiet magnitude error those guards must never see.
 *
 * `gain`/`offset` are carried as opaque floats here -- this codec has no
 * opinion on units or on which direction the linear fit runs (SaftyFW's
 * config_store.h/current_sense.c own that, same "this codec only serializes
 * bytes, the receiver decides what they mean" split kilnlink_set_config.h's
 * tc_type uses). `calibrated` is carried explicitly, not inferred from
 * gain/offset being non-default -- see config_store.h's header comment on
 * why "uncalibrated" must be its own signal, not a gain==1/offset==0
 * convention.
 *
 * Refused (reason logged on the Pico side, same as SET_CONFIG) if the relay
 * is currently ARMED (config_store_decide_write()) or if `channel` is out of
 * range -- both refusal decisions belong to SaftyFW, not this codec.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). Same conventions as kilnlink_set_config.c/kilnlink_status.c
 * (this one uses kilnlink_bytes.h's f32 helpers, same as kilnlink_status.c,
 * for its two float fields). */

#define KILNLINK_SET_CT_CAL_CMD 0x19u
#define KILNLINK_SET_CT_CAL_NUM_CHANNELS 3u
#define KILNLINK_SET_CT_CAL_LEN 11u /* cmd(1)+channel(1)+calibrated(1)+gain f32(4)+offset f32(4) */

typedef enum {
    KILNLINK_SET_CT_CAL_OK = 0,
    KILNLINK_SET_CT_CAL_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_SET_CT_CAL_LEN */
    KILNLINK_SET_CT_CAL_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_SET_CT_CAL_LEN (fixed-size frame) */
    KILNLINK_SET_CT_CAL_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_SET_CT_CAL_CMD */
} kilnlink_set_ct_cal_status_t;

typedef struct {
    uint8_t channel;    /* 0..KILNLINK_SET_CT_CAL_NUM_CHANNELS-1; receiver validates range */
    uint8_t calibrated; /* 0/1, opaque here -- same split as tc_type above */
    float   gain;
    float   offset;
} kilnlink_set_ct_cal_t;

/* Serializes `msg` (SAFETY_CMD_SET_CT_CAL payload, byte 0 = 0x19 included)
 * into `out`. Always exactly KILNLINK_SET_CT_CAL_LEN (11) bytes -- this frame
 * has no variable-length fields. Returns 11, or 0 on
 * KILNLINK_SET_CT_CAL_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_set_ct_cal_encode(const kilnlink_set_ct_cal_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_set_ct_cal_status_t *status);

/* Parses a SET_CT_CAL payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_SET_CT_CAL_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). Does NOT validate `channel`'s range -- that is
 * the receiver's job (this codec only serializes bytes). */
kilnlink_set_ct_cal_status_t kilnlink_set_ct_cal_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_set_ct_cal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_SET_CT_CAL_H */
