// console_sink.h -- registers a pico-sdk stdio_driver_t that mirrors every
// byte already going out over UART0 (pico_stdio_uart, CMakeLists.txt) to
// usb_owner's CDC1 console sink as well (2026-08-23, "give SimFW a second
// USB CDC interface, dedicated to console/log output" pass).
//
// WHY A SECOND STDIO DRIVER, NOT A NEW PRINT CALL SITE PER LINE. main.c's
// boot trace and drivers/simfw_fatal.c's fatal message are both plain
// printf()/fflush(stdout) calls today, going out over whichever stdio
// backends are registered (currently just pico_stdio_uart). pico-sdk's
// stdio layer already fans one printf() out to every ENABLED driver
// (stdio_put_string() in pico-sdk's stdio.c walks the driver list) --
// registering a second driver here means every existing and future
// printf()/puts() call site gets mirrored to CDC1 automatically, with zero
// changes at any of those call sites and no risk of one of them being
// missed. The alternative -- adding a second, explicit "also send this to
// USB" call next to every printf() -- does not scale and is exactly the
// kind of thing a future call site would forget.
//
// This file does NOT include tusb.h and is NOT one of the single-owner
// files for the USB peripheral (tools/check_single_owner.ps1's allowlist:
// tasks/usb_owner.c, tasks/usb_descriptors.c). It calls only
// usb_owner_console_write() (tasks/usb_owner.h's public API) -- the same
// "route through the owner's own header" doctrine every other task in this
// project follows for a peripheral it does not itself own.
#ifndef SIMFW_DRIVERS_CONSOLE_SINK_H
#define SIMFW_DRIVERS_CONSOLE_SINK_H

#ifdef __cplusplus
extern "C" {
#endif

// Registers the CDC1 stdio driver. Call once, from main() -- after
// stdio_init_all() (so the UART driver is already the first one registered;
// registration order does not change fan-out behavior, but this keeps
// main.c's own "stdio_init_all() is the very first call, everything below
// it has a live UART to write to" trace-provenance comment true without
// adding a second exception to it) and before usb_owner_start() is strictly
// unnecessary but harmless either way -- usb_owner_console_write() itself
// tolerates being called before usb_owner's TinyUSB init has completed
// (returns false, drops the byte, see usb_owner.h).
void console_sink_register(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_CONSOLE_SINK_H
