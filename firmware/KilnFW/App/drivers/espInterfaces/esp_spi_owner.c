#include "esp_spi_owner.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "esp_spi_owner";

/* Bound on both halves of spi_owner_transfer()'s wait (queuing the request,
 * then waiting for the owner task to complete it). Generous on purpose: a
 * real transfer -- display flush or MAX31856 register poke alike -- is
 * microseconds to low single-digit milliseconds (DISPLAY_ST7796_PLAN.md
 * section 9), so this only ever fires when the owner task is genuinely
 * stuck (e.g. wedged inside spi_device_transmit()), never as a false alarm
 * against a slow-but-healthy transfer. Must not be tightened without
 * re-checking the largest real flush chunk this bus ever pushes. */
#define SPI_OWNER_TRANSFER_TIMEOUT_MS 1000u

static void spi_owner_task(void *arg)
{
    spi_owner_t *owner = (spi_owner_t *)arg;
    spi_owner_request_t request;

    while (true) {
        if (xQueueReceive(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (request.shutdown) {
            break;
        }

        esp_err_t result = ESP_OK;
        if (request.tx_buffer && request.tx_length > 0) {
            gpio_set_level((gpio_num_t)request.cs_pin, 0);
            spi_transaction_t trans = {
                .length = request.tx_length * 8,
                .tx_buffer = request.tx_buffer,
                .rx_buffer = request.rx_buffer,
                .rxlength = request.rx_length * 8,
            };
            result = spi_device_transmit(request.device, &trans);
            gpio_set_level((gpio_num_t)request.cs_pin, 1);
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

esp_err_t spi_owner_init(spi_owner_t *owner,
                             spi_host_device_t host,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id)
{
    if (!owner) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(owner, 0, sizeof(*owner));
    owner->host = host;
    owner->request_queue = xQueueCreate(queue_len, sizeof(spi_owner_request_t));
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

    BaseType_t task_created = xTaskCreatePinnedToCore(spi_owner_task,
                                                      "spi_owner_task",
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

esp_err_t spi_owner_deinit(spi_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_owner_request_t shutdown_request;
    memset(&shutdown_request, 0, sizeof(shutdown_request));
    shutdown_request.shutdown = true;

    if (owner->request_queue) {
        xQueueSend(owner->request_queue, &shutdown_request, portMAX_DELAY);
    }

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

esp_err_t spi_owner_transfer(spi_owner_t *owner,
                                 spi_device_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 int cs_pin)
{
    if (!owner || !owner->initialized || !owner->request_queue || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    if (owner->wedged) {
        /* Fail fast -- see the .h comment on spi_owner_t::wedged. Do not
         * touch the queue or spin up a semaphore for a request the owner
         * task will never get to. */
        return ESP_ERR_INVALID_STATE;
    }

    /* Static, stack-resident semaphore -- same fix as uart_owner_transfer()
     * and i2c_owner_transfer() (2026-08-20): this is the LCD's SPI flush
     * path, the hottest transfer path on the whole board (every LVGL
     * redraw), so its per-call heap churn was a prime suspect alongside the
     * touch driver for the board-wide internal-SRAM starvation that was
     * breaking Wi-Fi AP client handshakes (deauth reason 1 right after
     * assoc). */
    StaticSemaphore_t done_sem_storage;
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinaryStatic(&done_sem_storage);
    if (!done_sem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    spi_owner_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = device;
    request.tx_buffer = tx_buffer;
    request.tx_length = tx_length;
    request.rx_buffer = rx_buffer;
    request.rx_length = rx_length;
    request.cs_pin = cs_pin;
    request.done_sem = done_sem;
    request.result_out = &result;

    if (xQueueSend(owner->request_queue, &request, pdMS_TO_TICKS(SPI_OWNER_TRANSFER_TIMEOUT_MS)) !=
        pdTRUE) {
        /* Never reached the owner task -- nothing has a reference to
         * done_sem, so it is safe to delete here. The queue itself being
         * unable to accept a request for a full second means the owner is
         * stuck on whatever it is currently processing; latch wedged so the
         * next caller (display or thermocouple, whichever comes first)
         * fails immediately instead of also blocking. */
        vSemaphoreDelete(done_sem);
        owner->wedged = true;
        ESP_LOGE(TAG, "request queue did not accept a transfer within %ums -- "
                      "owner task presumed wedged, failing all transfers until reset",
                 (unsigned)SPI_OWNER_TRANSFER_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(SPI_OWNER_TRANSFER_TIMEOUT_MS)) != pdTRUE) {
        /* The request WAS handed to the owner task, which is presumably
         * still blocked inside spi_device_transmit() on it (that is the
         * only blocking call between dequeue and xSemaphoreGive()). This
         * stack-resident done_sem is therefore not provably safe to delete
         * -- a wedged transfer that later, against expectation, completes
         * would give a semaphore backed by memory this function has already
         * returned from. Latching wedged makes that a one-time, already-
         * disclosed risk instead of an ongoing one: every future call fails
         * before touching the queue or a semaphore at all, so this is the
         * last transfer that can ever be in flight. A truly wedged SPI
         * transaction does not resolve on its own in practice; recovery is
         * a reset, same as any other watchdog trip in this firmware. */
        owner->wedged = true;
        ESP_LOGE(TAG, "owner task did not complete a transfer within %ums -- "
                      "presumed wedged, failing all transfers until reset",
                 (unsigned)SPI_OWNER_TRANSFER_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done_sem);
    return result;
}
