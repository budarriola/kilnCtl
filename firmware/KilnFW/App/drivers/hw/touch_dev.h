// touch_dev.h -- controller-agnostic touch abstraction.
//
// DISPLAY_ST7796_PLAN.md section 7 (Phase 5): lvgl_port.c used to type its
// touch device concretely as `NS2009Class *` and call NS2009_read()
// directly. That stopped working the moment a second controller entered the
// picture (FT6336U, on the not-yet-connected MSP4031 module) -- the two
// parts disagree on the single most important property a caller needs to
// know before deciding how to turn a raw sample into a screen pixel:
//
//   - NS2009 (this board's real hardware, BIGTREETECH TFT35-SPI J2) is a
//     4-wire resistive controller. It reports raw ADC counts with no fixed
//     relationship to panel pixels -- every board needs its own affine fit
//     (touch_cal_store.h) before those counts mean anything as a screen
//     coordinate.
//   - FT6336U (ft6336u.h, MSP4031 module, NOT populated on this board) is a
//     capacitive controller with its own on-chip touch processor. It reports
//     coordinates already in panel space -- running touch_cal_store's
//     per-board affine fit on top of that would double-apply a correction
//     the controller already did internally, and worse, it would drag a
//     capacitive board through the resistive controller's forced first-boot
//     calibration flow, which a capacitive controller can never complete via
//     that grid (see kiln_ui.c's touch_cal boot gate).
//
// `self_calibrating` is the single flag that decides which of those two
// paths a controller's raw sample takes. It does NOT mean "no correction is
// ever needed" -- an FT6336U-driven panel still needs the module's mounting
// rotation/axis-swap corrected (the same TOUCH_CAL_SWAP_XY/INVERT_X/INVERT_Y
// Kconfig knobs NS2009's own uncalibrated bootstrap fallback already uses,
// see touch_dev_map_uncalibrated() below) -- it means the per-board affine
// fit in touch_cal_store.h must never run on its output.
#ifndef TOUCH_DEV_H
#define TOUCH_DEV_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One blocking read: same shape as NS2009_read()/FT6336U_read() so either
 * driver's real function can be pointed to directly through a thin
 * (ctx, ...)-shaped adapter -- see lvgl_port.c's ns2009_touch_dev_read().
 * `out_pressed`/`out_x`/`out_y` are required; `out_z1` may be NULL exactly
 * like NS2009_read's (a capacitive controller has no pressure channel to
 * report -- see TOUCH_DEV_NO_PRESSURE_SENTINEL below for what a
 * self-calibrating device's read function should write there instead of a
 * fabricated number). */
typedef esp_err_t (*touch_dev_read_fn)(void *ctx, bool *out_pressed, uint16_t *out_x,
                                        uint16_t *out_y, uint16_t *out_z1);

typedef struct {
    void *ctx;
    touch_dev_read_fn read;

    /* true: raw_x/raw_y from `read` are already panel coordinates. Bypass
     * touch_cal_store.h's affine fit entirely; only the rotation/axis-swap
     * mapping (touch_dev_map_uncalibrated) applies.
     * false (NS2009, every board that exists today): raw_x/raw_y are ADC
     * counts needing touch_cal_store's per-board fit once calibrated, and
     * the same rotation/axis-swap mapping as a known-inaccurate bootstrap
     * guess before that fit exists. */
    bool self_calibrating;

    /* Rotation/axis-swap mapping for THIS controller (2026-09-04,
     * DISPLAY_ST7796_PLAN.md section 7) -- deliberately a property of the
     * CONTROLLER, not of whichever display panel driver happens to be
     * selected: panel pixel rendering (panel_desc_t, panel_codec.h) and the
     * physical touch chip are independent choices on this board (the
     * ILI9488 driver can be selected for pixels while the MSP4031's real
     * FT6336U is still the only touch chip actually wired up), and this
     * field used to live on panel_desc_t until that exact scenario proved
     * it wrong -- reverting the panel selection back to ILI9488 while
     * FT6336U stayed physically attached would have silently pulled in the
     * NS2009-tuned mapping for a controller that was never NS2009. The
     * caller building this struct (main.c) sets these from the matching
     * per-controller Kconfig bench knobs (TOUCH_CAL_* for NS2009,
     * TOUCH_CAP_* for FT6336U, settings.h). */
    bool swap_xy;
    bool invert_x;
    bool invert_y;
} touch_dev_t;

/* A capacitive controller (FT6336U) has no analog pressure channel -- Z1 is
 * an NS2009-specific ADC reading. Writing a value in the plausible 0..4095
 * NS2009 range here would be a fabricated number pretending to be real
 * sensor data (this repo's documented consumer-without-producer bug class,
 * ROADMAP.md / TODO.md). Out of range for a 12-bit ADC on purpose, so
 * anything that inspects a self-calibrating device's raw touch state (or a
 * host test) can tell "no pressure channel" apart from "a real reading of
 * zero" at a glance. */
#define TOUCH_DEV_NO_PRESSURE_SENTINEL 0xFFFFu

static inline esp_err_t touch_dev_read(const touch_dev_t *dev, bool *out_pressed,
                                        uint16_t *out_x, uint16_t *out_y, uint16_t *out_z1)
{
    if (!dev || !dev->read) return ESP_ERR_INVALID_ARG;
    return dev->read(dev->ctx, out_pressed, out_x, out_y, out_z1);
}

/* The self_calibrating/calibrated branch selection touch_read_cb()
 * (lvgl_port.c) runs on every physical touch poll, pulled out into a pure,
 * host-testable decision: touch_cal_store's per-board affine fit applies
 * only for a NON-self-calibrating device that HAS a completed fit loaded.
 * Both conditions matter independently -- a self-calibrating device must
 * never take this branch even if touch_cal_store happens to hold a stale
 * fit from a previously-attached different controller (NVS persists across
 * a controller swap; nothing erases it), and a non-self-calibrating device
 * still needs the uncalibrated fallback until its own fit completes. */
static inline bool touch_dev_use_calibrated_fit(const touch_dev_t *dev, bool touch_cal_calibrated)
{
    if (!dev) return false;
    return !dev->self_calibrating && touch_cal_calibrated;
}

/* Whether a USER-RUN calibration step (the 3x3 target grid in
 * ui_page_touch_cal.c, fitted and persisted through touch_cal_store.h) is a
 * meaningful thing to OFFER for the controller actually wired in right now.
 *
 * THE ONE SHARED PREDICATE. Every surface that offers, hides, or refuses
 * calibration answers this question through touch_dev_cal_support() below --
 * the LCD config hub's nav cell (ui_page_config.c), kiln_ui.c's forced
 * first-boot gate, ui_page_touch_cal.c's own build(), and the web UI (via
 * /api/status's touch_cal_supported field). They used to ask three subtly
 * different questions instead (`!self_calibrating`; `!self_calibrating &&
 * !touch_cal_store_is_calibrated()`; and nothing at all on the web side),
 * which is this repo's documented drift class: independent conditions that
 * agree today and diverge silently the moment one of them is edited.
 *
 * Deliberately a THREE-state answer, not a bool. "No touch controller came
 * up" is a genuinely different state from "this controller self-calibrates",
 * and collapsing them into one `false` is exactly how an unknown gets
 * silently reported as a confident "not supported" -- see each value's note. */
typedef enum {
    /* A resistive-style controller reporting raw ADC counts IS present.
     * Calibration is both meaningful and performable: offer it. */
    TOUCH_CAL_SUPPORT_SUPPORTED = 0,

    /* A self-calibrating controller (FT6336U) is present. It reports panel
     * coordinates directly and never populates touch_cal_store, so the grid
     * could never complete and the fit would never be read back. A CONFIDENT
     * "not supported": the hardware is known, and known not to need it. */
    TOUCH_CAL_SUPPORT_SELF_CALIBRATING,

    /* No touch controller is wired in at all -- a zeroed touch_dev_t: either
     * bring-up failed (main_boot_early.c logs a WARN and leaves the struct
     * zeroed) or the board has no touch hardware. NOT the same claim as
     * SELF_CALIBRATING above: the part that WOULD be there may well be one
     * calibration is meaningful for; bring-up simply got no answer from it
     * this boot.
     *
     * The option is still hidden, because the calibration flow needs the
     * operator to physically tap nine targets and there is nothing to read
     * those taps with -- a grid that cannot register a single press is a
     * worse lie than hiding it. But every surface reports THIS value
     * distinctly ("touch controller not detected"), never as the plain
     * "self-calibrating, none needed" message, so an unreported bring-up
     * failure can never masquerade as a panel that simply does not need
     * calibrating. Unknown is shown as unknown. */
    TOUCH_CAL_SUPPORT_NO_TOUCH,
} touch_cal_support_t;

/* The predicate itself. Pure (reads only the struct handed to it), so it is
 * host-testable -- which matters beyond tidiness here: HTTP handlers in this
 * tree are target-build-only (only a handful of 50 link into the host
 * suite), so a test written against the /api/status handler would never
 * actually run. Keeping the decision in this file, which IS in the host
 * build (build_host_tests.ps1's touch_dev.c entry), is what makes it
 * coverable at all. See test_touch_dev.c.
 *
 * `dev` NULL, or a dev with no read function, is NO_TOUCH -- a zeroed
 * touch_dev_t is precisely how main_boot_early.c represents "nothing came
 * up", and lvgl_port.c's touch_dev field comment says the same. Note the
 * ordering that matters: presence is checked BEFORE self_calibrating,
 * because a zeroed struct has self_calibrating == false and would otherwise
 * read as a present resistive controller -- i.e. a board whose capacitive
 * bring-up failed would be told calibration is SUPPORTED and sent into a
 * grid it cannot complete. Not hypothetical: that is the exact shape of the
 * pre-existing bug this predicate replaces, where every surface asked
 * `!self_calibrating` and a failed FT6336U bring-up answered "resistive". */
touch_cal_support_t touch_dev_cal_support(const touch_dev_t *dev);

/* The single "should this surface show the option?" test, so no caller has
 * to re-derive it by comparing against the enum and risk picking a different
 * set of values than its sibling surfaces did. */
static inline bool touch_cal_support_is_offerable(touch_cal_support_t support)
{
    return support == TOUCH_CAL_SUPPORT_SUPPORTED;
}

/* Stable machine-readable spelling for the wire (/api/status's
 * touch_cal_supported) and for logs: "supported", "self_calibrating",
 * "no_touch". Never NULL, even for an out-of-range value ("unknown"), so a
 * caller can hand this straight to a %s. */
const char *touch_cal_support_name(touch_cal_support_t support);

/* Pure raw-count -> panel-pixel scaling for ONE axis: clamp raw to
 * [0, raw_max], scale linearly onto [0, panel_extent-1], then optionally
 * invert. Same formula lvgl_port.c's old touch_raw_to_px() used for NS2009
 * (raw_max = NS2009_ADC_MAX); passing raw_max = panel_extent-1 makes this an
 * identity-plus-invert-plus-clamp, which is exactly what an already-panel-
 * space FT6336U sample needs. panel_extent must be >= 1; a caller with a
 * zero-sized display has bigger problems than this function catching it. */
int32_t touch_dev_axis_to_px(uint16_t raw, uint16_t raw_max, uint16_t panel_extent, bool invert);

/* The rotation/axis-swap mapping shared by BOTH: NS2009's uncalibrated
 * bootstrap fallback (before touch_cal_store has a fit) and every
 * self-calibrating controller's only mapping (it never gets a fit). Not
 * "the calibrated path" -- callers with a real touch_cal_t fit call
 * touch_cal_apply() instead and never reach this function at all.
 * raw_x_max/raw_y_max are each axis's own pre-swap raw ceiling (NS2009_ADC_MAX
 * for the resistive fallback; width-1/height-1 for an already-panel-space
 * capacitive sample) -- swap_xy decides which physical raw axis lands on
 * which screen axis BEFORE either axis's max or invert is applied, matching
 * how a controller's own X/Y wiring, not its value range, determines swap. */
void touch_dev_map_uncalibrated(uint16_t raw_x, uint16_t raw_y, uint16_t raw_x_max,
                                 uint16_t raw_y_max, uint16_t width, uint16_t height,
                                 bool swap_xy, bool invert_x, bool invert_y, int32_t *out_px,
                                 int32_t *out_py);

/* The raw_x_max/raw_y_max the touch_read_cb() caller (lvgl_port.c) must pass
 * into touch_dev_map_uncalibrated() above -- pulled out into its own pure,
 * host-testable function because getting this assignment wrong is exactly
 * the bug an opus review caught: touch_dev_map_uncalibrated's raw_x_max/
 * raw_y_max are each axis's PRE-swap raw ceiling (it swaps the maxes right
 * along with the axes when swap_xy is set), so a caller that assigns
 * POST-swap screen extents (raw_x_max = width-1 unconditionally) is wrong
 * the moment swap_xy is set -- every self-calibrating sample above the
 * un-swapped axis's extent would clamp, and everything below it would be
 * stretched to fill the wrong-sized range. Not self_calibrating: both
 * outputs are ns2009_adc_max regardless of swap_xy (matching
 * touch_dev_map_uncalibrated's NS2009 bootstrap-fallback caller, which
 * always passes symmetric maxes). */
void touch_dev_uncalibrated_max(bool self_calibrating, bool swap_xy, uint16_t width,
                                 uint16_t height, uint16_t ns2009_adc_max, uint16_t *out_raw_x_max,
                                 uint16_t *out_raw_y_max);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_DEV_H
