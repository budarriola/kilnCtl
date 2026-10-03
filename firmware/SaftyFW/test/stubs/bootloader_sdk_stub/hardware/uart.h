// Host stub of hardware/uart.h: TX is captured by the test, RX is never fed
// (the test drives recovery_dispatch() directly).
#ifndef BOOTLOADER_SDK_STUB_UART_H
#define BOOTLOADER_SDK_STUB_UART_H
#include <stdbool.h>
typedef struct uart_inst uart_inst_t;
#define uart1 ((uart_inst_t *)0)
void uart_putc_raw(uart_inst_t *uart, char c);
bool uart_is_readable(uart_inst_t *uart);
char uart_getc(uart_inst_t *uart);
#endif
