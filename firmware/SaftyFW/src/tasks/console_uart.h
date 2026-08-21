// console_uart.h -- Phase 2/8 bench console: UART0 on GP16/GP17, the debug
// probe's UART bridge (docs/HARDWARE.md section 7b, docs/ARCHITECTURE.md
// section 1's "RTT over SWD... Secondary" transport's UART-bridge sibling --
// this is the plain-UART half of that path, not RTT). Exists purely so a
// human with a serial terminal on the probe's COM port can see boot text and
// log lines with no PC connection to the safety domain other than the debug
// probe itself (already required for SWD flashing/halt).
//
// Deliberately NOT the same thing as SAFTYFW_ENABLE_USB_STDIO:
//   - GP16/GP17 are unconnected to anything else on this board (board_pins.h,
//     docs/HARDWARE.md section 2's "Unconnected" rows) -- no 3V3-back-feed
//     contention, no TinyUSB, no blocking-CDC risk. It costs one hardware
//     UART peripheral and two GPIOs, nothing else.
//   - It is therefore always compiled in, not gated behind a build option.
// uart_owner.c's UART1 remains the isolated link to the ESP and is
// untouched by this file -- different peripheral instance, different pins,
// no shared state.
#ifndef SAFTYFW_TASKS_CONSOLE_UART_H
#define SAFTYFW_TASKS_CONSOLE_UART_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up UART0 at 115200 8N1 on SAFTYFW_PIN_UART0_TX/RX (board_pins.h).
// Safe to call before the scheduler starts (main.c step 5-adjacent) -- no
// FreeRTOS primitives involved, just the hardware_uart peripheral, same as
// uart_owner_init()'s own early-bring-up shape.
bool console_uart_init(void);

// Best-effort, blocking-on-this-task-only write (uart_write_blocking()) --
// same tradeoff log_task.c's existing stdio_usb mirror already accepts: only
// ever called from log_task (lowest priority, always droppable) or before
// the scheduler starts, so a slow/absent terminal on the other end stalls
// nothing that matters. Never called from any task on SAFTYFW_CORE_TRIP_PATH.
void console_uart_write(const char *data, size_t len);

// Convenience: writes a NUL-terminated string, no length computation at the
// call site. Still goes through console_uart_write() underneath.
void console_uart_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_CONSOLE_UART_H
