#pragma once

/* ui_theme.h -- shared LVGL palette/spacing constants, TODO.md section 10.2
 * ("Visual style -- match KlipperScreen").
 *
 * PROVENANCE, READ THIS FIRST: nobody on this pass has the real KlipperScreen
 * theme files or the physical LCD in hand -- these hex values are a first-pass
 * palette matching a *description* of KlipperScreen's look (dark navy
 * background, white text, a small rotating set of saturated accent colors
 * used per-row/per-zone rather than one brand color), not colorpicked from an
 * actual screenshot or measured on the ILI9488 panel. Treat every color below
 * the same way this codebase already treats `KILNCTL_TOUCH_CAL_SWAP_XY` and
 * `KILNCTL_TOUCH_Z1_MAX_THRESHOLD` (see App/drivers/Kconfig): a reasonable
 * guess, not a measured value, and due for a sanity check against real
 * hardware once a panel is available to look at (colors read differently on
 * a small TFT under bench lighting than they do in a mental model of a
 * screenshot). 10.6 (web dashboard restyle) is meant to pull these exact hex
 * values into a shared CSS `:root` block rather than picking its own -- see
 * firmware/KilnFW/docs/UI_THEME.md.
 *
 * Nothing in the codebase consumes this header yet -- 10.3 (the real
 * dashboard/settings pages) is what will eventually pick per-zone accents
 * from the UI_THEME_ACCENT_* set below. kiln_ui.c's current placeholder
 * screen (build_home_page(), lv_color_black()) is deliberately left alone:
 * it's the pre-10.2 bring-up screen, not the real theme, and 10.3 is what
 * replaces it.
 */

#include <stdbool.h>

#include "lvgl.h"

/* ---- Backgrounds -------------------------------------------------------
 * Dark navy, not pure black -- KlipperScreen's reference screenshots showed
 * a background with a visible blue-gray cast rather than #000000. Kept
 * distinct from lvgl_port.c's current lv_color_black() placeholder screen,
 * which is intentionally plain black and unrelated to this theme.
 */

/* Main screen background: near-black navy, ~#1a1f2b per the reference look. */
#define UI_THEME_COLOR_BG_HEX      0x1a1f2b
#define UI_THEME_COLOR_BG          lv_color_hex(UI_THEME_COLOR_BG_HEX)

/* Card/panel background: a step lighter than the main background, used to
 * visually group content (stat rows, button clusters) without a hard border.
 * Guessed as roughly two shades up from UI_THEME_COLOR_BG. */
#define UI_THEME_COLOR_CARD_HEX    0x242a3a
#define UI_THEME_COLOR_CARD        lv_color_hex(UI_THEME_COLOR_CARD_HEX)

/* ---- Text --------------------------------------------------------------
 */

/* Primary text: near-white, not pure #ffffff, to avoid harsh contrast
 * against the dark background on a small TFT. */
#define UI_THEME_COLOR_TEXT_PRIMARY_HEX    0xf0f0f0
#define UI_THEME_COLOR_TEXT_PRIMARY        lv_color_hex(UI_THEME_COLOR_TEXT_PRIMARY_HEX)

/* Secondary/dim text: labels, units, less-important status text. */
#define UI_THEME_COLOR_TEXT_SECONDARY_HEX  0x9aa0ae
#define UI_THEME_COLOR_TEXT_SECONDARY      lv_color_hex(UI_THEME_COLOR_TEXT_SECONDARY_HEX)

/* ---- Accent colors -------------------------------------------------------
 * A small rotating palette used to color-code rows/zones/nav icons, the way
 * KlipperScreen's reference screenshots used a different accent bar color
 * per stat row (orange for one channel, purple/magenta for another, teal for
 * another, green for a nav icon underline). Named generically (ACCENT_1..5)
 * rather than by zone name on purpose -- this header knows nothing about how
 * many zones the kiln has configured; 10.3 is expected to assign one accent
 * per configured zone/metric in rotation, e.g. `UI_THEME_ACCENT_1` for zone
 * 0, `UI_THEME_ACCENT_2` for zone 1, wrapping around if there are more zones
 * than accents.
 */

/* Accent 1: orange. */
#define UI_THEME_ACCENT_1_HEX   0xe8974e
#define UI_THEME_ACCENT_1       lv_color_hex(UI_THEME_ACCENT_1_HEX)

/* Accent 2: purple/magenta. */
#define UI_THEME_ACCENT_2_HEX   0xa15fd6
#define UI_THEME_ACCENT_2       lv_color_hex(UI_THEME_ACCENT_2_HEX)

/* Accent 3: teal/cyan. */
#define UI_THEME_ACCENT_3_HEX   0x3ec6c6
#define UI_THEME_ACCENT_3       lv_color_hex(UI_THEME_ACCENT_3_HEX)

/* Accent 4: green (matches the reference's nav-icon underline color). */
#define UI_THEME_ACCENT_4_HEX   0x5cc06e
#define UI_THEME_ACCENT_4       lv_color_hex(UI_THEME_ACCENT_4_HEX)

/* Accent 5: red/amber, held back for an "attention"/alarm use rather than a
 * fifth ordinary zone color, since every other accent above is a calm color
 * and the UI needs at least one that reads as "look at this" (fault, alarm,
 * stop button) without introducing a whole separate semantic-color set. Not
 * seen directly in the reference screenshots described for this pass --
 * flagged as the one accent here that's inferred rather than observed, so
 * double-check it particularly hard once real hardware is available. */
#define UI_THEME_ACCENT_5_HEX   0xd6555f
#define UI_THEME_ACCENT_5       lv_color_hex(UI_THEME_ACCENT_5_HEX)

/* ---- Spacing / sizing ----------------------------------------------------
 * Budgeted against the 480x320 landscape panel (DISPLAY_WIDTH/HEIGHT,
 * App/drivers/settings.h) -- these are round numbers chosen to fit a small
 * grid of large touch targets on that resolution, not measured against a
 * finger or a real layout mockup.
 */

/* Minimum touch target edge length, in pixels. 72px is roughly one sixth of
 * the 480px width -- big enough to comfortably tap on a 3.5-4" panel without
 * a stylus, small enough that a 480x320 screen still fits a small grid (e.g.
 * 3-4 columns) of them plus a status bar. Round number, not derived from a
 * measured fingertip contact area -- sanity-check on real hardware. */
#define UI_THEME_MIN_TOUCH_TARGET_PX   72

/* Standard corner radius for buttons/cards, in pixels. Large enough to read
 * as "rounded-rect" at this scale rather than "square with clipped corners". */
#define UI_THEME_CORNER_RADIUS_PX      10

/* Standard padding/gap between grouped elements (grid cells, card interior
 * padding), in pixels. */
#define UI_THEME_PADDING_PX            8

/* Persistent top status bar height, in pixels. Sized to comfortably hold a
 * status icon row without eating too much of the 320px height budget the
 * rest of the page (10.3's content) needs. */
#define UI_THEME_STATUS_BAR_HEIGHT_PX  32

/* ---- Touch hit-area sizing -- TODO.md 10.4 ("Touch hit-testing") ---------
 *
 * TODO.md 10.4 asked for "nearest widget-center wins, within a dynamic
 * offset based on local density/button size." Before building that from
 * scratch, this pass actually read LVGL v9.5.0's real hit-testing code
 * (components/lvgl/src/indev/lv_indev.c:lv_indev_search_obj() +
 * components/lvgl/src/core/lv_obj_pos.c:lv_obj_hit_test()/
 * lv_obj_get_click_area()) instead of assuming 10.1's evaluation note was
 * right. It is NOT nearest-center arbitration: lv_indev_search_obj() walks
 * the widget tree depth-first, children checked topmost-z-order-first
 * (highest index first, since later siblings draw on top), and returns the
 * *first* object whose (possibly click-area-expanded) bounding box contains
 * the point -- plain rectangle containment via lv_area_is_point_on(), no
 * distance-to-center comparison anywhere, no arbitration between two
 * overlapping candidate boxes. Whichever object is tested first in z-order
 * and contains the point wins, full stop.
 *
 * What LVGL *does* give for free is lv_obj_set_ext_click_area(obj, size) --
 * lv_obj_get_click_area() (lv_obj_pos.c) symmetrically expands an object's
 * own coords by `size` px on every side before that containment test runs.
 * That's a real, per-widget, density-tunable answer to the *sizing* half of
 * 10.4 (a sparse layout's buttons can each claim a generous halo; a dense
 * grid's cells claim little or none) -- ui_theme_apply_touch_area() below is
 * a thin helper over exactly that call. It is NOT an answer to the
 * *arbitration* half: if two widgets' expanded boxes ever overlap, z-order
 * decides, not proximity. See ui_theme.c for the extension amounts chosen
 * per density case, and TODO.md 10.4 for the open item this leaves. */

/**
 * Set `widget`'s ext_click_area (see lv_obj_set_ext_click_area() above) to a
 * size appropriate for its own on-screen size and the density of the layout
 * it lives in.
 *
 * `compact_layout` is 10.4's explicit "dense grid" case (numeric keypad,
 * settings list row) as opposed to the sparse main-page button case:
 *   - compact_layout == true:  a small, capped extension -- enough to soften
 *     the exact pixel edge without the expanded box reaching past the
 *     midpoint of the standard inter-cell gap (UI_THEME_PADDING_PX) into a
 *     neighboring cell's own expanded box. See the z-order caveat above:
 *     letting two compact cells' expanded areas actually overlap is a real
 *     mis-tap bug on this backend, not a cosmetic rounding error.
 *   - compact_layout == false: a generous extension, and if the widget's own
 *     smaller edge is below UI_THEME_MIN_TOUCH_TARGET_PX, extended further
 *     so the *effective* clickable square reaches that minimum even though
 *     the drawn widget itself doesn't grow.
 *
 * Must be called after `widget`'s size is known (post-layout, or after an
 * explicit lv_obj_set_size()/similar) -- it reads back lv_obj_get_width()/
 * _height(), which are meaningless before that.
 */
void ui_theme_apply_touch_area(lv_obj_t *widget, bool compact_layout);
