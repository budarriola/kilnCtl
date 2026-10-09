#ifndef KILNLINK_ROLLBACK_RESULT_H
#define KILNLINK_ROLLBACK_RESULT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_ROLLBACK_RESULT = 0x25 -- docs/LINK_PROTOCOL.md
 * sec 4. The missing half of SAFETY_CMD_ROLLBACK (0x17, kilnlink_rollback.h):
 * that frame is fire-and-forget and "never ACKed on the wire" by design
 * (link_task_handle_rollback(), src/tasks/link_task.c), which used to mean
 * a refusal (relay ARMED, or bootloader_decide_rollback() saying the other
 * slot is not VALID/PENDING_VERIFY) was only ever visible in SaftyFW's own
 * local log -- the ESP, and therefore the OTA web page, had no way to know
 * a rollback it requested had been refused rather than accepted. This frame
 * closes that gap.
 *
 * 0x25 was verified free the same way 0x20 (kilnlink_commit_config_
 * rejected.h) was: 0x1B-0x24 are all spoken for (COMMISSIONING.md sec 2's
 * table plus the GET_CT_CAL/GET_PARAM/GET_CONFIG_PAGE split, kilnlink_
 * version.h's 6->7 entry), so 0x25 is the next unallocated SAFETY_CMD_* id.
 * Grepped across CommonFW/SaftyFW/KilnFW for any existing `_CMD 0x25` (or
 * equivalent) in this command-id namespace and found none.
 *
 * ASYMMETRIC BY DESIGN -- read this before "fixing" the accepted path:
 * update_task_request_rollback() does NOT return on acceptance (it calls
 * watchdog_reboot() and the RP2040 resets immediately, src/tasks/
 * update_task.c). There is no code path on the Pico that could still send a
 * wire frame after that call returns true, because it never returns true.
 * So this frame is, in practice, ONLY ever sent on REFUSAL
 * (link_task_handle_rollback() only reaches the point where it could send
 * one after update_task_request_rollback() has already returned false --
 * see that function's call site). `accepted` is still a real field (not
 * hardcoded false in the codec) so the wire format does not quietly assume
 * a sender that will never exist today is impossible forever, but every
 * decoder on the ESP side must treat "acceptance" as something it infers
 * from the ABSENCE of this frame plus a link drop-and-reconnect with a new
 * boot_id -- never as something this frame itself reports. See safety_
 * link.c's safety_link_send_rollback_ex() (KilnFW) for exactly that
 * inference: it snapshots the peer's boot_id before sending, sends the
 * request as a small retry burst, and -- only once its reply window closes
 * with no refusal -- watches for that boot_id to change before reporting
 * ACCEPTED, exactly as this paragraph describes (fire-and-forget
 * safety_link_send_rollback() above is the OLD, uninstrumented sender kept
 * only for uart_bridge.c's PC-link caller; it does not implement this
 * inference at all). See CommonFW/docs/LINK_PROTOCOL.md sec 4's entry on
 * this frame for the full skew-safety argument (a timeout must never be
 * misread as success when the link was simply down to begin with).
 *
 * SKEW SAFETY: SaftyFW only emits this frame once it has positively learned,
 * via ANNOUNCE_VERSION, that the peer ESP is running KILNLINK_PROTOCOL_
 * VERSION >= KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL (see link_task.c's
 * s_peer_protocol_version and link_frame_rollback_result_supported() --
 * exact mirror of LINK_FRAME_STATUS_V2_MIN_PROTOCOL's own gate). An OLD ESP
 * therefore never receives a frame it doesn't understand: it keeps seeing
 * exactly the pre-existing "logged locally on the Pico, no wire visibility"
 * behavior, unchanged. A NEW ESP talking to an OLD Pico (one that predates
 * this frame entirely) never receives a reply either -- old firmware simply
 * never sends 0x25 -- so the NEW ESP's rollback caller must ALWAYS bound its
 * wait for this frame with a timeout and treat that timeout as "unknown
 * outcome," never as "refused" (no frame arrived) and never as "accepted"
 * on its own (only a subsequent link drop + reconnect + new boot_id proves
 * that). This is the same reasoning link_frame_status_v2_supported()'s own
 * doc comment gives for the V2 status frame, applied to a Pico->ESP
 * unsolicited reply instead of a periodic broadcast.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_commit_config_rejected.c, the closest existing precedent (a
 * Pico->ESP refusal reply with a closed reason enum). */

#define KILNLINK_ROLLBACK_RESULT_CMD 0x25u
#define KILNLINK_ROLLBACK_RESULT_LEN 3u /* cmd(1) + accepted u8(1) + reason u8(1) */

/* The peer protocol_version (KILNLINK_PROTOCOL_VERSION, kilnlink_version.h)
 * at or above which a Pico may emit this frame -- this bump (8 -> 9, see
 * that header's own history comment) is the version this gate checks
 * against, mirroring link_frame.h's LINK_FRAME_STATUS_V2_MIN_PROTOCOL
 * pattern exactly. Defined here (not link_frame.h) because, unlike the V2
 * status field, this whole frame is new rather than an existing frame
 * growing a field -- there is no shared struct for both sides to gate the
 * same way, so the gate lives next to the frame it gates. */
#define KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL 9u

typedef enum {
    /* Rollback refused: relay is ARMED (same gate SET_CONFIG/SET_CT_CAL
     * use) -- update_task_request_rollback()'s first check. */
    KILNLINK_ROLLBACK_RESULT_REASON_ARMED = 0,
    /* Rollback refused: no bootloader metadata record to roll back from at
     * all. */
    KILNLINK_ROLLBACK_RESULT_REASON_NO_METADATA = 1,
    /* Rollback refused: bootloader_decide_rollback() says the other slot is
     * not currently VALID or PENDING_VERIFY -- the one property that keeps
     * a rollback from ever stranding the board with zero bootable slots.
     * This is the refusal reason this whole feature exists to surface. */
    KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID = 2,
    /* Rollback refused: validation passed (ARMED clear, other slot valid)
     * but persisting the updated metadata record to flash failed. */
    KILNLINK_ROLLBACK_RESULT_REASON_STORAGE = 3,
    /* Fallback; should not occur in practice -- same role as COMMIT_CONFIG_
     * REJECTED's own _UNKNOWN member. Also what a decoder should assume for
     * any wire byte outside this enum's known range (CommonFW/README.md
     * rule 6: untrusted input, do not let an unrecognized value alias a
     * real reason). */
    KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN = 4,
} kilnlink_rollback_result_reason_t;

typedef enum {
    KILNLINK_ROLLBACK_RESULT_OK = 0,
    KILNLINK_ROLLBACK_RESULT_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_ROLLBACK_RESULT_LEN */
    KILNLINK_ROLLBACK_RESULT_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_ROLLBACK_RESULT_LEN (fixed-size frame) */
    KILNLINK_ROLLBACK_RESULT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_ROLLBACK_RESULT_CMD */
} kilnlink_rollback_result_status_t;

typedef struct {
    /* 0/1. Present for wire-format completeness (see this header's own
     * "ASYMMETRIC BY DESIGN" comment above) -- no sender in this codebase
     * ever sets this to 1 today; every real sender is a refusal. A decoder
     * must not treat a 1 it happens to see as impossible to parse, only as
     * something the current firmware never emits. */
    uint8_t accepted;
    uint8_t reason; /* kilnlink_rollback_result_reason_t; opaque byte on the wire, same
                     * "codec serializes, receiver interprets" split kilnlink_commit_
                     * config_rejected.h documents for its own `reason` field. Only
                     * meaningful when accepted == 0. */
} kilnlink_rollback_result_t;

/* Serializes `msg` (SAFETY_CMD_ROLLBACK_RESULT payload, byte 0 = 0x25
 * included) into `out`. Always exactly KILNLINK_ROLLBACK_RESULT_LEN (3)
 * bytes. Returns 3, or 0 on KILNLINK_ROLLBACK_RESULT_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_rollback_result_encode(const kilnlink_rollback_result_t *msg, uint8_t *out, size_t out_cap,
                                       kilnlink_rollback_result_status_t *status);

/* Parses a ROLLBACK_RESULT payload (as extracted from kilnlink_frame_t::
 * payload) into `out`. `len` must be exactly KILNLINK_ROLLBACK_RESULT_LEN --
 * this is untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_rollback_result_status_t kilnlink_rollback_result_decode(const uint8_t *payload, size_t len,
                                                                   kilnlink_rollback_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_ROLLBACK_RESULT_H */
