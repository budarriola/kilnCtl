#ifndef GPIO_PROBE_H
#define GPIO_PROBE_H

#include "esp_err.h"
#include "../../../../hwAbstraction/esp/uart/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UART_TASK_ID_GPIO_PROBE bridge task -- see uart_task_ids.h for the wire
 * format. Compiled to a stub that always returns ESP_ERR_NOT_SUPPORTED (and
 * registers no task) unless CONFIG_KILNCTL_ENABLE_GPIO_PROBE is set, so
 * calling this unconditionally from main.c is safe on every build: the
 * capability either exists in full, deny-list and all, or does not exist at
 * all -- there is no partially-gated state. */
esp_err_t uart_bridge_start_gpio_probe_task(uart_protocol_t *proto);

#ifdef __cplusplus
}
#endif

#endif // GPIO_PROBE_H
