#ifndef UART_OWNER_H
#define UART_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t port;
    QueueHandle_t request_queue;
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    TaskHandle_t event_task_handle;
    SemaphoreHandle_t shutdown_done; /* given by uart_owner_task right before it exits */
    bool initialized;
    bool shutdown_requested;
    volatile uint32_t rx_error_count;
} uart_owner_t;

typedef struct {
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    size_t *rx_length_out;
    uint32_t timeout_ms;
    SemaphoreHandle_t done_sem;
    esp_err_t *result_out;
    bool shutdown;
} uart_owner_request_t;

esp_err_t uart_owner_init(uart_owner_t *owner,
                           uart_port_t port,
                           int tx_io,
                           int rx_io,
                           int baud_rate,
                           UBaseType_t queue_len,
                           UBaseType_t task_priority,
                           uint32_t stack_depth,
                           BaseType_t core_id);
esp_err_t uart_owner_deinit(uart_owner_t *owner);

/* Queues a transaction and blocks the calling task until it has been sent
 * (and, if rx_buffer/rx_length are set, a reply has been read) in FIFO order
 * relative to every other caller sharing this owner. */
esp_err_t uart_owner_transfer(uart_owner_t *owner,
                               const uint8_t *tx_buffer,
                               size_t tx_length,
                               uint8_t *rx_buffer,
                               size_t rx_length,
                               size_t *rx_length_out,
                               uint32_t timeout_ms);

/* Count of line errors (break/parity/frame) and RX/FIFO overflows observed
 * since init, each of which triggers an automatic input flush. Useful for
 * diagnosing a flaky physical connection. */
uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner);

/* On-demand version of the same RX flush the event task already does
 * automatically on a FIFO/ring-buffer overflow (see uart_owner.c) -- resets
 * rx_error_count too. Deliberately does not touch the TX side or the
 * request/event tasks themselves: safe to call while other transfers are
 * in flight, and safe to call from within a handler that's replying to the
 * very request that triggered it (see SYSTEM_CMD_RESTART_UART in
 * uart_task_ids.h). */
esp_err_t uart_owner_restart(uart_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif // UART_OWNER_H
