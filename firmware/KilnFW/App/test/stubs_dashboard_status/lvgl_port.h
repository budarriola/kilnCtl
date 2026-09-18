// Private, exe9-only shim for lvgl_port.h: dashboard_status_http.c #includes
// the real lvgl_port.h only to reach lvgl_port_touch_is_calibrated() -- the
// real header also pulls in lvgl.h/panel_spi.h/screen_idle.h/touch_dev.h,
// none of which are host-compilable (dashboard_http_internal.h's own header
// comment documents this for dashboard_http.c; the same is true transitively
// here). This directory is placed ahead of the real drivers/ui include path
// on this ONE executable's command line (build_host_tests.ps1's exe9 block)
// so only this translation unit sees the shim -- every other test executable
// still sees the real header if it ever needs it.
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_port_touch_is_calibrated(void);

#ifdef __cplusplus
}
#endif

#endif
