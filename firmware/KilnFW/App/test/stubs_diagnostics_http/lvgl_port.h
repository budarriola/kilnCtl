// Private shim for lvgl_port.h, test_diagnostics_http.c only: diagnostics_http.c
// uses just lvgl_port_get_flush_stats_ex(); the real header needs lvgl.h.
// Placed ahead of the real drivers/ui path on this executable's /I list.
#ifndef LVGL_PORT_H
#define LVGL_PORT_H
#include <stdint.h>
void lvgl_port_get_flush_stats_ex(uint32_t *last_us, uint32_t *min_us, uint32_t *max_us,
                                  uint32_t *count, uint32_t *mean_us);
#endif
