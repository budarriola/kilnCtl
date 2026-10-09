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

/* The REAL touch_dev.h, deliberately not shimmed: it is pure stdint/stdbool
 * math plus esp_err_t (its own header comment says so), host-compilable as
 * it stands, and exe9 links the real touch_dev.c. So the calibration-support
 * enum and touch_cal_support_name() the status handler emits are the
 * production ones here, not a transcription that could drift from them --
 * the wire spelling is asserted against real code. Only the lvgl_port entry
 * point below is faked, because THAT is what drags in lvgl.h/panel_spi.h. */
#include "touch_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_port_touch_is_calibrated(void);
touch_cal_support_t lvgl_port_touch_cal_support(void);

#ifdef __cplusplus
}
#endif

#endif
