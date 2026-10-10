// Private shim for test_dashboard_http_relay.c: the exe9 shim plus the one
// extra entry point dashboard_get_status() calls (the real header cannot be
// compiled on the host, see ../stubs_dashboard_status/lvgl_port.h).
#ifndef DASHBOARD_HTTP_RELAY_LVGL_PORT_H
#define DASHBOARD_HTTP_RELAY_LVGL_PORT_H
#include <stdint.h>
#include "../stubs_dashboard_status/lvgl_port.h"
void lvgl_port_get_flush_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *count);
#endif
