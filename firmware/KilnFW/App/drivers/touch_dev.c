// touch_dev.c -- see touch_dev.h for the design writeup. Pure math only
// (stdint/stdbool), no ESP-IDF dependency beyond esp_err_t's typedef, so
// this compiles and is exercised identically on host and on target -- see
// App/test/test_touch_dev.c.
#include "touch_dev.h"

int32_t touch_dev_axis_to_px(uint16_t raw, uint16_t raw_max, uint16_t panel_extent, bool invert)
{
    if (raw_max == 0 || panel_extent == 0) return 0;
    if (raw > raw_max) raw = raw_max;
    uint32_t px = ((uint32_t)raw * (uint32_t)(panel_extent - 1u)) / raw_max;
    if (invert) px = (uint32_t)(panel_extent - 1u) - px;
    return (int32_t)px;
}

void touch_dev_map_uncalibrated(uint16_t raw_x, uint16_t raw_y, uint16_t raw_x_max,
                                 uint16_t raw_y_max, uint16_t width, uint16_t height,
                                 bool swap_xy, bool invert_x, bool invert_y, int32_t *out_px,
                                 int32_t *out_py)
{
    if (!out_px || !out_py) return;

    uint16_t ax = swap_xy ? raw_y : raw_x;
    uint16_t ay = swap_xy ? raw_x : raw_y;
    uint16_t ax_max = swap_xy ? raw_y_max : raw_x_max;
    uint16_t ay_max = swap_xy ? raw_x_max : raw_y_max;

    *out_px = touch_dev_axis_to_px(ax, ax_max, width, invert_x);
    *out_py = touch_dev_axis_to_px(ay, ay_max, height, invert_y);
}
