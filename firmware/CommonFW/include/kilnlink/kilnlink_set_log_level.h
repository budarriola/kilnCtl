#ifndef KILNLINK_SET_LOG_LEVEL_H
#define KILNLINK_SET_LOG_LEVEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_SET_LOG_LEVEL = 0x1B -- docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2's table ("minted in the same pass because it
 * was the last unallocated id blocking log_task_set_level() from being
 * reachable over the wire"). Unrelated to commissioning itself -- runtime
 * log verbosity, not a staged config field -- but it shares this pass's id
 * block because it was the last one blocking a real function.
 *
 * `level` is carried as an opaque uint8_t here, the same "this codec only
 * serializes the byte, the receiver decides what it means" split
 * kilnlink_set_config.c documents for tc_type: SaftyFW's LOG_LEVEL_* enum
 * (firmware/SaftyFW/src/tasks/log_task.h) lives in the application layer,
 * not here, and this module must stay as dependency-free of it as
 * kilnlink_clear_trip.c is of safety_trip_t.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions and
 * fixed-size-frame shape as kilnlink_set_config.c. */

#define KILNLINK_SET_LOG_LEVEL_CMD 0x1Bu
#define KILNLINK_SET_LOG_LEVEL_LEN 2u /* cmd(1) + level u8(1) */

typedef enum {
    KILNLINK_SET_LOG_LEVEL_OK = 0,
    KILNLINK_SET_LOG_LEVEL_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_SET_LOG_LEVEL_LEN */
    KILNLINK_SET_LOG_LEVEL_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_SET_LOG_LEVEL_LEN (fixed-size frame) */
    KILNLINK_SET_LOG_LEVEL_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_SET_LOG_LEVEL_CMD */
} kilnlink_set_log_level_status_t;

typedef struct {
    uint8_t level; /* opaque LOG_LEVEL_* value; receiver validates range */
} kilnlink_set_log_level_t;

/* Serializes `msg` (SAFETY_CMD_SET_LOG_LEVEL payload, byte 0 = 0x1B included)
 * into `out`. Always exactly KILNLINK_SET_LOG_LEVEL_LEN (2) bytes -- this
 * frame has no variable-length fields. Returns 2, or 0 on
 * KILNLINK_SET_LOG_LEVEL_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_set_log_level_encode(const kilnlink_set_log_level_t *msg, uint8_t *out,
                                      size_t out_cap, kilnlink_set_log_level_status_t *status);

/* Parses a SET_LOG_LEVEL payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_SET_LOG_LEVEL_LEN -- untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_set_log_level_status_t kilnlink_set_log_level_decode(const uint8_t *payload, size_t len,
                                                                kilnlink_set_log_level_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_SET_LOG_LEVEL_H */
