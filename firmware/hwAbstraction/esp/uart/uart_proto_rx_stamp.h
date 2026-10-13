#ifndef UART_PROTO_RX_STAMP_H
#define UART_PROTO_RX_STAMP_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Receive stamp (ms) for uart_proto_message_t::rx_ms: the tick count times the tick period, wrapping mod 2^32
 * like every other ESP ms stamp. Standalone so the host-test stub of uart_protocol.h shares the REAL helper
 * (REBOOTFX5 LOW-1): safety_link.c ages DIAG through it too, so the units formula exists exactly once. */
static inline uint32_t uart_proto_rx_stamp_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * (TickType_t)portTICK_PERIOD_MS);
}

#endif
