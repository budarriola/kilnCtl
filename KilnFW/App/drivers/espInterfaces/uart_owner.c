#include "uart_owner.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "uart_owner";

/* Generous headroom over a single worst-case stuffed frame (~140 bytes
 * unstuffed, up to ~2x that if every byte happened to need escaping) so a
 * burst -- e.g. the uart_log_bridge boot-time backlog, or many DATA/ACK
 * frames arriving back-to-back -- can't overrun the driver's ring buffer
 * before the owner/protocol tasks get a chance to drain it. */
#define UART_OWNER_RX_RING_BUF_SIZE 4096
#define UART_OWNER_TX_RING_BUF_SIZE 4096
#define UART_OWNER_EVENT_QUEUE_LEN  16

/* Watches the driver's event queue for line/buffer errors so a flaky wire
 * (noise, wrong baud, disconnected sensor) doesn't leave stale garbage
 * sitting in the RX ring buffer for the next transaction to read. */
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
                ESP_LOGW(TAG, "parity error on uart%d", owner->port);
                owner->rx_error_count++;
                uart_flush_input(owner->port);
                break;
            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "frame error on uart%d (check baud/wiring)", owner->port);
                owner->rx_error_count++;
                uart_flush_input(owner->port);
                break;
            default:
                break;
        }
    }
}

static void uart_owner_task(void *arg)
{
    uart_owner_t *owner = (uart_owner_t *)arg;
    uart_owner_request_t request;

    while (true) {
        if (xQueueReceive(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (request.shutdown) {
            break;
        }

        esp_err_t result = ESP_OK;

        if (request.tx_buffer && request.tx_length > 0) {
            int written = uart_write_bytes(owner->port, (const char *)request.tx_buffer, request.tx_length);
            if (written != (int)request.tx_length) {
                result = ESP_FAIL;
            } else if (uart_wait_tx_done(owner->port, pdMS_TO_TICKS(request.timeout_ms)) != ESP_OK) {
                result = ESP_ERR_TIMEOUT;
            }
        }

        if (result == ESP_OK && request.rx_buffer && request.rx_length > 0) {
            /* Only flush ahead of a reply we're about to wait for, i.e. a
             * query we just transmitted -- an RX-only call (no TX phase) is
             * a plain listen and must not discard bytes already queued. */
            if (request.tx_buffer && request.tx_length > 0) {
                uart_flush_input(owner->port);
            }
            int read = uart_read_bytes(owner->port, request.rx_buffer, request.rx_length,
                                        pdMS_TO_TICKS(request.timeout_ms));
            if (read < 0) {
                result = ESP_FAIL;
            } else {
                if (request.rx_length_out) {
                    *request.rx_length_out = (size_t)read;
                }
                if ((size_t)read == 0) {
                    result = ESP_ERR_TIMEOUT;
                }
            }
        }

        if (!request.tx_buffer && !request.rx_buffer) {
            result = ESP_ERR_INVALID_ARG;
        }

        if (request.result_out) {
            *request.result_out = result;
        }

        if (request.done_sem) {
            xSemaphoreGive(request.done_sem);
        }
    }

    if (owner->shutdown_done) {
        xSemaphoreGive(owner->shutdown_done);
    }
    vTaskDelete(NULL);
}

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

    owner->request_queue = xQueueCreate(queue_len, sizeof(uart_owner_request_t));
    if (!owner->request_queue) {
        ESP_LOGE(TAG, "failed to create request queue");
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }

    owner->shutdown_done = xSemaphoreCreateBinary();
    if (!owner->shutdown_done) {
        ESP_LOGE(TAG, "failed to create shutdown semaphore");
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(uart_owner_task,
                                                       "uart_owner_task",
                                                       stack_depth,
                                                       owner,
                                                       task_priority,
                                                       &owner->task_handle,
                                                       core_id);
    if (task_created != pdPASS) {
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        vSemaphoreDelete(owner->shutdown_done);
        owner->shutdown_done = NULL;
        uart_driver_delete(port);
        ESP_LOGE(TAG, "failed to create owner task");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t event_task_created = xTaskCreatePinnedToCore(uart_owner_event_task,
                                                             "uart_owner_evt_task",
                                                             stack_depth,
                                                             owner,
                                                             task_priority,
                                                             &owner->event_task_handle,
                                                             core_id);
    if (event_task_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create event task");
        uart_owner_request_t shutdown_request;
        memset(&shutdown_request, 0, sizeof(shutdown_request));
        shutdown_request.shutdown = true;
        xQueueSend(owner->request_queue, &shutdown_request, portMAX_DELAY);
        xSemaphoreTake(owner->shutdown_done, pdMS_TO_TICKS(2000));
        vSemaphoreDelete(owner->shutdown_done);
        owner->shutdown_done = NULL;
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
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

    uart_owner_request_t shutdown_request;
    memset(&shutdown_request, 0, sizeof(shutdown_request));
    shutdown_request.shutdown = true;

    if (owner->request_queue) {
        xQueueSend(owner->request_queue, &shutdown_request, portMAX_DELAY);
    }

    /* Wait for the worker to actually confirm it drained the queue and is
     * about to exit before freeing the queue out from under it -- a fixed
     * delay can't account for a backlog of in-flight requests ahead of the
     * shutdown sentinel, each potentially taking up to their own
     * timeout_ms. */
    if (owner->shutdown_done) {
        if (xSemaphoreTake(owner->shutdown_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "worker did not confirm shutdown in time; deleting queue anyway");
        }
        vSemaphoreDelete(owner->shutdown_done);
    }

    if (owner->request_queue) {
        vQueueDelete(owner->request_queue);
    }

    /* The event task has no sentinel of its own -- it just blocks on the
     * driver's event queue -- so it must be torn down before
     * uart_driver_delete() frees that queue out from under it. */
    if (owner->event_task_handle) {
        vTaskDelete(owner->event_task_handle);
    }

    uart_driver_delete(owner->port);

    owner->request_queue = NULL;
    owner->task_handle = NULL;
    owner->event_task_handle = NULL;
    owner->event_queue = NULL;
    owner->shutdown_done = NULL;
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

esp_err_t uart_owner_transfer(uart_owner_t *owner,
                               const uint8_t *tx_buffer,
                               size_t tx_length,
                               uint8_t *rx_buffer,
                               size_t rx_length,
                               size_t *rx_length_out,
                               uint32_t timeout_ms)
{
    if (!owner || !owner->initialized || !owner->request_queue) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    uart_owner_request_t request;
    memset(&request, 0, sizeof(request));
    request.tx_buffer = tx_buffer;
    request.tx_length = tx_length;
    request.rx_buffer = rx_buffer;
    request.rx_length = rx_length;
    request.rx_length_out = rx_length_out;
    request.timeout_ms = timeout_ms;
    request.done_sem = done_sem;
    request.result_out = &result;

    if (xQueueSend(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return ESP_ERR_TIMEOUT;
    }

    /* The worker enforces timeout_ms internally on each phase (TX flush, RX
     * read); wait unbounded here so a TX+RX transaction isn't cut off by the
     * queue wait before the worker has finished both phases. */
    if (xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done_sem);
    return result;
}
