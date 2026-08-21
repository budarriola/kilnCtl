#ifndef BENCHPROTO_LINK_H
#define BENCHPROTO_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "benchproto/benchproto_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reliable, addressed request/reply layer on top of benchproto_frame:
 * sender-assigned sequence numbers (msg_index), retry-attempt counting,
 * receiver-side dedup, and an addressable-task-registration model -- the
 * same shape firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md described
 * for its DAC/AD9833/OLED/PCF8575 task IDs, generalized here to be
 * transport-agnostic. SimFW's command groups (SYS/MODEL/TC/CT/RELAY/IO/
 * FAULT/EVT, DESIGN_NOTES.md sec 5) register as addressable tasks the same way.
 *
 * Freestanding C11, no allocation, no I/O, no globals -- CommonFW/README.md
 * rules 1-4 (written for kilnlink) apply equally here. In particular there
 * is no blocking wait and no clock read anywhere in this file: retry
 * timing and the actual byte transfer are each caller's job (an RTOS task
 * loop, a poll loop, whatever the host environment provides). This library
 * only tracks the *decisions* -- whose turn a reply is, whether a DATA
 * frame has already been delivered, whether a task_id is a legal
 * destination -- the part that both firmware and host tests can share
 * without pulling in FreeRTOS, a UART driver, or a clock. See
 * firmware/CommonFW/docs/BENCHPROTO.md sec 3 for the full reliability
 * spec this implements, and sec 5 for why the actual send/receive I/O and
 * retry timer are deliberately left to each consumer (the same
 * transport-vs-contract split CommonFW/README.md draws for kilnlink). */

#define BENCHPROTO_MAX_TASKS   16u
#define BENCHPROTO_MAX_RETRIES 10u
#define BENCHPROTO_DEDUP_DEPTH 4u
/* Suggested default; not read or enforced anywhere in this file -- the
 * actual timeout wait is the caller's own clock/RTOS primitive. Carried
 * here so firmware, host tests and docs all cite the same number. */
#define BENCHPROTO_DEFAULT_ACK_TIMEOUT_MS 200u

typedef enum {
    BENCHPROTO_LINK_OK = 0,
    BENCHPROTO_LINK_ERR_ALREADY_REGISTERED,
    BENCHPROTO_LINK_ERR_NO_SLOTS,
    BENCHPROTO_LINK_ERR_NOT_FOUND,
    BENCHPROTO_LINK_ERR_INVALID_ARG,
} benchproto_link_status_t;

/* What benchproto_link_on_frame() decided a just-decoded frame means, in
 * terms of what the caller must do next. This function never touches an
 * inbox queue, a UART, or a USB endpoint -- see the file comment. */
typedef enum {
    /* Frame is not relevant to this side right now: wrong dst_device, or
     * an ACK/NACK with no pending request outstanding, or one that doesn't
     * match it. Caller does nothing. */
    BENCHPROTO_LINK_ACTION_IGNORE = 0,
    /* A new DATA or BROADCAST frame addressed to a registered task_id.
     * Caller should deliver frame->payload to that task's inbox. For a
     * DATA frame, only once delivery actually succeeded, call
     * benchproto_link_mark_delivered() (so a retransmit of the same
     * message is classified DUPLICATE_REACK next time, not DELIVER again)
     * and send an ACK built from this frame's (src_device, src_task,
     * msg_index). For a BROADCAST frame do neither: broadcasts are
     * fire-and-forget -- never deduped, never acknowledged, and a lost or
     * duplicated one is the sender's problem to notice, not this layer's
     * (BENCHPROTO.md sec 4). */
    BENCHPROTO_LINK_ACTION_DELIVER,
    /* A DATA frame whose (src_device, src_task, msg_index) was already
     * marked delivered (the sender's retry outran our first ACK). Caller
     * must NOT redeliver to the task's inbox; just resend ACK. */
    BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
    /* A DATA frame addressed to a task_id that isn't registered on this
     * side. Caller should send NACK ("undeliverable"). BROADCAST frames to
     * an unregistered task_id are instead silently IGNOREd -- see
     * BENCHPROTO.md sec 4, a broadcast receiver is never obliged to reply
     * either way. */
    BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE,
    /* An ACK matching the `pending` request passed in: the send succeeded. */
    BENCHPROTO_LINK_ACTION_ACK_MATCHED,
    /* A NACK matching the `pending` request passed in: the destination
     * task wasn't registered on the far side. */
    BENCHPROTO_LINK_ACTION_NACK_MATCHED,
} benchproto_link_action_t;

typedef struct {
    bool in_use;
    uint8_t task_id;
    /* Small ring of recently-delivered (src_device, src_task, msg_index)
     * tuples, so a retransmitted DATA frame (the receiver's own ACK was
     * lost) is classified DUPLICATE_REACK instead of DELIVER a second
     * time. */
    struct {
        bool valid;
        uint8_t src_device;
        uint8_t src_task;
        uint16_t msg_index;
    } dedup[BENCHPROTO_DEDUP_DEPTH];
    uint8_t dedup_next;
} benchproto_task_slot_t;

typedef struct {
    uint8_t own_device;
    uint16_t next_tx_index;
    benchproto_task_slot_t tasks[BENCHPROTO_MAX_TASKS];
} benchproto_link_t;

/* Outstanding send-and-await-reply state for ONE in-flight request. The
 * pattern this is lifted from (UnitTestFw's uart_protocol_t) allows only
 * one outstanding request per protocol instance at a time, serialized by a
 * mutex in that FreeRTOS-specific caller; the same discipline applies
 * here: a caller wanting concurrent outstanding requests runs multiple
 * benchproto_pending_request_t instances, one per allowed concurrency
 * slot, exactly as it would run multiple benchproto_link_t instances for
 * multiple physical links. */
typedef struct {
    bool active;
    uint8_t dst_device;
    uint8_t dst_task;
    uint16_t msg_index;
    uint8_t attempt; /* 1-based: the sender's Nth attempt at this msg_index */
} benchproto_pending_request_t;

void benchproto_link_init(benchproto_link_t *link, uint8_t own_device);

/* Registers task_id as a valid destination on this side. Task IDs only
 * need to be unique within their own device -- see BENCHPROTO.md sec 4,
 * same convention as UART_PROTOCOL.md's original task table. This library
 * does not allocate or own an inbox queue for the task; that is the
 * consumer's own RTOS/host-runtime object (see file comment). */
benchproto_link_status_t benchproto_link_register_task(benchproto_link_t *link, uint8_t task_id);
benchproto_link_status_t benchproto_link_unregister_task(benchproto_link_t *link, uint8_t task_id);
bool benchproto_link_is_registered(const benchproto_link_t *link, uint8_t task_id);

/* Next sender-assigned msg_index for a *new* logical send (not a retry of
 * an existing one -- retries reuse the same index, see
 * benchproto_pending_begin()/note_retry()). Wraps like a plain uint16_t
 * counter; the dedup ring is intentionally shallow (BENCHPROTO_DEDUP_DEPTH)
 * and does not depend on the counter never wrapping within a session --
 * same as UART_PROTOCOL.md's original design. */
uint16_t benchproto_link_next_msg_index(benchproto_link_t *link);

/* Begins tracking a new outstanding request at attempt 1. */
void benchproto_pending_begin(benchproto_pending_request_t *pending, uint8_t dst_device, uint8_t dst_task,
                               uint16_t msg_index);
/* Call when about to retransmit the same msg_index. Returns false (and
 * clears `pending`) once the next attempt would exceed
 * BENCHPROTO_MAX_RETRIES -- the caller's send has timed out and should be
 * reported as such (BENCHPROTO.md sec 3's TIMEOUT result). Returns true
 * (with `pending->attempt` incremented) when another attempt is still
 * allowed. */
bool benchproto_pending_note_retry(benchproto_pending_request_t *pending);
void benchproto_pending_clear(benchproto_pending_request_t *pending);

/* Classifies a just-decoded, already CRC/length-validated frame (i.e. the
 * output of benchproto_frame_decode() with status == BENCHPROTO_FRAME_OK)
 * and returns what the caller must do next. `pending` may be NULL if the
 * caller never sends requests on this link (a pure responder); ACK/NACK
 * frames are then always classified BENCHPROTO_LINK_ACTION_IGNORE. */
benchproto_link_action_t benchproto_link_on_frame(benchproto_link_t *link,
                                                    benchproto_pending_request_t *pending,
                                                    const benchproto_frame_t *frame);

/* Call after a BENCHPROTO_LINK_ACTION_DELIVER for a DATA frame has actually
 * been handed off to task_id's own inbox -- recording before delivery
 * truly succeeds would make a message dropped for being un-enqueueable
 * (inbox full) look like a duplicate on the sender's retry, so it would
 * get falsely re-ACKed without ever having been delivered (ported from
 * UnitTestFw's uart_protocol.c dedup_record() comment, same reasoning).
 * Never call this for a BROADCAST frame -- see BENCHPROTO_LINK_ACTION_DELIVER.
 * Returns BENCHPROTO_LINK_ERR_NOT_FOUND if task_id isn't registered. */
benchproto_link_status_t benchproto_link_mark_delivered(benchproto_link_t *link, uint8_t task_id,
                                                          uint8_t src_device, uint8_t src_task,
                                                          uint16_t msg_index);

#ifdef __cplusplus
}
#endif

#endif /* BENCHPROTO_LINK_H */
