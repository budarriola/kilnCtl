#ifndef KILNLINK_REBOOT_RESULT_H
#define KILNLINK_REBOOT_RESULT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_REBOOT_RESULT = 0x2A -- docs/LINK_PROTOCOL.md sec
 * 4. The reply to SAFETY_CMD_REBOOT (0x29, kilnlink_reboot.h), sent on BOTH
 * outcomes.
 *
 * SYMMETRIC, unlike kilnlink_rollback_result.h -- read this before assuming
 * the two behave the same. ROLLBACK's result frame is refusal-only because
 * update_task_request_rollback() never returns on acceptance (it persists
 * the metadata record and calls hal_wdt_reboot() from inside itself, so
 * there is no instant at which a wire frame could still be queued). A
 * reboot-in-place has no such constraint: link_task_handle_reboot() decides,
 * SENDS this frame with accepted=1, gives the UART a bounded moment to
 * drain, and only then calls hal_wdt_reboot(). So the ESP gets positive,
 * wire-visible confirmation that the Pico accepted -- it never has to infer
 * acceptance from silence plus a boot_id change the way the rollback caller
 * does.
 *
 * That confirmation is "the Pico accepted and is about to reset", NOT "the
 * Pico has finished rebooting" -- the frame necessarily leaves before the
 * reset happens. KilnFW's sw_reset_http.c is explicit about that distinction
 * in the text it reports to the operator: it says the reboot was COMMANDED
 * and ACCEPTED, never that it was observed to complete.
 *
 * SKEW SAFETY -- and why KILNLINK_PROTOCOL_VERSION is NOT bumped for this
 * pair (see kilnlink_version.h's own note): this frame is REQUEST-TRIGGERED.
 * A Pico only ever emits 0x2A in direct response to a 0x29 it just decoded,
 * and only a build new enough to have this feature ever sends 0x29. An ESP
 * that predates this pair therefore never receives 0x2A at all -- there is
 * no path by which it could -- so unlike SAFETY_CMD_ROLLBACK_RESULT (an
 * unsolicited Pico-initiated reply, hence its own
 * KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL gate) no peer-version gate is
 * needed or possible here. The reverse skew (a NEW ESP, an OLD Pico that has
 * no 0x29 case at all) is handled the way this codebase always handles it:
 * the request is silently unmatched by the old dispatch switch, no reply
 * comes back, and safety_link_send_reboot() reports NO_REPLY -- never
 * ACCEPTED. Silence is never evidence of success.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_rollback_result.c. */

#define KILNLINK_REBOOT_RESULT_CMD 0x2Au
#define KILNLINK_REBOOT_RESULT_LEN 3u /* cmd(1) + accepted u8(1) + reason u8(1) */

typedef enum {
    /* Reboot accepted; `reason` is meaningless. Only ever paired with
     * accepted == 1. */
    KILNLINK_REBOOT_RESULT_REASON_NONE = 0,
    /* Refused: the relay is energized (ARMED) -- the same gate config writes
     * and SAFETY_CMD_ROLLBACK already use. Rebooting the safety processor
     * while it is holding heating permission would drop that supervision
     * mid-firing. */
    KILNLINK_REBOOT_RESULT_REASON_ARMED = 1,
    /* Fallback; should not occur in practice. Also what a decoder must
     * assume for any wire byte outside this enum's known range
     * (CommonFW/README.md rule 6: do not let an unrecognized value alias a
     * real reason). */
    KILNLINK_REBOOT_RESULT_REASON_UNKNOWN = 2,
} kilnlink_reboot_result_reason_t;

typedef enum {
    KILNLINK_REBOOT_RESULT_OK = 0,
    KILNLINK_REBOOT_RESULT_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_REBOOT_RESULT_LEN */
    KILNLINK_REBOOT_RESULT_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_REBOOT_RESULT_LEN (fixed-size frame) */
    KILNLINK_REBOOT_RESULT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_REBOOT_RESULT_CMD */
} kilnlink_reboot_result_status_t;

typedef struct {
    uint8_t accepted; /* 0/1 -- unlike ROLLBACK_RESULT, 1 is a real, routinely-sent value */
    uint8_t reason;   /* kilnlink_reboot_result_reason_t; opaque byte on the wire, same
                       * "codec serializes, receiver interprets" split kilnlink_rollback_
                       * result.h documents. Only meaningful when accepted == 0. */
} kilnlink_reboot_result_t;

/* Serializes `msg` (SAFETY_CMD_REBOOT_RESULT payload, byte 0 = 0x2A
 * included) into `out`. Always exactly KILNLINK_REBOOT_RESULT_LEN (3) bytes.
 * Returns 3, or 0 on KILNLINK_REBOOT_RESULT_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_reboot_result_encode(const kilnlink_reboot_result_t *msg, uint8_t *out, size_t out_cap,
                                     kilnlink_reboot_result_status_t *status);

/* Parses a REBOOT_RESULT payload (as extracted from kilnlink_frame_t::
 * payload) into `out`. `len` must be exactly KILNLINK_REBOOT_RESULT_LEN --
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_reboot_result_status_t kilnlink_reboot_result_decode(const uint8_t *payload, size_t len,
                                                              kilnlink_reboot_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_REBOOT_RESULT_H */
