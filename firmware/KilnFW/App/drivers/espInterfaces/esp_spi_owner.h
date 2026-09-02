#ifndef SPI_OWNER_H
#define SPI_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t shutdown_done; /* given by the worker right before it exits */
    bool initialized;
    bool shutdown_requested;
    /* Latched by spi_owner_transfer() the first time it gives up waiting on
     * the owner task (DISPLAY_ST7796_PLAN.md 9.9 / TODO.md: unbounded
     * portMAX_DELAY there defeated every caller-side timeout above it).
     * Once set, every subsequent transfer -- display AND thermocouple alike,
     * both routed through this one owner -- fails fast with
     * ESP_ERR_INVALID_STATE instead of queuing more work behind a task that
     * is presumed stuck inside a blocking spi_device_transmit(). This is
     * deliberately permanent for the life of the owner: a genuinely wedged
     * SPI transaction is not something a later caller can un-wedge, and
     * letting new requests pile up on a stuck queue is exactly the failure
     * mode being removed. Recovery is a reset, same as any other watchdog
     * trip in this firmware. */
    volatile bool wedged;
} spi_owner_t;

typedef struct {
    spi_device_handle_t device;
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    int cs_pin;
    SemaphoreHandle_t done_sem;
    esp_err_t *result_out;
    bool shutdown;
} spi_owner_request_t;

esp_err_t spi_owner_init(spi_owner_t *owner,
                             spi_host_device_t host,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id);
esp_err_t spi_owner_deinit(spi_owner_t *owner);
esp_err_t spi_owner_transfer(spi_owner_t *owner,
                                 spi_device_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 int cs_pin);

#ifdef __cplusplus
}
#endif

#endif // SPI_OWNER_H
