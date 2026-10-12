// touch_cal_store.h -- persisted NS2009 raw-to-screen calibration.
//
// The Kconfig-level TOUCH_CAL_SWAP_XY/INVERT_X/INVERT_Y flags (lvgl_port.c)
// assume the raw ADC range maps linearly onto the full 0..NS2009_ADC_MAX
// span with no offset -- bench data (2026-08-19) showed that's not true on
// this panel: raw readings for known on-screen targets did not fit any
// swap/invert combination of that simple model. A full 2D affine fit
// (screen = A * raw, 6 coefficients, least-squares over N calibration
// points) handles swap, invert, scale, offset and skew all at once without
// needing to guess which raw axis maps to which screen axis.
//
// This module owns persisting that fit to NVS and applying it. It does NOT
// own collecting the calibration points -- that is ui_page_touch_cal.c's
// job (it has the on-screen target positions and drives the tap sequence).
#ifndef KILNCTL_TOUCH_CAL_STORE_H
#define KILNCTL_TOUCH_CAL_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// screen_x = a*raw_x + b*raw_y + c
// screen_y = d*raw_x + e*raw_y + f
typedef struct {
    bool  calibrated;
    float a, b, c;
    float d, e, f;
} touch_cal_t;

// Loads the persisted calibration from NVS. On any failure (never
// calibrated, corrupt record, size mismatch after a layout change) *out is
// set to {.calibrated = false, ...identity-ish...} and ESP_OK is still
// returned for a FRESH board ("not calibrated yet" is the expected steady
// state). A record that exists but is unreadable, wrong size/version, or
// holds non-finite/degenerate coefficients returns an error code (and logs)
// with *out uncalibrated (K10-06/08). Callers only need out->calibrated.
esp_err_t touch_cal_store_load(touch_cal_t *out);

// Convenience wrapper around touch_cal_store_load() for a caller that only
// needs the yes/no answer (kiln_ui.c's boot routing).
bool touch_cal_store_is_calibrated(void);

// Persists `cal` (which must have .calibrated = true -- this is how a
// completed calibration is recorded, there is no separate "erase" call
// since overwriting with a fresh fit is the only way this is ever redone).
esp_err_t touch_cal_store_save(const touch_cal_t *cal);

// Fits screen = A*raw by least squares over n >= 3 points and fills *out
// (leaving out->calibrated at whatever the caller set -- this is a pure
// math helper, not a persist call). Returns ESP_ERR_INVALID_ARG if n < 3 or
// the points are degenerate (collinear raw samples -- the 3x3 normal-
// equations matrix is singular).
esp_err_t touch_cal_fit(const uint16_t *raw_x, const uint16_t *raw_y, const int32_t *screen_x,
                         const int32_t *screen_y, size_t n, touch_cal_t *out);

// Applies `cal` to one raw sample, clamping the result to
// [0, width-1] x [0, height-1] -- an affine fit extrapolated slightly past
// its calibration points (a press right at a screen edge) must still land
// inside the display rather than handing LVGL an out-of-bounds point.
void touch_cal_apply(const touch_cal_t *cal, uint16_t raw_x, uint16_t raw_y, uint16_t width,
                      uint16_t height, int32_t *out_x, int32_t *out_y);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_TOUCH_CAL_STORE_H
