#ifndef UART_OWNER_H
#define UART_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t port;
    /* Driver-install (uart_driver_install/uart_param_config/uart_set_pin)
     * plus the event-queue task that watches for FIFO_OVF/BUFFER_FULL/line
     * errors both now live in this embedded hal_uart_t, delegated to via
     * hal_uart_init() -- see uart_owner.c's header comment. `event_queue` is
     * gone (it was never touched outside uart_owner.c); `port` and
     * `event_task_handle` stay direct fields because callers outside this
     * file read them (uart_protocol.c's proto->owner->port,
     * safety_link.c's stack-margin registration on
     * &link->owner.event_task_handle). */
    hal_uart_t hal;
    TaskHandle_t event_task_handle;
    bool initialized;
} uart_owner_t;

/* uart_owner_request_t and the request-queue/worker-task pair
 * (uart_owner_task()) it fed were deleted 2026-09-06 (uart collapse,
 * docs/HW_ABSTRACTION.md): uart_owner_transfer() was their only
 * caller, and a 2026-09-05 grep-confirmed audit (see uart_owner.c's prior
 * header comment on this, reproduced in git history) already found
 * uart_owner_transfer() had zero real callers left in KilnFW -- every path
 * that used to reach it goes through uart_protocol.c's hal_uart_send_
 * blocking() instead, on this owner's own `hal` handle directly (hal_uart_
 * attach() deleted, docs/HW_ABSTRACTION.md). `queue_len` is kept in
 * uart_owner_init's
 * signature for source compatibility with existing call sites but is no
 * longer used for anything. */

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

/* Count of line errors (break/parity/frame) and RX/FIFO overflows observed
 * since init, each of which triggers an automatic input flush. Useful for
 * diagnosing a flaky physical connection. */
uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner);

/* On-demand version of the same RX flush the event task already does
 * automatically on a FIFO/ring-buffer overflow (see uart_owner.c) -- resets
 * rx_error_count too. Deliberately does not touch the TX side or the
 * event task itself: safe to call while other transfers are in flight, and
 * safe to call from within a handler that's replying to the very request
 * that triggered it (see SYSTEM_CMD_RESTART_UART in uart_task_ids.h). */
esp_err_t uart_owner_restart(uart_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif // UART_OWNER_H
