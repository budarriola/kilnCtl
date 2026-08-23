// console_sink.c -- see console_sink.h. A pico-sdk stdio_driver_t (same
// shape pico_stdio_uart's own stdio_uart.c uses) whose out_chars() callback
// forwards to usb_owner_console_write() (tasks/usb_owner.h) instead of
// touching any hardware peripheral directly -- this file owns no peripheral
// itself, it only fans an already-existing byte stream out to a second
// consumer.
#include "console_sink.h"

#include "pico/stdio.h"
#include "pico/stdio/driver.h"

#include "../tasks/usb_owner.h"

// Mirrors every byte pico-sdk's stdio_put_string() hands this driver --
// same fan-out point every OTHER registered stdio driver (currently just
// pico_stdio_uart) gets called from, so this needs no knowledge of what
// produced the bytes (printf(), puts(), simfw_fatal()'s vsnprintf()+printf()
// pair, ...). usb_owner_console_write() itself is the non-blocking,
// drop-if-nobody's-listening half of this contract (see that function's own
// doc comment in usb_owner.h) -- nothing here adds buffering or blocking on
// top of it.
static void console_sink_out_chars(const char *buf, int len)
{
    if (buf == NULL || len <= 0) {
        return;
    }
    (void)usb_owner_console_write((const uint8_t *)buf, (size_t)len);
}

// usb_owner_console_write() already flushes (tud_cdc_n_write_flush()) on
// every successful call -- console lines are short and this project has no
// reason to batch them the way a high-throughput link might, so there is
// nothing left for a separate flush hook to do.
static void console_sink_out_flush(void)
{
}

static stdio_driver_t console_sink_driver = {
    .out_chars = console_sink_out_chars,
    .out_flush = console_sink_out_flush,
    .in_chars = NULL, // console sink is output-only -- CDC1 RX is not read by anything
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

void console_sink_register(void)
{
    stdio_set_driver_enabled(&console_sink_driver, true);
}
