#ifndef I2C_OWNER_H
#define I2C_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t shutdown_done; /* given by the worker right before it exits */
    bool initialized;
    bool shutdown_requested;
} i2c_owner_t;

typedef struct {
    i2c_master_dev_handle_t device;
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    uint32_t timeout_ms;
    SemaphoreHandle_t done_sem;
    esp_err_t *result_out;
    bool shutdown;
} i2c_owner_request_t;

esp_err_t i2c_owner_init(i2c_owner_t *owner,
                             i2c_master_bus_handle_t bus,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id);
esp_err_t i2c_owner_deinit(i2c_owner_t *owner);
esp_err_t i2c_owner_transfer(i2c_owner_t *owner,
                                 i2c_master_dev_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // I2C_OWNER_H
