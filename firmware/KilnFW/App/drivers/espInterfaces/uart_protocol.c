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

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    return kilnlink_crc16_ccitt_false(data, len);
}

static size_t stuff_and_send(uart_protocol_t *proto, const uint8_t *raw, size_t raw_len, uint32_t timeout_ms)
{
    uint8_t out[STUFFED_FRAME_MAX];
    size_t o = kilnlink_stuff(raw, raw_len, out, sizeof(out));
    if (o == 0) {
        ESP_LOGW(TAG, "frame stuffing failed (raw_len=%u exceeds STUFFED_FRAME_MAX capacity)",
                 (unsigned)raw_len);
        return 0;
    }

    esp_err_t err = uart_owner_transfer(proto->owner, out, o, NULL, 0, NULL, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "frame tx failed: %s", esp_err_to_name(err));
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
    uint16_t crc = crc16_ccitt_false(raw, HEADER_LEN);
    raw[8] = (uint8_t)(crc >> 8);
    raw[9] = (uint8_t)(crc & 0xFF);
    stuff_and_send(proto, raw, sizeof(raw), 1000);
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

    uint16_t expected_crc = crc16_ccitt_false(raw, HEADER_LEN + length);
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
    uint8_t chunk[32];
    uint8_t raw[RAW_FRAME_MAX];
    size_t raw_len = 0;
    bool in_frame = false;
    bool escaped = false;

    while (!proto->shutdown_requested) {
        int n = uart_read_bytes(proto->owner->port, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
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

    proto->tasks_lock = xSemaphoreCreateMutex();
    proto->tx_lock = xSemaphoreCreateMutex();
    proto->ack_sem = xSemaphoreCreateBinary();
    if (!proto->tasks_lock || !proto->tx_lock || !proto->ack_sem) {
        ESP_LOGE(TAG, "failed to allocate sync primitives");
        uart_protocol_deinit(proto);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(uart_protocol_rx_task,
                                                  "uart_proto_rx",
                                                  stack_depth,
                                                  proto,
                                                  task_priority,
                                                  &proto->rx_task_handle,
                                                  core_id);
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
    uint16_t crc = crc16_ccitt_false(raw, HEADER_LEN + length);
    raw[HEADER_LEN + length] = (uint8_t)(crc >> 8);
    raw[HEADER_LEN + length + 1] = (uint8_t)(crc & 0xFF);
    size_t raw_len = HEADER_LEN + length + 2;

    proto->awaited_device = dst_device;
    proto->awaited_task = dst_task;
    proto->awaited_index = msg_index;
    xSemaphoreTake(proto->ack_sem, 0); /* clear any stale signal */

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (stuff_and_send(proto, raw, raw_len, 1000) == 0) {
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
    uint16_t crc = crc16_ccitt_false(raw, HEADER_LEN + length);
    raw[HEADER_LEN + length] = (uint8_t)(crc >> 8);
    raw[HEADER_LEN + length + 1] = (uint8_t)(crc & 0xFF);
    size_t raw_len = HEADER_LEN + length + 2;

    /* No ack wait, no retry: the whole point is that the sender never blocks
     * on the far end replying. */
    esp_err_t result = (stuff_and_send(proto, raw, raw_len, 1000) != 0) ? ESP_OK : ESP_FAIL;

    xSemaphoreGive(proto->tx_lock);
    return result;
}
