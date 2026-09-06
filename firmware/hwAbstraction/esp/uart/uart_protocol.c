#include "uart_protocol.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/idf_additions.h"

#include "kilnlink/kilnlink_frame.h"

static const char *TAG = "uart_proto";

/* Framing/CRC constants and delegated logic now live in firmware/CommonFW's
 * kilnlink component (kilnlink_frame.c/kilnlink_crc.c) -- see
 * CommonFW/README.md rule 7 ("no CRC or byte-stuffing implementation outside
 * CommonFW") and ROADMAP.md M2's "KilnFW delegating framing and CRC, proven
 * byte-identical before the old code is deleted" item. Proven byte-identical
 * against the old local implementation via
 * firmware/CommonFW/test/test_uart_protocol_delegate.c before this file was
 * switched over. FRAME_DELIM/FRAME_ESC/FRAME_ESC_XOR/HEADER_LEN are kept as
 * local aliases so the rest of this file (and its comments) don't need to
 * spell out the KILNLINK_FRAME_* names everywhere. */
#define FRAME_DELIM   KILNLINK_FRAME_DELIM
#define FRAME_ESC     KILNLINK_FRAME_ESC
#define FRAME_ESC_XOR KILNLINK_FRAME_ESC_XOR
#define HEADER_LEN    KILNLINK_FRAME_HEADER_LEN

/* Raw (unstuffed) header layout, CRC covers offsets [0, HEADER_LEN+length). */
#define RAW_FRAME_MAX (HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2u)
/* Stuffing can at most double the bytes, plus two delimiters. */
#define STUFFED_FRAME_MAX (RAW_FRAME_MAX * 2u + 2u)

/* The RX read buffer is deliberately LARGER than one worst-case frame (see
 * UART_PROTOCOL_RX_CHUNK_BYTES in uart_protocol.h), but it may never be
 * smaller, or a single frame could need two reads purely for want of room. */
_Static_assert(UART_PROTOCOL_RX_CHUNK_BYTES >= STUFFED_FRAME_MAX,
               "RX chunk must hold at least one worst-case stuffed frame");

/* One no-reply warning per 5s per protocol instance (see uart_protocol.h). */
#define RETRY_LOG_INTERVAL_US 5000000

/* peer_seen[] lookup/update -- see uart_protocol.h's field comment. Not
 * locked: only ever touched from within uart_protocol_send_limited() while
 * proto->tx_lock is held, same single-writer rule as awaited_* / ack_result. */
static bool peer_ever_replied(uart_protocol_t *proto, uart_proto_device_t device, uint8_t task)
{
    for (size_t i = 0; i < sizeof(proto->peer_seen) / sizeof(proto->peer_seen[0]); ++i) {
        if (proto->peer_seen[i].valid && proto->peer_seen[i].device == device &&
            proto->peer_seen[i].task == task) {
            return proto->peer_seen[i].ever_replied;
        }
    }
    return false;
}

static void peer_mark_replied(uart_protocol_t *proto, uart_proto_device_t device, uint8_t task)
{
    int free_slot = -1;
    for (size_t i = 0; i < sizeof(proto->peer_seen) / sizeof(proto->peer_seen[0]); ++i) {
        if (proto->peer_seen[i].valid && proto->peer_seen[i].device == device &&
            proto->peer_seen[i].task == task) {
            proto->peer_seen[i].ever_replied = true;
            return;
        }
        if (!proto->peer_seen[i].valid && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot >= 0) {
        proto->peer_seen[free_slot].valid = true;
        proto->peer_seen[free_slot].device = device;
        proto->peer_seen[free_slot].task = task;
        proto->peer_seen[free_slot].ever_replied = true;
    }
    /* Table full: extremely unlikely (see header comment on its size), and
     * the worst consequence is just this one destination not getting the
     * "has replied before" memory -- the retry warning stays as loud as
     * before for it, which is the safe direction to fail in. */
}

/* 2026-08-27: these used to be two local static functions, crc16_ccitt_false()
 * and stuff_and_send() (frame_and_send() below is the latter, renamed), each
 * just calling straight through to
 * kilnlink_crc16_ccitt_false()/kilnlink_stuff() -- pure pass-throughs, no
 * algorithm of their own. They still tripped SaftyFW/tools/
 * check_link_impl_isolation.ps1's from-scratch-implementation grep purely on
 * their NAMES (the check looks for a definition whose name contains "crc" or
 * "stuff", which a wrapper's name does too, even though its body is one
 * line). Deleting the wrapper and calling kilnlink_crc16_ccitt_false()/
 * kilnlink_stuff() directly at each site removes that false trip without
 * changing behavior at all -- proven byte-identical in
 * App/test/test_uart_protocol_link_delegate.c before this indirection was
 * removed, same as the original migration off a from-scratch implementation
 * was proven in firmware/CommonFW/test/test_uart_protocol_delegate.c. */
static size_t frame_and_send(uart_protocol_t *proto, const uint8_t *raw, size_t raw_len, uint32_t timeout_ms)
{
    uint8_t out[STUFFED_FRAME_MAX];
    size_t o = kilnlink_stuff(raw, raw_len, out, sizeof(out));
    if (o == 0) {
        ESP_LOGW(TAG, "frame stuffing failed (raw_len=%u exceeds STUFFED_FRAME_MAX capacity)",
                 (unsigned)raw_len);
        return 0;
    }

    /* Phase 1b (docs/HW_ABSTRACTION_PLAN.md "hal_uart -- two primitives, ESP
     * backend unchanged"): was uart_owner_transfer(proto->owner, out, o, NULL,
     * 0, NULL, timeout_ms) -- TX-only (rx=NULL,0), the only real caller of
     * that function (see uart_owner.h's own doc comment). hal_uart_send_
     * blocking's ESP backend is uart_write_bytes()+uart_wait_tx_done(), the
     * same pair uart_owner_task() used internally for this request, so this
     * still blocks until the bytes are on the wire before returning -- load-
     * bearing, since the ACK timer at uart_protocol_send_limited() starts
     * right after this call returns (see hal_uart.h's own doc comment on
     * why a fire-and-forget send would start that timer early). proto->
     * hal_uart is attached (not driver-installed) to the same port
     * proto->owner already owns -- see hal_uart_attach()'s doc comment.
     *
     * Partial-send caveat (hal_uart.h): only matters for `o` larger than the
     * ESP backend's TX ring (4096 B) -- STUFFED_FRAME_MAX here is well under
     * that, so every send is one chunk and a HAL_TIMEOUT return means nothing
     * was queued. */
    hal_status_t hal_result = hal_uart_send_blocking(&proto->hal_uart, out, o, timeout_ms);
    if (hal_result != HAL_OK) {
        /* Review fix (2026-09-05): hal_result was only ever logged here --
         * a run of HAL_TIMEOUT/HAL_IO from a genuinely wedged TX ring looked
         * identical to a healthy link on every counter this module exposes
         * (frame_and_send's caller already counts RETRIES via
         * suppressed_retry_logs, but that path fires just as loud for "peer
         * never answers" as for "we can't even get bytes onto the wire" --
         * see this file's frames_deframed/etc. doc comment on not adding an
         * ambiguous counter). uart_protocol_get_tx_send_failures() now
         * surfaces this specific failure mode (this call site is the only
         * caller of hal_uart_send_blocking in this module) so a caller (e.g.
         * safety_link.c's link-stats mirror) can tell "the wire itself is
         * failing sends" apart from "no reply", which this counter alone
         * cannot distinguish either but at least makes visible. Single-writer
         * (tx_lock is held by every caller of frame_and_send -- see
         * uart_protocol_send_limited()/_send_broadcast()), plain volatile
         * read from any task, same pattern as the deframe-stats counters. */
        proto->tx_send_failures++;
        ESP_LOGW(TAG, "frame tx failed: %s", hal_status_to_name(hal_result));
        return 0;
    }
    return o;
}

static uart_proto_task_slot_t *find_slot(uart_protocol_t *proto, uint8_t task_id)
{
    for (int i = 0; i < UART_PROTO_MAX_TASKS; ++i) {
        if (proto->tasks[i].in_use && proto->tasks[i].task_id == task_id) {
            return &proto->tasks[i];
        }
    }
    return NULL;
}

static bool dedup_check(const uart_proto_task_slot_t *slot, uart_proto_device_t src_device,
                         uint8_t src_task, uint16_t msg_index)
{
    for (int i = 0; i < UART_PROTO_DEDUP_DEPTH; ++i) {
        if (slot->dedup[i].valid && slot->dedup[i].src_device == src_device &&
            slot->dedup[i].src_task == src_task && slot->dedup[i].msg_index == msg_index) {
            return true; /* already processed (delivered + ACKed) this exact message */
        }
    }
    return false;
}

/* Only call once the message has actually been handed off (e.g. enqueued
 * into the destination task's inbox) -- recording before that point would
 * make a message dropped for being un-enqueueable (inbox full) look like a
 * duplicate on the sender's retry, so it would get falsely ACKed without
 * ever being delivered. */
static void dedup_record(uart_proto_task_slot_t *slot, uart_proto_device_t src_device,
                          uint8_t src_task, uint16_t msg_index)
{
    slot->dedup[slot->dedup_next].valid = true;
    slot->dedup[slot->dedup_next].src_device = src_device;
    slot->dedup[slot->dedup_next].src_task = src_task;
    slot->dedup[slot->dedup_next].msg_index = msg_index;
    slot->dedup_next = (uint8_t)((slot->dedup_next + 1) % UART_PROTO_DEDUP_DEPTH);
}

static void send_control_frame(uart_protocol_t *proto, uart_proto_msg_type_t type, uint16_t msg_index,
                                uart_proto_device_t dst_device, uint8_t dst_task,
                                uart_proto_device_t src_device, uint8_t src_task)
{
    uint8_t raw[HEADER_LEN + 2];
    raw[0] = (uint8_t)type;
    raw[1] = (uint8_t)(msg_index >> 8);
    raw[2] = (uint8_t)(msg_index & 0xFF);
    raw[3] = (uint8_t)src_device;
    raw[4] = src_task;
    raw[5] = (uint8_t)dst_device;
    raw[6] = dst_task;
    raw[7] = 0; /* length */
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN);
    raw[8] = (uint8_t)(crc >> 8);
    raw[9] = (uint8_t)(crc & 0xFF);
    frame_and_send(proto, raw, sizeof(raw), 1000);
}

static void handle_raw_frame(uart_protocol_t *proto, const uint8_t *raw, size_t len)
{
    if (len < HEADER_LEN + 2) {
        return; /* too short to even hold a header + CRC */
    }
    uint8_t length = raw[7];
    if (len != HEADER_LEN + length + 2u) {
        /* The port number is load-bearing, not decoration. Two independent
         * uart_protocol_t instances run in this firmware -- the PC link and
         * the isolated link to the safety processor -- and they share
         * this TAG, so an unqualified message here is genuinely ambiguous
         * about which wire is misbehaving. That cost a long detour on
         * 2026-08-23: this exact line was read as a fault on the isolated
         * link and chased there for some time, until SWD instrumentation on
         * the RP2040 proved it had never sent a frame anywhere near this
         * size, so the frame had to be coming from the PC link all along.
         *
         * Note the two numbers are not directly comparable and never were:
         * `length` is the header's PAYLOAD length, `len` is the whole
         * assembled raw frame, so the expected total is HEADER_LEN + length +
         * 2. Both are printed with their meaning spelled out now, because
         * subtracting one from the other looks meaningful and is not. */
        ESP_LOGW(TAG, "uart%d: frame length mismatch (header declares %u payload bytes, so "
                      "expected %u total, got %u) hdr=%02x %02x %02x %02x %02x %02x %02x %02x "
                      "tail=%02x %02x %02x %02x",
                 (int)proto->owner->port, length, (unsigned)(HEADER_LEN + length + 2u),
                 (unsigned)len,
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                 raw[len - 4u], raw[len - 3u], raw[len - 2u], raw[len - 1u]);
        proto->frame_length_mismatch++;
        return;
    }

    uint16_t expected_crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + length);
    uint16_t actual_crc = (uint16_t)((raw[HEADER_LEN + length] << 8) | raw[HEADER_LEN + length + 1]);
    if (expected_crc != actual_crc) {
        ESP_LOGW(TAG, "uart%d: frame CRC mismatch, dropping", (int)proto->owner->port);
        proto->frame_crc_mismatch++;
        return;
    }
    proto->frames_deframed++; /* CRC-valid, any type/dest -- see uart_protocol_t's own doc comment */

    uart_proto_msg_type_t type = (uart_proto_msg_type_t)raw[0];
    uint16_t msg_index = (uint16_t)((raw[1] << 8) | raw[2]);
    uart_proto_device_t src_device = (uart_proto_device_t)raw[3];
    uint8_t src_task = raw[4];
    uart_proto_device_t dst_device = (uart_proto_device_t)raw[5];
    uint8_t dst_task = raw[6];

    if (type == UART_PROTO_MSG_ACK || type == UART_PROTO_MSG_NACK) {
        /* A reply to something *we* sent: the replier is our original dst. */
        if (src_device == proto->awaited_device && src_task == proto->awaited_task &&
            msg_index == proto->awaited_index) {
            proto->ack_result = type;
            xSemaphoreGive(proto->ack_sem);
        } else {
            /* Well-formed ACK/NACK, but not for anything we're currently
             * waiting on (stale retry, or this side isn't in an exchange
             * right now) -- counted, not silently swallowed. */
            proto->frames_routed_nowhere++;
        }
        return;
    }

    if (type == UART_PROTO_MSG_BROADCAST) {
        if (dst_device != proto->own_device) {
            return; /* not for us */
        }
        xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
        uart_proto_task_slot_t *bslot = find_slot(proto, dst_task);
        if (!bslot) {
            proto->frames_routed_nowhere++;
            xSemaphoreGive(proto->tasks_lock);
            return; /* fire-and-forget: no NACK, unregistered task is just dropped */
        }
        uart_proto_message_t bmsg = {
            .device = src_device,
            .task_id = src_task,
            .msg_index = msg_index,
            .length = length,
        };
        memcpy(bmsg.payload, &raw[HEADER_LEN], length);
        /* No dedup, no reply either way: a lost or duplicated broadcast is
         * the sender's problem to notice (staleness), never this layer's --
         * EXCEPT for "the inbox was already full", which the sender can
         * never even find out about (no ACK, no NACK, no retry on a
         * BROADCAST) and which used to leave zero trace anywhere on this
         * side either. 2026-08-23: counted now, per task, so it is at least
         * visible to whoever owns that inbox (safety_link.c's GET_LINK_STATS
         * mirror, for task 7) -- see uart_proto_task_slot_t::
         * broadcast_dropped's own doc comment. */
        if (xQueueSend(bslot->inbox, &bmsg, 0) != pdTRUE) {
            bslot->broadcast_dropped++;
            ESP_LOGW(TAG, "uart%d: inbox full for task %u, BROADCAST dropped (no retry possible)",
                     (int)proto->owner->port, dst_task);
        }
        xSemaphoreGive(proto->tasks_lock);
        return;
    }

    if (type != UART_PROTO_MSG_DATA) {
        return;
    }

    if (dst_device != proto->own_device) {
        return; /* not for us (shouldn't happen point-to-point, but be safe) */
    }

    xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
    uart_proto_task_slot_t *slot = find_slot(proto, dst_task);
    if (!slot) {
        proto->frames_routed_nowhere++;
        xSemaphoreGive(proto->tasks_lock);
        ESP_LOGW(TAG, "uart%d: dst task %u not registered, replying NACK (undeliverable)",
                 (int)proto->owner->port, dst_task);
        send_control_frame(proto, UART_PROTO_MSG_NACK, msg_index, src_device, src_task,
                            proto->own_device, dst_task);
        return;
    }

    if (dedup_check(slot, src_device, src_task, msg_index)) {
        /* Already delivered on a prior attempt; our ACK for that attempt
         * must have been lost. Re-ACK without re-delivering. */
        xSemaphoreGive(proto->tasks_lock);
        send_control_frame(proto, UART_PROTO_MSG_ACK, msg_index, src_device, src_task,
                            proto->own_device, dst_task);
        return;
    }

    uart_proto_message_t msg = {
        .device = src_device,
        .task_id = src_task,
        .msg_index = msg_index,
        .length = length,
    };
    memcpy(msg.payload, &raw[HEADER_LEN], length);

    if (xQueueSend(slot->inbox, &msg, 0) != pdTRUE) {
        /* Inbox full: don't record or ACK. Recording here (before delivery
         * actually succeeded) would make the sender's retry look like a
         * duplicate of an already-processed message on the next pass
         * through this function, and it would get falsely ACKed without
         * ever having been enqueued. Leaving it unrecorded means the retry
         * is handled fresh, giving the receiving task time to drain its
         * queue before we accept this message. */
        xSemaphoreGive(proto->tasks_lock);
        ESP_LOGW(TAG, "uart%d: inbox full for task %u, withholding ACK (sender will retry)",
                 (int)proto->owner->port, dst_task);
        return;
    }
    dedup_record(slot, src_device, src_task, msg_index);
    xSemaphoreGive(proto->tasks_lock);

    send_control_frame(proto, UART_PROTO_MSG_ACK, msg_index, src_device, src_task,
                        proto->own_device, dst_task);
}

/* Reads directly from the port rather than going through
 * uart_owner_transfer: that path allocates a semaphore per call and (by
 * design, see uart_owner.c) is meant for discrete request/response
 * transactions, not a dedicated task continuously draining the byte stream.
 * This task is the sole RX consumer of the port once the protocol is
 * attached, so bypassing the owner's queue here is safe. */
static void uart_protocol_rx_task(void *arg)
{
    uart_protocol_t *proto = (uart_protocol_t *)arg;
    /* A whole worst-case frame of capacity, but NEVER waited on as a whole.
     *
     * 2026-08-28 (second pass, live bench): 3149393 raised this buffer from
     * 32 to STUFFED_FRAME_MAX to cut the number of scheduler round trips
     * needed to assemble a big CONFIG_PAGE -- and left the read itself as
     * uart_read_bytes(port, chunk, sizeof(chunk), 200ms). That call does not
     * return early with whatever has arrived: ESP-IDF's uart_read_bytes()
     * keeps consuming and re-blocking until it has `length` bytes or the
     * ticks_to_wait budget is gone. Asking for 528 bytes on a link whose
     * frames are ~40 bytes and whose busiest moment is ~10 frames/s means the
     * length is never satisfied, so every read holds its bytes for the FULL
     * 200 ms before handing them up. The safety poll's whole reply budget is
     * SAFETY_LINK_REPLY_TIMEOUT_MS (~345 ms), and it does not survive a
     * receiver that quantises delivery into 200 ms buckets on a peer that is
     * answering every request: measured live on the bench afterwards as 42
     * timeouts in 42 polls -- 100% -- while 423 STATUS frames arrived in the
     * same window with zero CRC errors, zero resyncs and zero drops. That is
     * SAFETY_FAULT_SRC_SAFETY_LINK asserting continuously, which blocks all
     * heating. The bytes were never lost; they were only ever late.
     *
     * The buffer size was not the mistake -- coupling it to the wait was.
     * Keep the large buffer (a big frame still arrives in one or two reads,
     * which is what 3149393 wanted) but never block for it to fill:
     *
     *   - if the driver already has bytes buffered, take up to a bufferful of
     *     exactly what is there, with a ZERO timeout -- a short frame is
     *     handed up the instant it lands, whatever the buffer's capacity;
     *   - only when nothing is buffered, block for a single byte, bounded by
     *     UART_PROTOCOL_RX_IDLE_POLL_MS. Asking for one byte means the wait
     *     ends on the first byte of the next frame rather than on the
     *     timeout, so the bound is a shutdown-noticing tick, not added
     *     latency.
     *
     * 2026-08-28 (third pass): the buffer was raised again, 528 ->
     * UART_PROTOCOL_RX_CHUNK_BYTES (2048), at the owner's request. That is
     * headroom, not a fix -- with the read shape below, capacity only sets
     * how many bytes ONE wakeup may carry (now up to ~4 back-to-back frames
     * instead of one), and never how long an arrived byte waits.
     *
     * Read latency is therefore governed by arrival, not by capacity, which
     * is the invariant to preserve if this loop is ever reworked again (DMA
     * included): a receiver may schedule against a buffer far larger than the
     * frame it is about to get, but it must never wait on that buffer
     * filling. */
    uint8_t chunk[UART_PROTOCOL_RX_CHUNK_BYTES];
    uint8_t raw[RAW_FRAME_MAX];
    size_t raw_len = 0;
    bool in_frame = false;
    bool escaped = false;

    while (!proto->shutdown_requested) {
        int n = 0;
        size_t buffered = 0;
        if (uart_get_buffered_data_len(proto->owner->port, &buffered) == ESP_OK && buffered > 0) {
            size_t want = (buffered > sizeof(chunk)) ? sizeof(chunk) : buffered;
            n = uart_read_bytes(proto->owner->port, chunk, want, 0);
        } else {
            /* Nothing buffered: block on the FIRST byte only. */
            n = uart_read_bytes(proto->owner->port, chunk, 1,
                                pdMS_TO_TICKS(UART_PROTOCOL_RX_IDLE_POLL_MS));
        }
        if (n <= 0) {
            continue; /* timeout is just a poll interval, lets us notice shutdown */
        }

        for (int i = 0; i < n; ++i) {
            uint8_t byte = chunk[i];

            if (byte == FRAME_DELIM) {
                if (in_frame && raw_len > 0) {
                    handle_raw_frame(proto, raw, raw_len);
                }
                raw_len = 0;
                in_frame = true;
                escaped = false;
                continue;
            }

            if (!in_frame) {
                continue; /* discard noise before the first delimiter */
            }

            if (escaped) {
                byte = (uint8_t)(byte ^ FRAME_ESC_XOR);
                escaped = false;
            } else if (byte == FRAME_ESC) {
                escaped = true;
                continue;
            }

            if (raw_len < sizeof(raw)) {
                raw[raw_len++] = byte;
            } else {
                /* Oversized/corrupt frame: resync on next delimiter. */
                in_frame = false;
                proto->frame_resync++;
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t uart_protocol_init(uart_protocol_t *proto,
                              uart_owner_t *owner,
                              uart_proto_device_t own_device,
                              UBaseType_t task_priority,
                              uint32_t stack_depth,
                              BaseType_t core_id)
{
    if (!proto || !owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(proto, 0, sizeof(*proto));
    proto->owner = owner;
    proto->own_device = own_device;

    /* Attach (not init) to the port owner already installed the driver for --
     * see hal_uart_attach()'s doc comment and frame_and_send()'s Phase 1b
     * comment. owner->initialized is already checked above. */
    hal_status_t attach_result = hal_uart_attach(&proto->hal_uart, (int)owner->port);
    if (attach_result != HAL_OK) {
        ESP_LOGE(TAG, "hal_uart_attach failed: %s", hal_status_to_name(attach_result));
        return ESP_ERR_INVALID_STATE;
    }

    proto->tasks_lock = xSemaphoreCreateMutex();
    proto->tx_lock = xSemaphoreCreateMutex();
    proto->ack_sem = xSemaphoreCreateBinary();
    if (!proto->tasks_lock || !proto->tx_lock || !proto->ack_sem) {
        ESP_LOGE(TAG, "failed to allocate sync primitives");
        uart_protocol_deinit(proto);
        return ESP_ERR_NO_MEM;
    }

    /* PSRAM stack (2026-08-27). This task's whole call graph is
     * uart_read_bytes() plus handle_raw_frame() -- CRC, dedup, memcpy, and a
     * control-frame reply. It touches no NVS and no flash, which is the rule
     * that governs a PSRAM stack: a flash operation from one asserts inside
     * ESP-IDF's cache-disable path (see safety_cfg_store.c's deferred flush
     * for that incident). Every consumer that DOES write flash reads its
     * frames out of an inbox on its own task, not on this one.
     *
     * Worth stating for the safety-link instance in particular, since this
     * task carries the telemetry that gates all heating: a PSRAM stack does
     * NOT make it less able to run while flash is being written. Ordinary
     * code lives in flash and is equally unrunnable with the cache off
     * whatever the stack is made of; the only thing that changes is where the
     * frame bytes are staged. What it does change is 4 kB of internal DRAM
     * per instance -- and there are two instances, the PC link and the safety
     * link. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(uart_protocol_rx_task,
                                                         "uart_proto_rx",
                                                         stack_depth,
                                                         proto,
                                                         task_priority,
                                                         &proto->rx_task_handle,
                                                         core_id,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create rx task");
        uart_protocol_deinit(proto);
        return ESP_ERR_NO_MEM;
    }

    proto->initialized = true;
    return ESP_OK;
}

esp_err_t uart_protocol_deinit(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }

    proto->shutdown_requested = true;
    if (proto->rx_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(250)); /* let the rx task's poll loop exit on its own */
        proto->rx_task_handle = NULL;
    }

    if (proto->tasks_lock) {
        xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
        for (int i = 0; i < UART_PROTO_MAX_TASKS; ++i) {
            if (proto->tasks[i].in_use && proto->tasks[i].inbox) {
                vQueueDeleteWithCaps(proto->tasks[i].inbox);
            }
        }
        memset(proto->tasks, 0, sizeof(proto->tasks));
        xSemaphoreGive(proto->tasks_lock);
        vSemaphoreDelete(proto->tasks_lock);
    }
    if (proto->tx_lock) {
        vSemaphoreDelete(proto->tx_lock);
    }
    if (proto->ack_sem) {
        vSemaphoreDelete(proto->ack_sem);
    }

    proto->initialized = false;
    return ESP_OK;
}

esp_err_t uart_protocol_register_task(uart_protocol_t *proto,
                                       uint8_t task_id,
                                       UBaseType_t inbox_len,
                                       QueueHandle_t *out_inbox)
{
    if (!proto || !proto->initialized || !out_inbox) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
    if (find_slot(proto, task_id)) {
        xSemaphoreGive(proto->tasks_lock);
        return ESP_ERR_INVALID_STATE; /* already registered */
    }

    uart_proto_task_slot_t *slot = NULL;
    for (int i = 0; i < UART_PROTO_MAX_TASKS; ++i) {
        if (!proto->tasks[i].in_use) {
            slot = &proto->tasks[i];
            break;
        }
    }
    if (!slot) {
        xSemaphoreGive(proto->tasks_lock);
        return ESP_ERR_NO_MEM;
    }

    /* xQueueCreate's storage must come from internal SRAM (FreeRTOS kernel
     * objects are never satisfied from PSRAM, regardless of how much overall
     * heap -- PSRAM included -- heap_caps_get_largest_free_block() reports as
     * free), and that specific pool is what wifi_prov_start()'s STA+AP
     * bring-up leans on hardest during its first couple of seconds. On the
     * bench (2026-08-18) that window landed squarely on this call for
     * whichever task happened to be starting at the time -- THERMO first,
     * then every task after it in main.c's registration order, all failing
     * ESP_ERR_NO_MEM back-to-back despite megabytes of PSRAM sitting idle --
     * and because this function is one-shot, the affected tasks stayed
     * unregistered (silently NACKing every request) for the rest of that
     * boot. Retry through the same transient window instead of taking its
     * outcome as final: a handful of short waits costs at most ~250 ms of
     * boot time in the failure case, versus a task that never comes up until
     * the next power cycle. */
    QueueHandle_t inbox = NULL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        /* PSRAM-backed, 2026-08-20. sizeof(uart_proto_message_t) is dominated
         * by its fixed 128-byte payload buffer, so an inbox of any useful
         * depth is a multi-KB contiguous request -- and this board's internal
         * heap fragments to a largest free block under 1KB during boot, so
         * plain xQueueCreate() failed here for most task ids ("internal SRAM
         * genuinely exhausted", logged just below). A registered task with no
         * inbox still runs but NACKs every request, which is how the
         * gpio_probe/wifi/profiles surfaces appeared broken.
         *
         * Safe in PSRAM because these inboxes are strictly task-to-task:
         * uart_protocol_rx_task() posts into them and the owning bridge task
         * drains them. Nothing here posts from an ISR -- that would require
         * internal memory, since PSRAM is unreachable while the cache is
         * disabled. */
        inbox = xQueueCreateWithCaps(inbox_len, sizeof(uart_proto_message_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (inbox) {
            break;
        }
        if (attempt < 4) {
            xSemaphoreGive(proto->tasks_lock);
            vTaskDelay(pdMS_TO_TICKS(50));
            xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
            /* Another task may have taken this task_id or the free slot
             * while the lock was released; re-check both rather than trust
             * the ones captured before the delay. */
            if (find_slot(proto, task_id)) {
                xSemaphoreGive(proto->tasks_lock);
                return ESP_ERR_INVALID_STATE;
            }
            slot = NULL;
            for (int i = 0; i < UART_PROTO_MAX_TASKS; ++i) {
                if (!proto->tasks[i].in_use) {
                    slot = &proto->tasks[i];
                    break;
                }
            }
            if (!slot) {
                xSemaphoreGive(proto->tasks_lock);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    if (!inbox) {
        ESP_LOGW(TAG, "task %u: xQueueCreate still failing after 5 attempts (~200ms) -- "
                       "internal SRAM genuinely exhausted, not just a boot-time WiFi transient",
                 task_id);
        xSemaphoreGive(proto->tasks_lock);
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->task_id = task_id;
    slot->inbox = inbox;
    *out_inbox = inbox;

    xSemaphoreGive(proto->tasks_lock);
    return ESP_OK;
}

esp_err_t uart_protocol_unregister_task(uart_protocol_t *proto, uint8_t task_id)
{
    if (!proto || !proto->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
    uart_proto_task_slot_t *slot = find_slot(proto, task_id);
    if (!slot) {
        xSemaphoreGive(proto->tasks_lock);
        return ESP_ERR_NOT_FOUND;
    }
    vQueueDeleteWithCaps(slot->inbox);
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(proto->tasks_lock);
    return ESP_OK;
}

esp_err_t uart_protocol_get_task_broadcast_dropped(uart_protocol_t *proto, uint8_t task_id,
                                                    uint32_t *out)
{
    if (!proto || !proto->initialized || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(proto->tasks_lock, portMAX_DELAY);
    uart_proto_task_slot_t *slot = find_slot(proto, task_id);
    if (!slot) {
        xSemaphoreGive(proto->tasks_lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out = slot->broadcast_dropped;
    xSemaphoreGive(proto->tasks_lock);
    return ESP_OK;
}

esp_err_t uart_protocol_get_deframe_stats(uart_protocol_t *proto, uint32_t *out_frames_deframed,
                                          uint32_t *out_frames_routed_nowhere,
                                          uint32_t *out_frame_length_mismatch,
                                          uint32_t *out_frame_crc_mismatch,
                                          uint32_t *out_frame_resync)
{
    if (!proto || !proto->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    /* No lock: single-writer (uart_protocol_rx_task() only) plain volatile
     * counters, same pattern uart_proto_task_slot_t::broadcast_dropped
     * already uses without one. */
    if (out_frames_deframed) {
        *out_frames_deframed = proto->frames_deframed;
    }
    if (out_frames_routed_nowhere) {
        *out_frames_routed_nowhere = proto->frames_routed_nowhere;
    }
    if (out_frame_length_mismatch) {
        *out_frame_length_mismatch = proto->frame_length_mismatch;
    }
    if (out_frame_crc_mismatch) {
        *out_frame_crc_mismatch = proto->frame_crc_mismatch;
    }
    if (out_frame_resync) {
        *out_frame_resync = proto->frame_resync;
    }
    return ESP_OK;
}

uint32_t uart_protocol_get_tx_send_failures(uart_protocol_t *proto)
{
    if (!proto || !proto->initialized) {
        return 0;
    }
    /* Single-writer (frame_and_send(), under tx_lock -- see this field's own
     * doc comment in uart_protocol.h), plain volatile read from any task. */
    return proto->tx_send_failures;
}

esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{
    if (!inbox || !out_msg) {
        return ESP_ERR_INVALID_ARG;
    }
    return (xQueueReceive(inbox, out_msg, wait_ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t uart_protocol_send_limited(uart_protocol_t *proto,
                                      uart_proto_device_t dst_device,
                                      uint8_t dst_task,
                                      uint8_t src_task,
                                      const uint8_t *payload,
                                      size_t length,
                                      uint32_t ack_timeout_ms,
                                      int max_retries)
{
    if (!proto || !proto->initialized || length > UART_PROTO_MAX_PAYLOAD || (length > 0 && !payload) ||
        max_retries < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(proto->tx_lock, portMAX_DELAY);

    uint16_t msg_index = proto->next_tx_index++;

    uint8_t raw[HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2];
    raw[0] = (uint8_t)UART_PROTO_MSG_DATA;
    raw[1] = (uint8_t)(msg_index >> 8);
    raw[2] = (uint8_t)(msg_index & 0xFF);
    raw[3] = (uint8_t)proto->own_device;
    raw[4] = src_task;
    raw[5] = (uint8_t)dst_device;
    raw[6] = dst_task;
    raw[7] = (uint8_t)length;
    if (length > 0) {
        memcpy(&raw[HEADER_LEN], payload, length);
    }
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + length);
    raw[HEADER_LEN + length] = (uint8_t)(crc >> 8);
    raw[HEADER_LEN + length + 1] = (uint8_t)(crc & 0xFF);
    size_t raw_len = HEADER_LEN + length + 2;

    proto->awaited_device = dst_device;
    proto->awaited_task = dst_task;
    proto->awaited_index = msg_index;
    xSemaphoreTake(proto->ack_sem, 0); /* clear any stale signal */

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (frame_and_send(proto, raw, raw_len, 1000) == 0) {
            continue; /* tx itself failed; still worth retrying */
        }
        if (xSemaphoreTake(proto->ack_sem, pdMS_TO_TICKS(ack_timeout_ms)) == pdTRUE) {
            /* Any reply -- ACK or NACK -- proves a peer is alive and
             * answering frames at (dst_device, dst_task), which is exactly
             * the distinction the suppression below needs. */
            peer_mark_replied(proto, dst_device, dst_task);
            result = (proto->ack_result == UART_PROTO_MSG_ACK) ? ESP_OK : ESP_ERR_NOT_FOUND;
            break;
        }
        /* Suppressed entirely (not even rate-limited) when this destination
         * has NEVER once replied: that is the safety link's normal state
         * until the RP2040 firmware exists, and safety_link.c's
         * safety_update_health() already logs that condition sensibly, once,
         * at its own slow rate ("no reply from the safety processor (never
         * seen one)") -- this per-retry warning under it added nothing but
         * noise, at exactly the RETRY_LOG_INTERVAL_US/5s cadence that flooded
         * uart_log_bridge's queue. A destination that WAS replying and then
         * stopped is a real fault and must stay loud, so the rate-limited
         * warning below still fires for that case. */
        if (!peer_ever_replied(proto, dst_device, dst_task)) {
            continue;
        }
        /* Rate-limited: see uart_protocol.h's last_retry_log_us comment. One
         * line every RETRY_LOG_INTERVAL_US, carrying however many were
         * suppressed since -- so a genuinely absent peer stays visible
         * without drowning every other log line on the board. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - proto->last_retry_log_us >= RETRY_LOG_INTERVAL_US) {
            if (proto->suppressed_retry_logs > 0) {
                ESP_LOGW(TAG, "uart%d: no reply for msg %u to dev%u/task%u, retry %d/%d (+%lu more suppressed)",
                         (int)proto->owner->port,
                         msg_index, dst_device, dst_task, attempt + 1, max_retries,
                         (unsigned long)proto->suppressed_retry_logs);
            } else {
                ESP_LOGW(TAG, "uart%d: no reply for msg %u to dev%u/task%u, retry %d/%d",
                         (int)proto->owner->port, msg_index, dst_device,
                         dst_task, attempt + 1, max_retries);
            }
            proto->last_retry_log_us = now_us;
            proto->suppressed_retry_logs = 0;
        } else {
            proto->suppressed_retry_logs++;
        }
    }

    xSemaphoreGive(proto->tx_lock);
    return result;
}

/* Thin wrapper: the original public entry point, now just
 * uart_protocol_send_limited() pinned to the full UART_PROTO_MAX_RETRIES.
 * Every existing caller (safety_link.c, uart_bridge.c's request/reply
 * handlers, etc.) keeps its current retry budget unchanged -- only
 * uart_log_bridge.c has moved to the limited form directly. */
esp_err_t uart_protocol_send(uart_protocol_t *proto,
                              uart_proto_device_t dst_device,
                              uint8_t dst_task,
                              uint8_t src_task,
                              const uint8_t *payload,
                              size_t length,
                              uint32_t ack_timeout_ms)
{
    return uart_protocol_send_limited(proto, dst_device, dst_task, src_task, payload, length,
                                       ack_timeout_ms, UART_PROTO_MAX_RETRIES);
}

esp_err_t uart_protocol_send_broadcast(uart_protocol_t *proto,
                                        uart_proto_device_t dst_device,
                                        uint8_t dst_task,
                                        uint8_t src_task,
                                        const uint8_t *payload,
                                        size_t length)
{
    if (!proto || !proto->initialized || length > UART_PROTO_MAX_PAYLOAD || (length > 0 && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(proto->tx_lock, portMAX_DELAY);

    uint16_t msg_index = proto->next_tx_index++;

    uint8_t raw[HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2];
    raw[0] = (uint8_t)UART_PROTO_MSG_BROADCAST;
    raw[1] = (uint8_t)(msg_index >> 8);
    raw[2] = (uint8_t)(msg_index & 0xFF);
    raw[3] = (uint8_t)proto->own_device;
    raw[4] = src_task;
    raw[5] = (uint8_t)dst_device;
    raw[6] = dst_task;
    raw[7] = (uint8_t)length;
    if (length > 0) {
        memcpy(&raw[HEADER_LEN], payload, length);
    }
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + length);
    raw[HEADER_LEN + length] = (uint8_t)(crc >> 8);
    raw[HEADER_LEN + length + 1] = (uint8_t)(crc & 0xFF);
    size_t raw_len = HEADER_LEN + length + 2;

    /* No ack wait, no retry: the whole point is that the sender never blocks
     * on the far end replying. */
    esp_err_t result = (frame_and_send(proto, raw, raw_len, 1000) != 0) ? ESP_OK : ESP_FAIL;

    xSemaphoreGive(proto->tx_lock);
    return result;
}

