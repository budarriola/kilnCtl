#ifndef KILNLINK_SET_CONFIG_H
#define KILNLINK_SET_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_SET_CONFIG = 0x16 -- docs/LINK_PROTOCOL.md sec 4.
 * The GUI's path to commissioning config_store.h's tc_type (SaftyFW
 * TODO.md Phase 9's "SAFETY_CMD_SET_CONFIG (wire command) -- still not
 * done"). Refused (with the reason reported in the next diagnostic frame,
 * same convention as CLEAR_TRIP) if the relay is currently ARMED
 * (config_store_decide_write()) or if tc_type is not a value the receiver
 * recognises -- both refusal decisions belong to SaftyFW, not this codec.
 *
 * `tc_type` is carried as an opaque uint8_t here rather than an enum:
 * MAX31856_TC_TYPE_* lives in SaftyFW's application-layer max31856.h, and
 * this module must stay as dependency-free as kilnlink_clear_trip.c is of
 * SaftyFW's safety_trip_t -- the same "this codec only serializes bytes,
 * the receiver decides what they mean" split CLEAR_TRIP's trip_mask uses.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). Same conventions as kilnlink_clear_trip.c. */

#define KILNLINK_SET_CONFIG_CMD 0x16u
#define KILNLINK_SET_CONFIG_LEN 2u /* cmd(1) + tc_type u8(1) */

typedef enum {
    KILNLINK_SET_CONFIG_OK = 0,
    KILNLINK_SET_CONFIG_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_SET_CONFIG_LEN */
    KILNLINK_SET_CONFIG_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_SET_CONFIG_LEN (fixed-size frame) */
    KILNLINK_SET_CONFIG_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_SET_CONFIG_CMD */
} kilnlink_set_config_status_t;

typedef struct {
    uint8_t tc_type; /* opaque MAX31856_TC_TYPE_* value; receiver validates range */
} kilnlink_set_config_t;

/* Serializes `msg` (SAFETY_CMD_SET_CONFIG payload, byte 0 = 0x16 included)
 * into `out`. Always exactly KILNLINK_SET_CONFIG_LEN (2) bytes -- this frame
 * has no variable-length fields. Returns 2, or 0 on
 * KILNLINK_SET_CONFIG_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_set_config_encode(const kilnlink_set_config_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_set_config_status_t *status);

/* Parses a SET_CONFIG payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_SET_CONFIG_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_set_config_status_t kilnlink_set_config_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_set_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_SET_CONFIG_H */
