#ifndef KILNLINK_PARAM_VALUE_H
#define KILNLINK_PARAM_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared type-tag + value codec for the commissioning family
 * (SET_PARAM/GET_PARAM/PARAM/CONFIG_PAGE, docs/COMMISSIONING.md sec 2 and
 * LINK_PROTOCOL.md's SAFETY_CMD_SET_PARAM 0x1C onward). CONFIG_REFERENCE.md
 * secs 1-5's tunables are u8 enums, u16 seconds/ms, f32 temperatures/amps/
 * volts, and bools -- one small explicit tag set, deliberately NOT a generic
 * "however many bytes the sender says" scheme, because that is exactly how a
 * float's bit pattern ends up silently reinterpreted as a u16 threshold.
 * A tag the receiver does not recognise is a decode ERROR here, never a
 * guess at what the bytes might mean.
 *
 * Freestanding C11, no allocation, no I/O, no globals -- CommonFW/README.md
 * rules 1-6. Every codec built on top of this (kilnlink_set_param.c,
 * kilnlink_get_param.c, kilnlink_param.c, kilnlink_config_page.c) calls
 * kilnlink_param_value_len() to learn how many bytes a tag carries BEFORE
 * touching the wire buffer at that offset, so a bad tag is caught before any
 * out-of-bounds read is possible. */

#define KILNLINK_PARAM_TYPE_BOOL 0x00u /* 1 byte, 0/1 */
#define KILNLINK_PARAM_TYPE_U8   0x01u /* 1 byte, small enum/int */
#define KILNLINK_PARAM_TYPE_U16  0x02u /* 2 bytes LE, seconds/ms-scale int */
#define KILNLINK_PARAM_TYPE_F32  0x03u /* 4 bytes LE, temperature/amps/volts */

typedef union {
    uint8_t  bool_val; /* KILNLINK_PARAM_TYPE_BOOL: 0 or 1 */
    uint8_t  u8_val;   /* KILNLINK_PARAM_TYPE_U8 */
    uint16_t u16_val;  /* KILNLINK_PARAM_TYPE_U16 */
    float    f32_val;  /* KILNLINK_PARAM_TYPE_F32 */
} kilnlink_param_value_t;

/* Wire length of a KILNLINK_PARAM_TYPE_* tag's value: 1, 1, 2, or 4. Returns
 * 0 for any tag not in the set above -- 0 is not a valid length for a real
 * type, so "vlen == 0" doubles as the bad-tag test every caller needs. */
size_t kilnlink_param_value_len(uint8_t type);

/* Writes exactly kilnlink_param_value_len(type) bytes of *value at out+off.
 * Caller must already have bounds-checked out_cap >= off + that length; this
 * function trusts its caller the same way kilnlink_bytes.h's put_* helpers
 * do. Returns false (writes nothing) if `type` is not recognised. */
bool kilnlink_param_value_encode(uint8_t type, const kilnlink_param_value_t *value, uint8_t *out,
                                  size_t off);

/* Reads exactly kilnlink_param_value_len(type) bytes from in+off into
 * *value. Same bounds-checking contract as the encode side: caller must
 * already know `len_avail` (bytes remaining from `off` to the end of the
 * buffer) is >= that length before calling. Returns false (writes nothing
 * to *value) if `type` is not recognised. */
bool kilnlink_param_value_decode(uint8_t type, const uint8_t *in, size_t off,
                                   kilnlink_param_value_t *value);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_PARAM_VALUE_H */
