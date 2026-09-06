#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hal_uart.h"
#include "uart_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reliable, addressed message protocol layered on top of uart_owner:
 *   - SLIP-style byte-stuffed framing (0x7E delimiter) so the receiver can
 *     always resync after noise/garbage, independent of message content.
 *   - CRC16/CCITT-FALSE over header+payload catches corrupted frames.
 *   - Every DATA frame carries a sender-assigned message index; the sender
 *     blocks for an ACK/NACK and retransmits the *same* index up to
 *     UART_PROTO_MAX_RETRIES times on timeout, so the receiver can dedup
 *     retransmits instead of double-delivering.
 *   - Frames are addressed to a (device, task_id) pair. A task on either
 *     side registers a task_id to get an inbox queue; a DATA frame for an
 *     unregistered task_id is answered with NACK ("undeliverable") instead
 *     of being silently dropped.
 *
 * uart_owner still owns the physical port and serializes writes from all
 * callers; this layer sends frames through uart_owner_transfer (TX-only)
 * and runs its own dedicated RX task to assemble the byte stream into
 * frames. Once a uart_protocol_t is attached to a uart_owner_t, that owner's
 * RX path (the rx_buffer args of uart_owner_transfer) should no longer be
 * used directly by other callers on the same port -- the protocol's RX task
 * is the sole consumer of incoming bytes. */

/* Capped at 253, not a round number: the wire header's LENGTH field
 * (uart_protocol.c) is one byte, so 255 is the hard ceiling; 253 keeps
 * DISPLAY_BLIT_CHUNK_PIXELS (pc_tools devices.py) an exact pixel count
 * ((253-1)/2 = 126, twice the old 63) instead of wasting a byte to a
 * fractional last pixel. Bumped from 128 -- see UART_PROTOCOL_VERSION's
 * bump note in uart_task_ids.h for why this needed a protocol version
 * change even though the frame layout itself didn't move. */
#define UART_PROTO_MAX_PAYLOAD   253
#define UART_PROTO_MAX_RETRIES   10

/* Upper bound on how long uart_protocol_rx_task() may sit in one read while
 * the wire is IDLE. It is not a delivery latency: the idle wait asks for a
 * single byte, so it ends on the first byte of the next frame, and a read
 * issued while bytes are already buffered uses a zero timeout. It only bounds
 * how long the task can go without noticing shutdown_requested, and bounds
 * the worst case if uart_get_buffered_data_len() ever under-reports. See that
 * function's own comment for the live incident that fixed the read shape. */
#define UART_PROTOCOL_RX_IDLE_POLL_MS 100u

/* Capacity of uart_protocol_rx_task()'s read buffer. RAISED 528
 * (STUFFED_FRAME_MAX, the worst-case stuffed frame) -> 2048 on 2026-08-28 at
 * the owner's request, as headroom rather than as a fix for anything.
 *
 * This is safe to grow ONLY because the read no longer waits on it filling
 * (see uart_protocol_rx_task()'s comment and LINK_PROTOCOL.md's "Never wait
 * on a receive buffer filling"): the task asks uart_get_buffered_data_len()
 * first and reads min(buffered, capacity) with a ZERO timeout, so latency is
 * governed by arrival and capacity only sets how many bytes one wakeup may
 * carry. At 528 that was already a whole frame per read; at 2048 it is up to
 * ~4 back-to-back frames, which is what a burst after a scheduling delay
 * actually looks like. If this loop is ever reworked so the buffer size and
 * the wait are coupled again, this constant becomes a latency bug -- that
 * coupling, not the number, is the thing to police.
 *
 * MUST stay >= STUFFED_FRAME_MAX (uart_protocol.c static-asserts it), so one
 * frame can never be split across reads purely for want of capacity.
 *
 * It lives on the RX task's stack, which is why
 * CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE moved with it (App/drivers/Kconfig
 * and sdkconfig both -- sdkconfig is gitignored and wins). */
#define UART_PROTOCOL_RX_CHUNK_BYTES 2048u
#define UART_PROTO_DEFAULT_ACK_TIMEOUT_MS 200
/* Raised 8 -> 16 (2026-08-13) when uart_task_ids.h grew task_ids 8-11
 * (CONTROL/PROFILES/AUTOTUNE/WIFI): this board's own ESP-side registrations
 * alone (THERMO, IO, INFO, DISPLAY, LOG, SYSTEM, SAFETY, CONTROL, PROFILES,
 * AUTOTUNE, WIFI = 11) already exceeded the old cap of 8, which would have
 * made uart_protocol_register_task() start failing (ESP_ERR_NO_MEM) for
 * whichever bridge started 9th -- silently dropping that task_id's coverage
 * rather than refusing to boot. 16 leaves headroom over the current 11
 * (the PC side registers its own independent 11 on the HOST device, which
 * is a *different* uart_protocol_t instance/task table and unaffected by
 * this board-side cap). */
#define UART_PROTO_MAX_TASKS     16
#define UART_PROTO_DEDUP_DEPTH   4

/* Who a frame is addressed to / came from. ESP and HOST are the two ends of
 * the PC link. SAFETY is the RP2040 safety processor on the far side of the
 * digital isolator: a *second*, physically separate uart_protocol_t instance runs
 * on UART1 with own_device = ESP and talks to a peer that identifies as
 * SAFETY, so the same framing, retry and dedup logic covers both links. */
typedef enum {
    UART_PROTO_DEVICE_ESP = 0,
    UART_PROTO_DEVICE_HOST = 1,
    UART_PROTO_DEVICE_SAFETY = 2,
} uart_proto_device_t;

typedef enum {
    UART_PROTO_MSG_DATA      = 0x01,
    UART_PROTO_MSG_ACK       = 0x02,
    UART_PROTO_MSG_NACK      = 0x03, /* destination task not registered ("undeliverable") */
    UART_PROTO_MSG_BROADCAST = 0x04, /* fire-and-forget: no ACK, no retry, no dedup. See
                                       * CommonFW/docs/LINK_PROTOCOL.md sec 1 -- required for the
                                       * safety link, where the far end must never be obliged to
                                       * transmit in reply. */
} uart_proto_msg_type_t;

typedef struct {
    uart_proto_device_t device;
    uint8_t task_id;
    uint16_t msg_index;
    uint8_t length;
    uint8_t payload[UART_PROTO_MAX_PAYLOAD];
} uart_proto_message_t;

typedef struct {
    bool in_use;
    uint8_t task_id;
    QueueHandle_t inbox;
    /* Small ring of recently-accepted (src_device, src_task, msg_index)
     * tuples, so a retransmitted DATA frame (receiver's own ACK was lost)
     * gets re-ACKed without being pushed to the inbox a second time. */
    struct {
        bool valid;
        uart_proto_device_t src_device;
        uint8_t src_task;
        uint16_t msg_index;
    } dedup[UART_PROTO_DEDUP_DEPTH];
    uint8_t dedup_next;
    /* 2026-08-23, the DIAG-frame-went-dark investigation: a BROADCAST frame
     * (the only kind SaftyFW's link_task.c ever sends -- GET_STATUS, DIAG,
     * POWER, TRIP_EVENT, FW_VERSION, all of it) whose xQueueSend() into this
     * slot's inbox fails because the inbox is already full is silently
     * dropped by uart_protocol_rx_task()'s dispatch (unlike the ACK'd DATA
     * path just below it in that same function, which at least logs a
     * warning) -- there is no ACK to withhold, no retry the sender will ever
     * attempt, and until this counter existed, nothing on this side recorded
     * it happened at all. A dropped BROADCAST is invisible in
     * stats.frame_errors too: that counter only increments once a frame
     * reaches safety_apply_status()/_diag()/_power() and fails ITS OWN
     * length/opcode check -- a frame dropped here never gets that far, so
     * "crc/framing errors 0" can read perfectly clean while broadcasts are
     * still being lost. See uart_protocol_get_task_broadcast_dropped(). */
    uint32_t broadcast_dropped;
} uart_proto_task_slot_t;

typedef struct {
    uart_owner_t *owner;
    uart_proto_device_t own_device;

    /* Points at owner->hal -- uart_owner_t now embeds a real,
     * driver-installed hal_uart_t (2026-09-06 uart collapse) instead of this
     * layer attaching a second, non-owning handle to the same port
     * (hal_uart_attach(), deleted -- see docs/HW_ABSTRACTION_PLAN.md's
     * "delete hal_uart_attach()" item). frame_and_send() sends through this
     * instead of uart_owner_transfer() (Phase 1b). One handle per port now:
     * never hal_uart_deinit'd here -- owner still owns the driver's
     * install/deinit lifecycle via uart_owner_deinit(). */
    hal_uart_t *hal_uart;

    uart_proto_task_slot_t tasks[UART_PROTO_MAX_TASKS];
    SemaphoreHandle_t tasks_lock;

    TaskHandle_t rx_task_handle;
    bool shutdown_requested;

    SemaphoreHandle_t tx_lock;   /* serializes send-and-wait-for-ack cycles */
    uint16_t next_tx_index;
    uart_proto_device_t awaited_device;
    uint8_t awaited_task;
    uint16_t awaited_index;
    SemaphoreHandle_t ack_sem;
    volatile uart_proto_msg_type_t ack_result;

    /* Rate limit for the no-reply retry warning. A link whose peer does not
     * exist at all -- which is the normal state of the safety link until the
     * RP2040 firmware is written -- otherwise emits one warning per retry,
     * ten per message, continuously. That floods uart_log_bridge's queue and
     * drops every other log line the board produces, including the whole
     * boot sequence. The retries themselves are unchanged; only how often
     * they are reported is. */
    int64_t  last_retry_log_us;
    uint32_t suppressed_retry_logs;

    /* Per-(dst_device,dst_task) "has this peer EVER answered" memory, so the
     * retry warning above can tell "peer has never existed" (safety_link.c's
     * RP2040-not-built-yet case: already logged sensibly, once, at a slow
     * rate, by that module -- this warning adds nothing) apart from "peer WAS
     * answering and just stopped" (a real fault, must stay loud). Set true on
     * any reply at all -- ACK or NACK, not just ESP_OK -- since a NACK still
     * proves a peer is alive and answering frames. Small fixed table, linear
     * scan: the number of distinct (device,task) destinations any one link
     * ever sends to is a handful (task_ids in uart_task_ids.h), not a scaling
     * concern. */
    struct {
        bool valid;
        uart_proto_device_t device;
        uint8_t task;
        bool ever_replied;
    } peer_seen[8];

    /* Deframer/dispatch-level counters, 2026-08-23 (the DIAG-frame-went-dark
     * investigation, continued): everything above (broadcast_dropped,
     * frame_errors, timeouts) brackets the gap between "the Pico's TX ring
     * accepted the frame" and "safety_apply_diag()/_power() ran" without
     * covering it -- broadcast_dropped only fires on an inbox already at
     * capacity (confirmed innocent: it moved from 0 to a real, explicable 7
     * during a Pico reflash burst, proving the counter works, and sat at 0
     * everywhere else), and frame_errors only fires on a frame that already
     * reached a per-command length/opcode check post-dispatch. Nothing
     * existed below the per-task inbox, at the deframer/routing level, until
     * these five fields -- confirmed by reading the whole of handle_raw_
     * frame()/uart_protocol_rx_task() before adding anything, per the
     * standing "surface an existing number rather than add an ambiguous
     * fifth one" caution this investigation has already paid for once
     * (frames_received). None of these five already existed under another
     * name; all are genuinely new. Single-writer (uart_protocol_rx_task()
     * only), plain volatile read from any task, same pattern
     * uart_proto_task_slot_t::broadcast_dropped already uses -- see
     * uart_protocol_get_deframe_stats(). */
    volatile uint32_t frames_deframed;        /* CRC-valid frames of ANY type/dest pulled off
                                                * the wire -- the raw "what did this port
                                                * actually receive" number, upstream of every
                                                * type-based branch below */
    volatile uint32_t frames_routed_nowhere;  /* deframed, CRC-valid, but found no home:
                                                * BROADCAST/DATA for an unregistered dst_task,
                                                * or an ACK/NACK matching no outstanding
                                                * transaction -- NOT a dst_device mismatch,
                                                * which is normal traffic filtering, not a loss */
    volatile uint32_t frame_length_mismatch;  /* handle_raw_frame()'s own length check failed
                                                * (header's declared length disagreed with the
                                                * assembled frame) */
    volatile uint32_t frame_crc_mismatch;     /* handle_raw_frame()'s own CRC16/CCITT-FALSE
                                                * check failed */
    volatile uint32_t frame_resync;           /* uart_protocol_rx_task()'s raw assembly buffer
                                                * overflowed before a delimiter closed the
                                                * frame -- oversized/corrupt, discarded,
                                                * resynced on the next 0x7E */

    /* 2026-09-05 review fix: frame_and_send()'s hal_uart_send_blocking()
     * result was only ever logged, with no counter -- a wedged TX ring
     * (HAL_TIMEOUT/HAL_IO on every send) looked identical to "peer never
     * replies" on every counter this struct already exposed. Incremented at
     * frame_and_send()'s one call site, under tx_lock (held by every caller
     * of frame_and_send -- see uart_protocol_send_limited()/
     * _send_broadcast()), so this is single-writer despite tx_lock being a
     * different lock from tasks_lock, which the frame_resync/frames_deframed
     * group above uses for the same "single-writer, volatile read" pattern.
     * See uart_protocol_get_tx_send_failures(). */
    volatile uint32_t tx_send_failures;

    bool initialized;
} uart_protocol_t;

/* owner must already be uart_owner_init'd; proto does not take ownership of
 * it (call uart_owner_deinit separately, after uart_protocol_deinit). */
esp_err_t uart_protocol_init(uart_protocol_t *proto,
                              uart_owner_t *owner,
                              uart_proto_device_t own_device,
                              UBaseType_t task_priority,
                              uint32_t stack_depth,
                              BaseType_t core_id);
esp_err_t uart_protocol_deinit(uart_protocol_t *proto);

/* Registers task_id as a valid destination on this side and gives it an
 * inbox queue of inbox_len messages. Call uart_protocol_receive on
 * *out_inbox to read messages addressed to task_id. */
esp_err_t uart_protocol_register_task(uart_protocol_t *proto,
                                       uint8_t task_id,
                                       UBaseType_t inbox_len,
                                       QueueHandle_t *out_inbox);
esp_err_t uart_protocol_unregister_task(uart_protocol_t *proto, uint8_t task_id);

/* Count of BROADCAST frames addressed to task_id whose delivery into its
 * inbox failed because the inbox was already full -- see
 * uart_proto_task_slot_t::broadcast_dropped's own doc comment for why this
 * is the ONE place a lost SaftyFW telemetry frame is actually recorded.
 * Monotonic since uart_protocol_register_task(task_id), never reset.
 * Returns ESP_ERR_INVALID_ARG if task_id was never registered; *out is left
 * untouched in that case. Safe to call from any task. */
esp_err_t uart_protocol_get_task_broadcast_dropped(uart_protocol_t *proto, uint8_t task_id,
                                                    uint32_t *out);

/* Count of frame_and_send() calls whose hal_uart_send_blocking() returned
 * anything other than HAL_OK (HAL_TIMEOUT: ring never drained within
 * timeout_ms; HAL_IO: uart_write_bytes()/uart_wait_tx_done() itself failed).
 * This is the wire-level TX failure counter -- distinct from a NACK/no-reply
 * timeout at the protocol level, which means the frame WAS sent but nothing
 * answered it. A caller wanting the raw hal_uart-level drop count instead
 * (bytes dropped by the ESP backend's own free-space/partial-write guards)
 * can call hal_uart_get_tx_dropped() directly on the same handle this
 * function reads from internally. Monotonic since uart_protocol_init(),
 * never reset. Safe to call from any task. */
uint32_t uart_protocol_get_tx_send_failures(uart_protocol_t *proto);

/* Reads all five deframer/dispatch-level counters (uart_protocol_t's own
 * doc comment on them has the full "why these five" reasoning) in one call.
 * Any output pointer may be NULL. Monotonic since uart_protocol_init(),
 * never reset. Safe to call from any task. */
esp_err_t uart_protocol_get_deframe_stats(uart_protocol_t *proto, uint32_t *out_frames_deframed,
                                          uint32_t *out_frames_routed_nowhere,
                                          uint32_t *out_frame_length_mismatch,
                                          uint32_t *out_frame_crc_mismatch,
                                          uint32_t *out_frame_resync);

esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks);

/* Sends payload to (dst_device, dst_task) as src_task on this side. Blocks
 * until ACKed, NACKed, or UART_PROTO_MAX_RETRIES retransmissions have all
 * timed out. Returns:
 *   ESP_OK              - ACKed (delivered to the destination task's inbox)
 *   ESP_ERR_NOT_FOUND    - NACKed (destination task not registered there)
 *   ESP_ERR_TIMEOUT       - no reply after all retries (link/peer down)
 *   ESP_ERR_INVALID_ARG  - length > UART_PROTO_MAX_PAYLOAD, etc. */
esp_err_t uart_protocol_send(uart_protocol_t *proto,
                              uart_proto_device_t dst_device,
                              uint8_t dst_task,
                              uint8_t src_task,
                              const uint8_t *payload,
                              size_t length,
                              uint32_t ack_timeout_ms);

/* Same DATA-frame contract as uart_protocol_send(), but with the retry
 * ceiling given explicitly instead of the fixed UART_PROTO_MAX_RETRIES (10).
 * uart_protocol_send() is implemented as this with max_retries ==
 * UART_PROTO_MAX_RETRIES, so both share one retry loop.
 *
 * Added for uart_log_bridge.c (see 2026-08-20 congestion note in that file):
 * that caller already treats the result as fire-and-forget ("the result is
 * intentionally ignored"), but was calling the 10-retry uart_protocol_send()
 * anyway, so a single stuck ACK could hold this proto's tx_lock -- shared
 * with every real reply this link ever sends -- for up to
 * ack_timeout_ms * UART_PROTO_MAX_RETRIES. A true BROADCAST send would be
 * free of that cost, but the PC-side reader (serial_link.py's
 * _handle_frame()) only delivers MsgType.DATA to a registered task's inbox
 * today -- BROADCAST is silently dropped there -- so the log channel cannot
 * switch frame types without going dark. Capping max_retries instead keeps
 * the DATA frame (and therefore PC-side delivery) while bounding the worst
 * case a single log line can dominate the shared link to
 * ack_timeout_ms * max_retries. max_retries must be >= 1. Returns the same
 * codes as uart_protocol_send(). */
esp_err_t uart_protocol_send_limited(uart_protocol_t *proto,
                                      uart_proto_device_t dst_device,
                                      uint8_t dst_task,
                                      uint8_t src_task,
                                      const uint8_t *payload,
                                      size_t length,
                                      uint32_t ack_timeout_ms,
                                      int max_retries);

/* Sends payload to (dst_device, dst_task) as a BROADCAST frame: one shot, no
 * ACK wait, no retry, no dedup on the receiving side. Returns as soon as the
 * bytes are handed to the UART. A peer that never replies -- the safety
 * processor's normal, required behaviour -- costs nothing here, unlike
 * uart_protocol_send's ten-retry timeout. Returns:
 *   ESP_OK               - handed to the UART
 *   ESP_ERR_INVALID_ARG  - length > UART_PROTO_MAX_PAYLOAD, etc.
 *   ESP_FAIL             - the UART write itself failed */
esp_err_t uart_protocol_send_broadcast(uart_protocol_t *proto,
                                        uart_proto_device_t dst_device,
                                        uint8_t dst_task,
                                        uint8_t src_task,
                                        const uint8_t *payload,
                                        size_t length);

#ifdef __cplusplus
}
#endif

#endif // UART_PROTOCOL_H
