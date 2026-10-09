// config_store_flash_host_stubs.h -- host test doubles for the two
// non-hal_flash dependencies config_store_flash.c still has after its
// Phase 3 item 2 hal_flash rebase: relay_owner_get_state() (the ARMED gate
// input) and console_uart_puts()/console_uart_write() (the boot-time
// "record REJECTED" log line). Both real modules pull in pico-sdk/FreeRTOS
// surface config_store_flash.c's own host test has no need for -- this file
// defines just the two symbols config_store_flash.c calls, with a
// test-settable ARMED state and a captured log for assertions.
#ifndef SAFTYFW_TEST_CONFIG_STORE_FLASH_HOST_STUBS_H
#define SAFTYFW_TEST_CONFIG_STORE_FLASH_HOST_STUBS_H

#include <stddef.h>

#include "tasks/relay_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resets the ARMED state to RELAY_OWNER_STATE_INIT and clears the captured
// console log. Call between test cases.
void config_store_flash_host_stub_reset(void);

// Controls what relay_owner_get_state() (as seen by config_store_flash.c)
// returns.
void config_store_flash_host_stub_set_relay_state(relay_owner_state_t state);

// Concatenation of every console_uart_puts() call since the last reset, NUL
// terminated, truncated silently if it would overflow the internal buffer.
const char *config_store_flash_host_stub_console_log(void);

// Number of console_uart_puts() calls since the last reset.
size_t config_store_flash_host_stub_console_call_count(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TEST_CONFIG_STORE_FLASH_HOST_STUBS_H
