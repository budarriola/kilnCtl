#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include "AD9833.h"
#include "DcDac.h"
#include "PCF8575.h"
#include "SSD1306.h"
#include "esp_err.h"
#include "espInterfaces/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers UART_TASK_ID_DAC / UART_TASK_ID_AD9833 on proto and spawns a
 * task that turns incoming uart_protocol messages into DcDac_ and AD9833_
 * calls (see uart_task_ids.h for the payload formats). dac/gen must
 * outlive the bridge task (typically static/global, already initialized). */
esp_err_t uart_bridge_start_dac_task(uart_protocol_t *proto, DcDacClass *dac);
esp_err_t uart_bridge_start_ad9833_task(uart_protocol_t *proto, AD9833Class *gen);
esp_err_t uart_bridge_start_oled_task(uart_protocol_t *proto, SSD1306Class *oled);

/* Registers UART_TASK_ID_PCF8575 and spawns a task that turns incoming
 * messages into PCF8575_ calls. Unlike the DAC/AD9833/OLED bridges this one
 * is part query channel: READ_PORT and SCAN answer with their own DATA frame
 * back to the requester (see uart_task_ids.h), so the PC side must itself be
 * registered on this task_id to receive them. */
esp_err_t uart_bridge_start_pcf8575_task(uart_protocol_t *proto, PCF8575Class *expander);

/* Registers UART_TASK_ID_INFO and spawns a task that answers
 * INFO_CMD_GET_PIN_CONFIG queries with this firmware's actual pin usage
 * (pulled from settings.h at build time -- see uart_task_ids.h for the
 * response format). Doesn't need a device handle: it just reports config,
 * it doesn't own any hardware. */
esp_err_t uart_bridge_start_info_task(uart_protocol_t *proto);

/* Registers UART_TASK_ID_SYSTEM and spawns a task that handles admin-style
 * commands against the link itself (currently just RESTART_UART -- see
 * uart_task_ids.h). Needs the uart_owner_t directly (not just proto), since
 * that's where the actual RX-flush primitive lives. */
esp_err_t uart_bridge_start_system_task(uart_protocol_t *proto, uart_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif // UART_BRIDGE_H
