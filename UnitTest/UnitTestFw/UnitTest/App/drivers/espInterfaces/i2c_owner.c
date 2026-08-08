#include "i2c_owner.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "esp_i2c_owner";

static void i2c_owner_task(void *arg)
{
    i2c_owner_t *owner = (i2c_owner_t *)arg;
    i2c_owner_request_t request;

    while (true) {
        if (xQueueReceive(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (request.shutdown) {
            break;
        }

        esp_err_t result = ESP_OK;
        if (request.tx_buffer && request.tx_length > 0 && request.rx_buffer && request.rx_length > 0) {
            result = i2c_master_transmit_receive(request.device,
                                                 request.tx_buffer,
                                                 request.tx_length,
                                                 request.rx_buffer,
                                                 request.rx_length,
                                                 request.timeout_ms);
        } else if (request.tx_buffer && request.tx_length > 0) {
            result = i2c_master_transmit(request.device,
                                         request.tx_buffer,
                                         request.tx_length,
                                         request.timeout_ms);
        } else if (request.rx_buffer && request.rx_length > 0) {
            result = i2c_master_receive(request.device,
                                        request.rx_buffer,
                                        request.rx_length,
                                        request.timeout_ms);
        } else {
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
