#include "uart_owner.h"

#include <string.h>

#include "esp_log.h"

#include "hal_esp_common.h"

static const char *TAG = "uart_owner";

/* Event-queue depth for the driver's own event queue (FIFO_OVF/BUFFER_FULL/
 * line-error notifications), delegated to hal_uart_init() below. NOT the
 * same thing as the deleted request-queue worker's `queue_len` parameter
 * (kept in uart_owner_init's signature only for source compatibility -- see
 * uart_owner.h). Matches the value this file hardcoded before the 2026-09-06
 * uart-collapse delegation, and hal_uart_esp.c's own
 * HAL_UART_ESP_EVENT_QUEUE_LEN default (both 16) -- passed explicitly here
 * so this owner's behavior does not silently drift if that backend default
 * ever changes. */
#define UART_OWNER_EVENT_QUEUE_LEN 16

/* 2026-09-06 uart collapse (docs/HW_ABSTRACTION_PLAN.md "hal_uart_open()
 * still unused on ESP" item): uart_owner_init() used to duplicate
 * hal_uart_esp.c's uart_driver_install/uart_param_config/uart_set_pin
 * sequence byte-for-byte, including its own copy of the FIFO_OVF/
 * BUFFER_FULL/line-error event task (uart_owner_event_task(), deleted here --
 * see git history for its prior body, identical to hal_uart_esp.c's
 * hal_uart_esp_event_task()). uart_owner_init() now delegates the whole
 * driver-install-plus-event-task sequence to hal_uart_init() via an embedded
 * hal_uart_t (uart_owner.h), and every accessor below reads through that
 * handle (hal_uart_get_rx_error_count/hal_uart_restart/hal_uart_deinit)
 * instead of keeping a second, parallel copy of the same counters and task.
 * This was neither a full migration (uart_protocol.c/safety_link.c still
 * reach the port through uart_owner_t, not a bare hal_uart_t -- see
 * HW_ABSTRACTION_PLAN.md's "hal_uart_attach" item for why that stays true
 * until Phase 2/3) nor a permanent allowlist exempting this file from the
 * HAL: uart_owner_t is now a thin wrapper whose only job is to own the
 * fields callers already reach directly (port, event_task_handle) and to
 * forward everything else onto hal_uart_init/_get_rx_error_count/_restart/
 * _deinit. */

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

    /* queue_len is unused now that the request-queue worker task is gone
     * (see uart_owner.h's doc comment above the struct) -- kept as a
     * parameter for source compatibility with existing call sites. The
     * driver's own event queue depth is UART_OWNER_EVENT_QUEUE_LEN,
     * unrelated to this parameter, passed to hal_uart_init() below. */
    (void)queue_len;

    memset(owner, 0, sizeof(*owner));
    owner->port = port;

    hal_uart_cfg_t cfg = {
        .port = (int)port,
        .tx_io = tx_io,
        .rx_io = rx_io,
        .baud = (uint32_t)baud_rate,
        .queue_len = UART_OWNER_EVENT_QUEUE_LEN,
        .task_priority = (int)task_priority,
        .stack_depth = stack_depth,
        .core_id = (int)core_id,
    };

    hal_status_t st = hal_uart_init(&owner->hal, &cfg);
    if (st != HAL_OK) {
        /* hal_uart_init() (hal_uart_esp.c) already logs the specific
         * uart_driver_install/uart_param_config/uart_set_pin failure with
         * this port number and the real esp_err_to_name() string under its
         * own "hal_uart_esp" tag -- named here too so a log search for this
         * port under either tag finds the failure. */
        ESP_LOGE(TAG, "hal_uart_init(uart%d) failed: hal_status %d", (int)port, (int)st);
        return hal_status_to_esp_err(st);
    }

    owner->event_task_handle = (TaskHandle_t)hal_uart_get_task_handle(&owner->hal);
    owner->initialized = true;
    return ESP_OK;
}

esp_err_t uart_owner_deinit(uart_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    hal_status_t st = hal_uart_deinit(&owner->hal);

    owner->event_task_handle = NULL;
    owner->initialized = false;
    return hal_status_to_esp_err(st);
}

uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner)
{
    if (!owner) {
        return 0;
    }
    return hal_uart_get_rx_error_count(&owner->hal);
}

esp_err_t uart_owner_restart(uart_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    /* RX-only by contract, same as before -- hal_uart_restart() (see its
     * doc comment in interface/hal_uart.h) flushes RX and resets the rx
     * error counter, never touching TX-side state. */
    hal_status_t st = hal_uart_restart(&owner->hal);
    return hal_status_to_esp_err(st);
}
