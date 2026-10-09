#ifndef KILNLINK_APPLY_CONFIG_VOLATILE_H
#define KILNLINK_APPLY_CONFIG_VOLATILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_APPLY_CONFIG_VOLATILE = 0x2Du -- docs/LINK_PROTOCOL.md
 * sec 4, docs/KILN_PROFILES_PLAN.md item 15/sec 1a.3. One byte, no
 * arguments -- same trivial fixed-length-wrapper shape as kilnlink_commit_
 * config.h, which this codec deliberately mirrors byte-for-byte rather than
 * overloading COMMIT_CONFIG's own wire frame with an extra flag byte (the
 * plan's other option): COMMIT_CONFIG's frame length is asserted exactly by
 * test_commit_config.c's test_decode_too_long() and is otherwise depended on
 * everywhere as a fixed 1-byte frame, so growing it would be a real
 * behavioral change to an existing, heavily-referenced codec for a feature
 * this one command can express on its own, additively, instead.
 *
 * Tells SaftyFW to validate everything staged by SET_PARAM
 * (kilnlink_set_param.h) exactly as COMMIT_CONFIG does, and on success
 * install it into the Pico's live in-RAM config record (config_store_
 * write_volatile(), config_store_flash.c) WITHOUT writing flash and WITHOUT
 * the flash-stall half of the ARMED refusal that gates the ordinary flash
 * path (config_store_write()'s config_store_decide_write() call) -- the
 * Pico never has to leave RELAY_OWNER_STATE_ARMED to accept an ordinary
 * kiln package swap, because nothing here ever reaches the flash write that
 * half of the gate protects.
 *
 * Same fire-and-forget refusal reporting as COMMIT_CONFIG: a validation
 * failure is reported on the existing SAFETY_CMD_COMMIT_CONFIG_REJECTED
 * (0x20) frame (kilnlink_commit_config_rejected.h) -- a volatile install is
 * not a second, less-checked kind of install, so it is not given a second
 * kind of rejection reply either.
 *
 * CORRECTION (2026-09-14 review, Finding A): this comment used to claim
 * KILNLINK_COMMIT_CONFIG_REJECT_ARMED was "unreachable here by
 * construction, not merely by convention" -- that was true only of the
 * flash-stall reason the ARMED gate also serves; it ignored the gate's
 * OTHER documented purpose (ARCHITECTURE.md sec 7/8, CONFIG_REFERENCE.md
 * sec 210, COMMISSIONING.md sec 2: "retuning a safety threshold during a
 * firing is not a supported operation"), which this path had silently
 * stopped enforcing. config_store_write_volatile() now DOES refuse a
 * narrow class of installs while ARMED -- raising or clearing
 * abs_max_temp_c (S1) or max_rate_c_per_min (S8), or any tc_type change --
 * and IS reported with KILNLINK_COMMIT_CONFIG_REJECT_ARMED on this same
 * frame. An ordinary kiln-package swap (PID/profile-shaped params) is
 * unaffected, since a swap is already refused during a firing by its own
 * separate interlock and never needs to touch these fields anyway.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_APPLY_CONFIG_VOLATILE_CMD 0x2Du
#define KILNLINK_APPLY_CONFIG_VOLATILE_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_APPLY_CONFIG_VOLATILE_OK = 0,
    KILNLINK_APPLY_CONFIG_VOLATILE_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_APPLY_CONFIG_VOLATILE_LEN */
    KILNLINK_APPLY_CONFIG_VOLATILE_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_APPLY_CONFIG_VOLATILE_LEN (fixed-size frame) */
    KILNLINK_APPLY_CONFIG_VOLATILE_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_APPLY_CONFIG_VOLATILE_CMD */
} kilnlink_apply_config_volatile_status_t;

/* No fields -- the payload is the command byte alone. Kept as an (empty)
 * struct anyway so this codec's encode/decode signatures match every other
 * kilnlink_<name>_t codec's shape. */
typedef struct {
    uint8_t reserved; /* unused; always 0, not part of the wire payload */
} kilnlink_apply_config_volatile_t;

/* Serializes `msg` (SAFETY_CMD_APPLY_CONFIG_VOLATILE payload, byte 0 = 0x2D)
 * into `out`. Always exactly KILNLINK_APPLY_CONFIG_VOLATILE_LEN (1) byte.
 * `msg` may be NULL, since there is nothing in it to read. Returns 1, or 0
 * on KILNLINK_APPLY_CONFIG_VOLATILE_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_apply_config_volatile_encode(const kilnlink_apply_config_volatile_t *msg, uint8_t *out,
                                              size_t out_cap, kilnlink_apply_config_volatile_status_t *status);

/* Parses an APPLY_CONFIG_VOLATILE payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_APPLY_CONFIG_VOLATILE_LEN -- untrusted input from another
 * processor across an isolated link (CommonFW/README.md rule 6). `out` may
 * be NULL, since there is nothing to fill in. */
kilnlink_apply_config_volatile_status_t kilnlink_apply_config_volatile_decode(
    const uint8_t *payload, size_t len, kilnlink_apply_config_volatile_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_APPLY_CONFIG_VOLATILE_H */
