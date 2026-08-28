#ifndef UART_LOG_BRIDGE_H
#define UART_LOG_BRIDGE_H

#include "esp_err.h"
#include "espInterfaces/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Redirects every ESP_LOGx call in the firmware onto the reliable UART link
 * (UART_TASK_ID_LOG, see uart_task_ids.h) instead of the USB-Serial-JTAG
 * console, so log output is visible to whatever's already connected over
 * uart_protocol (the GUI's Device Console, an MCP client, etc.) without a
 * second cable or an active debugger session.
 *
 * Call uart_log_bridge_early_init() as the very first thing in app_main,
 * before any other driver bring-up -- it installs the vprintf hook and
 * starts buffering log lines immediately, so even failures during e.g.
 * SSD1306_start() (which runs before the uart_protocol_t exists) are
 * captured and get flushed out once uart_log_bridge_start() runs.
 *
 * Call uart_log_bridge_start() once proto is initialized (after
 * uart_protocol_init()) to actually start draining the queue over the wire;
 * anything buffered before that point is sent first, oldest first. */
void uart_log_bridge_early_init(void);
esp_err_t uart_log_bridge_start(uart_protocol_t *proto);

#ifdef __cplusplus
}
#endif

#endif // UART_LOG_BRIDGE_H
