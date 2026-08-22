#ifndef KILNLINK_COMMIT_CONFIG_REJECTED_H
#define KILNLINK_COMMIT_CONFIG_REJECTED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_COMMIT_CONFIG_REJECTED = 0x20 -- docs/LINK_PROTOCOL.md
 * sec 4, docs/COMMISSIONING.md sec 2/3.1. ROADMAP.md loose end: "no wire
 * codec carries a per-field COMMIT_CONFIG rejection reason back to the ESP
 * -- it only learns that the commit failed." This is that reply.
 *
 * 0x20 was verified free before being spent here (COMMISSIONING.md sec 2.1:
 * "ids are permanent... allocating one wrongly is expensive") -- 0x1B-0x1F
 * are COMMISSIONING.md sec 2's table (SET_LOG_LEVEL/SET_PARAM/COMMIT_CONFIG/
 * GET_PARAM+PARAM/GET_CONFIG_PAGE+CONFIG_PAGE), so 0x20 is the next
 * unallocated SAFETY_CMD_* id; grepped across CommonFW/SaftyFW/KilnFW for
 * any existing `_CMD 0x20` (or equivalent) in this command-id namespace and
 * found none -- the few unrelated `0x20` constants elsewhere (e.g.
 * MAX31856 register bits, LVGL/ILI9488 display command bytes,
 * SAFETY_THERMO_FAULT_CJHIGH) live in entirely different address spaces,
 * not SAFETY_CMD_*.
 *
 * Sent by SaftyFW's link_task.c ONLY when SAFETY_CMD_COMMIT_CONFIG (0x1D)
 * is refused -- never on acceptance (an accepted commit is already visible
 * via FW_VERSION's bumped config_crc, COMMISSIONING.md sec 3). Carries:
 *   - `param_id`: the offending field's wire id (config_params.h / this
 *     doc's sec 2.1 table), or KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID
 *     (0xFFFF, never a real id -- every real id has a nonzero high byte
 *     0x01-0x05) when the refusal is not about one field (ARMED, or a flash
 *     write failure).
 *   - `reason`: KILNLINK_COMMIT_CONFIG_REJECT_* below, coarse enough to be
 *     a fixed one-byte wire field, fine enough for the ESP to build an
 *     actionable sentence without needing the Pico's free-text rule string
 *     (config_params_validate()'s out_rule is a human-readable C string
 *     literal, useful in SaftyFW's own log, but never sent over the wire --
 *     LINK_PROTOCOL.md's fixed/bounded-frame discipline applies here same
 *     as everywhere else in this directory).
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_COMMIT_CONFIG_REJECTED_CMD 0x20u
#define KILNLINK_COMMIT_CONFIG_REJECTED_LEN 4u /* cmd(1) + param_id u16 LE(2) + reason u8(1) */

#define KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID 0xFFFFu

typedef enum {
    KILNLINK_COMMIT_CONFIG_REJECT_RANGE = 0,        /* one field's value fails its own range/finite check */
    KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION = 1, /* two staged fields contradict each other */
    KILNLINK_COMMIT_CONFIG_REJECT_ARMED = 2,         /* config writes are refused while ARMED */
    KILNLINK_COMMIT_CONFIG_REJECT_STORAGE = 3,       /* validation passed but the flash write itself failed */
    KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN = 4,       /* fallback; should not occur in practice */
} kilnlink_commit_config_reject_reason_t;

typedef enum {
    KILNLINK_COMMIT_CONFIG_REJECTED_OK = 0,
    KILNLINK_COMMIT_CONFIG_REJECTED_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_COMMIT_CONFIG_REJECTED_LEN */
    KILNLINK_COMMIT_CONFIG_REJECTED_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_COMMIT_CONFIG_REJECTED_LEN */
    KILNLINK_COMMIT_CONFIG_REJECTED_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_COMMIT_CONFIG_REJECTED_CMD */
} kilnlink_commit_config_rejected_status_t;

typedef struct {
    uint16_t param_id; /* KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID if not field-specific */
    uint8_t reason;    /* kilnlink_commit_config_reject_reason_t; opaque byte on the wire, same
                        * "codec serializes, receiver interprets" split kilnlink_set_log_level.h
                        * documents for `level` -- this codec does not itself validate that the
                        * value is one of the enum's members. */
} kilnlink_commit_config_rejected_t;

/* Serializes `msg` (SAFETY_CMD_COMMIT_CONFIG_REJECTED payload, byte 0 = 0x20
 * included) into `out`. Always exactly KILNLINK_COMMIT_CONFIG_REJECTED_LEN
 * (4) bytes -- this frame has no variable-length fields. Returns 4, or 0 on
 * KILNLINK_COMMIT_CONFIG_REJECTED_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_commit_config_rejected_encode(const kilnlink_commit_config_rejected_t *msg, uint8_t *out,
                                               size_t out_cap,
                                               kilnlink_commit_config_rejected_status_t *status);

/* Parses a COMMIT_CONFIG_REJECTED payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_COMMIT_CONFIG_REJECTED_LEN -- untrusted input from another
 * processor across an isolated link (CommonFW/README.md rule 6). */
kilnlink_commit_config_rejected_status_t kilnlink_commit_config_rejected_decode(
    const uint8_t *payload, size_t len, kilnlink_commit_config_rejected_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_COMMIT_CONFIG_REJECTED_H */
