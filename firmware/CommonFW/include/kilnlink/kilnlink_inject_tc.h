#ifndef KILNLINK_INJECT_TC_H
#define KILNLINK_INJECT_TC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_INJECT_TC = 0x21 -- docs/LINK_PROTOCOL.md sec 4.
 * Next unallocated id after SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20), the
 * highest id minted so far -- same "append after the last used id" rule
 * every other command in this family follows.
 *
 * Feeds a synthetic thermocouple reading into thermo_task.c's published
 * snapshot on the Pico, so the whole S1/S5/S11/S12 guard chain can be
 * exercised on real hardware before the physical safety MAX31856 exists
 * (SaftyFW/tasks/thermo_task.h's own doc comment on thermo_task_inject_
 * reading() has the full reasoning and the structural gate this codec's
 * receiver -- link_task.c's handler -- forwards into). This codec only
 * serializes the payload bytes; ALL of the gating (accepted only while
 * safety_tc_installed == 0, never persisted) happens on the receiving side,
 * in thermo_task_inject_reading() itself, not here -- same split
 * kilnlink_clear_trip.h documents for its own refusal logic.
 *
 * `valid` (0/1) mirrors thermo_snapshot_t's own valid flag: when 0, `tc_c`/
 * `cj_c` on the wire are ignored by the receiver (thermo_task_inject_
 * reading() substitutes NaN, per that struct's own "NaN when !valid, never
 * 0, never stale" contract) -- but they are still carried on the wire as
 * real IEEE754 floats (typically 0.0) rather than omitted, keeping this
 * frame fixed-size like every other command in this family.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). */

#define KILNLINK_INJECT_TC_CMD 0x21u
#define KILNLINK_INJECT_TC_LEN 11u /* cmd(1) + valid u8(1) + tc_c f32 LE(4) + cj_c f32 LE(4) + fault_bits u8(1) */

typedef enum {
    KILNLINK_INJECT_TC_OK = 0,
    KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_INJECT_TC_LEN */
    KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_INJECT_TC_LEN (fixed-size frame) */
    KILNLINK_INJECT_TC_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_INJECT_TC_CMD */
} kilnlink_inject_tc_status_t;

typedef struct {
    uint8_t  valid;      /* 0/1 -- "inject a bad read" vs "inject a good reading" */
    float    tc_c;       /* ignored by the receiver when valid == 0 */
    float    cj_c;       /* ignored by the receiver when valid == 0 */
    uint8_t  fault_bits; /* SAFETY_THERMO_FAULT_* bits (safety_guards.h), opaque to this codec */
} kilnlink_inject_tc_t;

/* Serializes `msg` (SAFETY_CMD_INJECT_TC payload, byte 0 = 0x21 included)
 * into `out`. Always exactly KILNLINK_INJECT_TC_LEN (11) bytes -- this frame
 * has no variable-length fields. Returns 11, or 0 on
 * KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_inject_tc_encode(const kilnlink_inject_tc_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_inject_tc_status_t *status);

/* Parses an INJECT_TC payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_INJECT_TC_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_inject_tc_status_t kilnlink_inject_tc_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_inject_tc_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_INJECT_TC_H */
