#include "uart_owner.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "uart_owner";

/* Generous headroom over a single worst-case stuffed frame (~140 bytes
 * unstuffed, up to ~2x that if every byte happened to need escaping) so a
 * burst -- e.g. the uart_log_bridge boot-time backlog, or many DATA/ACK
 * frames arriving back-to-back -- can't overrun the driver's ring buffer
 * before the owner/protocol tasks get a chance to drain it.
 *
 * NOT the same thing as UART_PROTOCOL_RX_CHUNK_BYTES (2048), and deliberately
 * not equal to it. This ring is filled by the ESP-IDF driver's own ISR and
 * emptied by uart_protocol_rx_task(); it must be big enough to absorb
 * everything that can arrive while that task is descheduled, so it is sized
 * against SCHEDULING DELAY at a priority below WiFi's. The chunk buffer is
 * sized against how much of that backlog ONE wakeup should hand up, so it is
 * sized against frame size. Keeping the ring at 2x the chunk means a single
 * read can never be the thing that empties it while more is still landing;
 * lowering it to 2048 to "match" would only shrink the overrun margin the
 * comment above is about, and buy nothing. Reviewed 2026-08-28 when the chunk
 * moved 528 -> 2048; left at 4096 on purpose. */
#define UART_OWNER_RX_RING_BUF_SIZE 4096
#define UART_OWNER_TX_RING_BUF_SIZE 4096
#define UART_OWNER_EVENT_QUEUE_LEN  16

/* Watches the driver's event queue for line/buffer errors so a flaky wire
 * (noise, wrong baud, disconnected sensor) doesn't leave stale garbage
 * sitting in the RX ring buffer for the next transaction to read.
 *
 * What it does NOT do is flush on a per-character line error. A framing or
 * parity error means one character was mangled; it says nothing about the
 * bytes already sitting in the ring behind it. uart_flush_input() throws
 * away the whole ring, so a single glitched character destroys every
 * complete frame queued behind it -- including frames that had already
 * arrived intact and were simply waiting to be read.
 *
 * That is a bad trade for this protocol, because the frame layer is already
 * self-synchronising: frames are 0x7E-delimited and CRC-checked, so the
 * parser resynchronises on the next delimiter and rejects the corrupted
 * frame on its own. Flushing cannot make a mangled frame good; it can only
 * take good ones with it.
 *
 * It is a *ruinous* trade on the isolated safety link specifically. That
 * line sits in a break condition whenever the far end is in reset (see
 * safety_link.h), and a peer that reboots -- or any noise on a 3 m isolated
 * link -- produces a line error every cycle, arriving interleaved with the
 * peer's telemetry. Flushing on each one deletes the telemetry, which
 * presents as "frames_received stays 0 forever while the error counters
 * climb", with a perfectly good wire and a perfectly good peer.
 *
 * So: count every line error (that is what rx_error_count and GET_LINK_STATS
 * are for -- the diagnosis must stay visible), but only flush when the ring
 * genuinely holds unusable state, i.e. FIFO overflow and ring-full, where
 * bytes have already been dropped and framing is broken by definition. */
static void uart_owner_event_task(void *arg)
{
    uart_owner_t *owner = (uart_owner_t *)arg;
    uart_event_t event;

    while (true) {
        if (xQueueReceive(owner->event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
            case UART_DATA:
                break;
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "hw fifo overflow on uart%d", owner->port);
                owner->rx_error_count++;
                uart_flush_input(owner->port);
                xQueueReset(owner->event_queue);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "rx ring buffer full on uart%d", owner->port);
                owner->rx_error_count++;
                uart_flush_input(owner->port);
                xQueueReset(owner->event_queue);
                break;
            case UART_BREAK:
                ESP_LOGW(TAG, "break condition on uart%d (line idle/disconnected?)", owner->port);
                owner->rx_error_count++;
                break;
            case UART_PARITY_ERR:
                /* Counted, not flushed -- see this task's header comment. */
                ESP_LOGW(TAG, "parity error on uart%d", owner->port);
                owner->rx_error_count++;
                break;
            case UART_FRAME_ERR:
                /* Counted, not flushed -- see this task's header comment.
                 * The frame parser resynchronises on the next 0x7E and the
                 * CRC rejects whatever this corrupted; flushing here would
                 * additionally delete every intact frame behind it. */
                ESP_LOGW(TAG, "frame error on uart%d (check baud/wiring)", owner->port);
                owner->rx_error_count++;
                break;
            default:
                break;
        }
    }
}

/* uart_owner_task() (the request-queue worker) and uart_owner_transfer()
 * were deleted 2026-09-06 (uart collapse, docs/HW_ABSTRACTION_PLAN.md): a
 * 2026-09-05 grep-confirmed audit found uart_owner_transfer() had zero real
 * callers left in KilnFW -- uart_protocol.c's frame_and_send() (its
 * historical caller) was switched onto hal_uart_attach()/hal_uart_send_
 * blocking, and safety_link.c's one remaining textual match was a comment
 * describing the old design, not a call site. See git history (this
 * function's prior body) for the full TX/RX request-queue implementation
 * that was removed. */

esp_err_t uart_owner_init(uart_owner_t *owner,
                           uart_port_t port,
                           int tx_io,
                           int rx_io,
                           int baud_rate,
                           UBaseType_t queue_len,
                           UBaseType_t task_priority,
                           uint32_t stack_depth,
                           BaseType_t core_id)
{
    if (!owner) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(owner, 0, sizeof(*owner));
    owner->port = port;

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(port, UART_OWNER_RX_RING_BUF_SIZE, UART_OWNER_TX_RING_BUF_SIZE,
                                         UART_OWNER_EVENT_QUEUE_LEN, &owner->event_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(port);
        return err;
    }

    err = uart_set_pin(port, tx_io, rx_io, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(port);
        return err;
    }

    /* queue_len is unused now that the request-queue worker task is gone
     * (see this file's header comment above uart_owner_init) -- kept as a
     * parameter for source compatibility with existing call sites. */
    (void)queue_len;

    BaseType_t event_task_created = xTaskCreatePinnedToCore(uart_owner_event_task,
                                                             "uart_owner_evt_task",
                                                             stack_depth,
                                                             owner,
                                                             task_priority,
                                                             &owner->event_task_handle,
                                                             core_id);
    if (event_task_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create event task");
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }

    owner->initialized = true;
    return ESP_OK;
}

esp_err_t uart_owner_deinit(uart_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The event task has no sentinel of its own -- it just blocks on the
     * driver's event queue -- so it must be torn down before
     * uart_driver_delete() frees that queue out from under it. */
    if (owner->event_task_handle) {
        vTaskDelete(owner->event_task_handle);
    }

    uart_driver_delete(owner->port);

    owner->event_task_handle = NULL;
    owner->event_queue = NULL;
    owner->initialized = false;
    return ESP_OK;
}

uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner)
{
    if (!owner) {
        return 0;
    }
    return owner->rx_error_count;
}

esp_err_t uart_owner_restart(uart_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = uart_flush_input(owner->port);
    if (err == ESP_OK) {
        owner->rx_error_count = 0;
    }
    return err;
}
