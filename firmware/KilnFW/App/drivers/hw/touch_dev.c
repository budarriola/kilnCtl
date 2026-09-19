// touch_dev.c -- see touch_dev.h for the design writeup. Pure math only
// (stdint/stdbool), no ESP-IDF dependency beyond esp_err_t's typedef, so
// this compiles and is exercised identically on host and on target -- see
// App/test/test_touch_dev.c.
#include "touch_dev.h"

touch_cal_support_t touch_dev_cal_support(const touch_dev_t *dev)
{
    /* Presence FIRST -- see touch_dev.h. A zeroed touch_dev_t has
     * self_calibrating == false, so testing that flag first would report a
     * board with no working touch controller as a present resistive one. */
    if (!dev || !dev->read) {
        return TOUCH_CAL_SUPPORT_NO_TOUCH;
    }
    if (dev->self_calibrating) {
        return TOUCH_CAL_SUPPORT_SELF_CALIBRATING;
    }
    return TOUCH_CAL_SUPPORT_SUPPORTED;
}

const char *touch_cal_support_name(touch_cal_support_t support)
{
    switch (support) {
        case TOUCH_CAL_SUPPORT_SUPPORTED:        return "supported";
        case TOUCH_CAL_SUPPORT_SELF_CALIBRATING: return "self_calibrating";
        case TOUCH_CAL_SUPPORT_NO_TOUCH:         return "no_touch";
    }
    /* Unreachable for any declared value; returned rather than asserted so a
     * %s caller can never be handed NULL. */
    return "unknown";
}

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

void touch_dev_uncalibrated_max(bool self_calibrating, bool swap_xy, uint16_t width,
                                 uint16_t height, uint16_t ns2009_adc_max, uint16_t *out_raw_x_max,
                                 uint16_t *out_raw_y_max)
{
    if (!out_raw_x_max || !out_raw_y_max) return;

    if (!self_calibrating) {
        *out_raw_x_max = ns2009_adc_max;
        *out_raw_y_max = ns2009_adc_max;
        return;
    }

    /* self-calibrating: raw_x/raw_y are already panel coordinates, so each
     * max is that axis's OWN pre-swap panel extent -- not the screen extent
     * it happens to land on after swap_xy is applied downstream in
     * touch_dev_map_uncalibrated(). */
    *out_raw_x_max = (uint16_t)((swap_xy ? height : width) - 1u);
    *out_raw_y_max = (uint16_t)((swap_xy ? width : height) - 1u);
}
