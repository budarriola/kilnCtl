// console_uart.c -- see console_uart.h. UART0 on GP16/GP17, the debug
// probe's UART bridge. Deliberately a tiny, dependency-free module: no ring
// buffer, no IRQ, just uart_init()/uart_write_blocking(), because its whole
// job is to be trustworthy when everything else is suspect.
#include "console_uart.h"

#include <string.h>

#include "hardware/gpio.h" // permanent holdout: write-only diagnostic UART, no IRQ, kept dependency-free by design -- see firmware/hwAbstraction/README.md "Permanent holdouts"
#include "hardware/uart.h" // permanent holdout: same rationale as hardware/gpio.h above

#include "board_pins.h"

#define CONSOLE_UART_INSTANCE  uart0
#define CONSOLE_UART_BAUD_RATE 115200u

static bool s_ready = false;

bool console_uart_init(void)
{
    uart_init(CONSOLE_UART_INSTANCE, CONSOLE_UART_BAUD_RATE);
    gpio_set_function(SAFTYFW_PIN_UART0_TX, GPIO_FUNC_UART);
    gpio_set_function(SAFTYFW_PIN_UART0_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(CONSOLE_UART_INSTANCE, false, false);
    uart_set_format(CONSOLE_UART_INSTANCE, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(CONSOLE_UART_INSTANCE, true);
    // No inversion here -- this pair goes straight to the debug probe's plain
    // USB-serial bridge, not through any isolation barrier part, so standard
    // polarity is correct on both ends. (uart_owner's UART1 also applies no
    // inversion any more since the 2026-08-25 optocoupler-to-digital-isolator
    // rework, but for a different reason: U6 doesn't invert either.)
    s_ready = true;
    return true;
}

void console_uart_write(const char *data, size_t len)
{
    if (!s_ready || data == NULL || len == 0) {
        return;
    }
    uart_write_blocking(CONSOLE_UART_INSTANCE, (const uint8_t *)data, len);
}

void console_uart_puts(const char *s)
{
    if (s == NULL) {
        return;
    }
    console_uart_write(s, strlen(s));
}
