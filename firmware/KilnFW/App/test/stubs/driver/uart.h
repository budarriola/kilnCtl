// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests (reached via the real settings.h); nothing
// wifi_prov.c uses ever expands a UART-typed macro from settings.h, so this
// was empty until uart_log_bridge.c's host test (test_uart_log_bridge.c)
// pulled in the real espInterfaces/uart_owner.h, which names uart_port_t as
// a field type -- type-only stand-in, same convention as every other stub
// in this directory.
#ifndef TEST_STUB_DRIVER_UART_H
#define TEST_STUB_DRIVER_UART_H

typedef int uart_port_t;

/* Added for safety_link.c's host build -- value never asserted against. */
#define UART_NUM_1 1

#endif // TEST_STUB_DRIVER_UART_H
