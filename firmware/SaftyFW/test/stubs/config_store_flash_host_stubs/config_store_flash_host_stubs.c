// config_store_flash_host_stubs.c -- see config_store_flash_host_stubs.h.
#include "config_store_flash_host_stubs.h"

#include <string.h>

#include "tasks/console_uart.h"

static relay_owner_state_t s_state = RELAY_OWNER_STATE_INIT;

#define CONSOLE_LOG_CAP 1024u
static char   s_console_log[CONSOLE_LOG_CAP];
static size_t s_console_log_len = 0;
static size_t s_console_call_count = 0;

void config_store_flash_host_stub_reset(void)
{
    s_state = RELAY_OWNER_STATE_INIT;
    memset(s_console_log, 0, sizeof(s_console_log));
    s_console_log_len = 0;
    s_console_call_count = 0;
}

void config_store_flash_host_stub_set_relay_state(relay_owner_state_t state)
{
    s_state = state;
}

const char *config_store_flash_host_stub_console_log(void)
{
    return s_console_log;
}

size_t config_store_flash_host_stub_console_call_count(void)
{
    return s_console_call_count;
}

// The symbol config_store_flash.c actually calls for its ARMED check.
relay_owner_state_t relay_owner_get_state(void)
{
    return s_state;
}

// console_uart_write()/console_uart_puts() -- config_store_flash.c only
// calls console_uart_puts(), but console_uart.h declares both; defining
// both here (rather than linking the real console_uart.c, which needs
// pico-sdk's hardware/gpio.h + hardware/uart.h) keeps this stub
// self-contained.
void console_uart_write(const char *data, size_t len)
{
    (void)data;
    (void)len;
}

void console_uart_puts(const char *s)
{
    if (s == NULL) {
        return;
    }
    s_console_call_count++;
    size_t remaining = (s_console_log_len < CONSOLE_LOG_CAP - 1u)
                            ? (CONSOLE_LOG_CAP - 1u - s_console_log_len)
                            : 0u;
    if (remaining == 0u) {
        return;
    }
    size_t n = strlen(s);
    if (n > remaining) {
        n = remaining;
    }
    memcpy(s_console_log + s_console_log_len, s, n);
    s_console_log_len += n;
    s_console_log[s_console_log_len] = '\0';
}
