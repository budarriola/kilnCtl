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
 * optocouplers: a *second*, physically separate uart_protocol_t instance runs
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
} uart_proto_task_slot_t;

typedef struct {
    uart_owner_t *owner;
    uart_proto_device_t own_device;

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
