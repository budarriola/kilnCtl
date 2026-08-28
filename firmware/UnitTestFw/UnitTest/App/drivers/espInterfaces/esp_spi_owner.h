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
