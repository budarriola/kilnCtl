#include "i2c_owner.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "esp_i2c_owner";

/* One raw attempt at whatever request.tx/rx_buffer describe. Pulled out of
 * i2c_owner_task's loop so the retry-after-reset path below can call it
 * twice without duplicating the tx/rx/tx+rx dispatch. */
static esp_err_t i2c_owner_do_transfer(const i2c_owner_request_t *request)
{
    if (request->tx_buffer && request->tx_length > 0 && request->rx_buffer && request->rx_length > 0) {
        return i2c_master_transmit_receive(request->device,
                                            request->tx_buffer,
                                            request->tx_length,
                                            request->rx_buffer,
                                            request->rx_length,
                                            request->timeout_ms);
    }
    if (request->tx_buffer && request->tx_length > 0) {
        return i2c_master_transmit(request->device,
                                    request->tx_buffer,
                                    request->tx_length,
                                    request->timeout_ms);
    }
    if (request->rx_buffer && request->rx_length > 0) {
        return i2c_master_receive(request->device,
                                   request->rx_buffer,
                                   request->rx_length,
                                   request->timeout_ms);
    }
    return ESP_ERR_INVALID_ARG;
}

static void i2c_owner_task(void *arg)
{
    i2c_owner_t *owner = (i2c_owner_t *)arg;
    i2c_owner_request_t request;
    uint32_t consecutive_failures = 0;

    while (true) {
        if (xQueueReceive(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (request.shutdown) {
            break;
        }

        esp_err_t result = i2c_owner_do_transfer(&request);

        /* A failed/timed-out transaction (marginal wiring, a slave holding
         * SDA low mid-transfer, ...) can leave the bus wedged: every
         * subsequent transfer -- even to a different, healthy device --
         * fails the same way, since nothing else ever recovers it. Reset it
         * right here, on the owner task, then retry ONCE so the caller that
         * hit the wedge doesn't have to (that's what turned "one bad
         * transfer" into "every transfer after it, forever" -- press
         * "Print" again and it looked like the whole link was dead). Skip
         * ESP_ERR_INVALID_ARG (a caller bug, not a bus problem -- resetting
         * won't fix it and would just add noise). */
        if (result != ESP_OK && result != ESP_ERR_INVALID_ARG) {
            esp_err_t reset_err = i2c_master_bus_reset(owner->bus);
            if (reset_err != ESP_OK) {
                consecutive_failures++;
                ESP_LOGE(TAG,
                         "transfer failed (%s); bus reset ALSO failed: %s (%lu consecutive failures)",
                         esp_err_to_name(result), esp_err_to_name(reset_err),
                         (unsigned long)consecutive_failures);
            } else {
                esp_err_t retry_result = i2c_owner_do_transfer(&request);
                if (retry_result == ESP_OK) {
                    ESP_LOGW(TAG, "transfer failed (%s); bus reset recovered it on retry",
                             esp_err_to_name(result));
                    result = retry_result;
                    consecutive_failures = 0;
                } else {
                    consecutive_failures++;
                    ESP_LOGE(TAG,
                             "transfer failed (%s); bus reset done but retry ALSO failed (%s) "
                             "(%lu consecutive failures -- likely a wiring/hardware fault, not "
                             "a transient glitch)",
                             esp_err_to_name(result), esp_err_to_name(retry_result),
                             (unsigned long)consecutive_failures);
                    result = retry_result;
                }
            }
        } else {
            consecutive_failures = 0;
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

esp_err_t i2c_owner_init(i2c_owner_t *owner,
                             i2c_master_bus_handle_t bus,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id)
{
    if (!owner || !bus) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(owner, 0, sizeof(*owner));
    owner->bus = bus;
    owner->request_queue = xQueueCreate(queue_len, sizeof(i2c_owner_request_t));
    if (!owner->request_queue) {
        ESP_LOGE(TAG, "failed to create request queue");
        return ESP_ERR_NO_MEM;
    }

    owner->shutdown_done = xSemaphoreCreateBinary();
    if (!owner->shutdown_done) {
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        ESP_LOGE(TAG, "failed to create shutdown semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(i2c_owner_task,
                                                      "i2c_owner_task",
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
        ESP_LOGE(TAG, "failed to create owner task");
        return ESP_ERR_NO_MEM;
    }

    owner->initialized = true;
    return ESP_OK;
}

esp_err_t i2c_owner_deinit(i2c_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_owner_request_t shutdown_request;
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

    owner->request_queue = NULL;
    owner->task_handle = NULL;
    owner->shutdown_done = NULL;
    owner->initialized = false;
    return ESP_OK;
}

esp_err_t i2c_owner_transfer(i2c_owner_t *owner,
                                 i2c_master_dev_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 uint32_t timeout_ms)
{
    if (!owner || !owner->initialized || !owner->request_queue || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    i2c_owner_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = device;
    request.tx_buffer = tx_buffer;
    request.tx_length = tx_length;
    request.rx_buffer = rx_buffer;
    request.rx_length = rx_length;
    request.timeout_ms = timeout_ms;
    request.done_sem = done_sem;
    request.result_out = &result;

    if (xQueueSend(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return ESP_ERR_TIMEOUT;
    }

    /* The worker enforces timeout_ms internally on the I2C transaction
     * itself; waiting only that long here too would let a queue backlog
     * (other callers' pending requests ahead of this one) fire this timeout
     * before the worker has even started the transfer. If that happened,
     * returning now would delete done_sem and unwind request's stack frame
     * while the worker is still going to dequeue it, write through the
     * now-dangling result_out, and give the now-deleted semaphore --
     * use-after-free. Wait unbounded instead; the transaction is still
     * bounded by timeout_ms internally. */
    if (xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done_sem);
    return result;
}
