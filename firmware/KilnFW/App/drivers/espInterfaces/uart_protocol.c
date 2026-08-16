#include "uart_protocol.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "uart_proto";

#define FRAME_DELIM   0x7Eu
#define FRAME_ESC     0x7Du
#define FRAME_ESC_XOR 0x20u

/* Raw (unstuffed) header layout, CRC covers offsets [0, HEADER_LEN+length). */
#define HEADER_LEN 8u
/* header(8) + max payload + crc(2), before stuffing */
#define RAW_FRAME_MAX (HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2u)
/* Stuffing can at most double the bytes, plus two delimiters. */
#define STUFFED_FRAME_MAX (RAW_FRAME_MAX * 2u + 2u)

/* One no-reply warning per 5s per protocol instance (see uart_protocol.h). */
#define RETRY_LOG_INTERVAL_US 5000000

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static size_t stuff_and_send(uart_protocol_t *proto, const uint8_t *raw, size_t raw_len, uint32_t timeout_ms)
{
    uint8_t out[STUFFED_FRAME_MAX];
    size_t o = 0;
    out[o++] = FRAME_DELIM;
    for (size_t i = 0; i < raw_len; ++i) {
        uint8_t b = raw[i];
        if (b == FRAME_DELIM || b == FRAME_ESC) {
            out[o++] = FRAME_ESC;
            out[o++] = (uint8_t)(b ^ FRAME_ESC_XOR);
        } else {
            out[o++] = b;
        }
    }
    out[o++] = FRAME_DELIM;

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
        ESP_LOGW(TAG, "frame length mismatch (hdr says %u, got %u bytes)", length, (unsigned)len);
        return;
    }

    uint16_t expected_crc = crc16_ccitt_false(raw, HEADER_LEN + length);
    uint16_t actual_crc = (uint16_t)((raw[HEADER_LEN + length] << 8) | raw[HEADER_LEN + length + 1]);
    if (expected_crc != actual_crc) {
        ESP_LOGW(TAG, "frame CRC mismatch, dropping");
        return;
    }

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
        }
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
        xSemaphoreGive(proto->tasks_lock);
        ESP_LOGW(TAG, "dst task %u not registered, replying NACK (undeliverable)", dst_task);
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
        ESP_LOGW(TAG, "inbox full for task %u, withholding ACK (sender will retry)", dst_task);
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
                vQueueDelete(proto->tasks[i].inbox);
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

    QueueHandle_t inbox = xQueueCreate(inbox_len, sizeof(uart_proto_message_t));
    if (!inbox) {
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
    vQueueDelete(slot->inbox);
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(proto->tasks_lock);
    return ESP_OK;
}

esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{
    if (!inbox || !out_msg) {
        return ESP_ERR_INVALID_ARG;
    }
    return (xQueueReceive(inbox, out_msg, wait_ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t uart_protocol_send(uart_protocol_t *proto,
                              uart_proto_device_t dst_device,
                              uint8_t dst_task,
                              uint8_t src_task,
                              const uint8_t *payload,
                              size_t length,
                              uint32_t ack_timeout_ms)
{
    if (!proto || !proto->initialized || length > UART_PROTO_MAX_PAYLOAD || (length > 0 && !payload)) {
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
    for (int attempt = 0; attempt < UART_PROTO_MAX_RETRIES; ++attempt) {
        if (stuff_and_send(proto, raw, raw_len, 1000) == 0) {
            continue; /* tx itself failed; still worth retrying */
        }
        if (xSemaphoreTake(proto->ack_sem, pdMS_TO_TICKS(ack_timeout_ms)) == pdTRUE) {
            result = (proto->ack_result == UART_PROTO_MSG_ACK) ? ESP_OK : ESP_ERR_NOT_FOUND;
            break;
        }
        /* Rate-limited: see uart_protocol.h's last_retry_log_us comment. One
         * line every RETRY_LOG_INTERVAL_US, carrying however many were
         * suppressed since -- so a genuinely absent peer stays visible
         * without drowning every other log line on the board. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - proto->last_retry_log_us >= RETRY_LOG_INTERVAL_US) {
            if (proto->suppressed_retry_logs > 0) {
                ESP_LOGW(TAG, "no reply for msg %u to dev%u/task%u, retry %d/%d (+%lu more suppressed)",
                         msg_index, dst_device, dst_task, attempt + 1, UART_PROTO_MAX_RETRIES,
                         (unsigned long)proto->suppressed_retry_logs);
            } else {
                ESP_LOGW(TAG, "no reply for msg %u to dev%u/task%u, retry %d/%d", msg_index, dst_device,
                         dst_task, attempt + 1, UART_PROTO_MAX_RETRIES);
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
