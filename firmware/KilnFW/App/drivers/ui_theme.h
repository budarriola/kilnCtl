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
#include <stddef.h>

#include "lvgl.h"
#include "settings.h" /* DISPLAY_WIDTH -- UI_THEME_PAGE_CONTENT_BUDGET_PX's derivation below */

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

/* Dual-role alias, Phase 7 theme/style pass (TODO.md 1223-1225):
 * WEB_UI_RESPONSIVE.md sec 5.1 item 2 (2026-09-03) renamed the web
 * dashboard's neutral-status token to `--neutral` (`main_page.html`,
 * `readiness_page.html`), and both pages define it as
 * `var(--ui-text-secondary)` in dark mode -- i.e. "neutral status" and
 * "secondary text" are already the same color on the web side, just reached
 * through two different names for two different roles. This header had no
 * name for the "neutral" role at all (the LCD side has never drawn a
 * neutral/can't-check-yet status chip); UI_THEME_COLOR_NEUTRAL exists so a
 * future LCD status widget reaches for the theme's own dual-role name
 * instead of either inventing a new hex or reaching for
 * UI_THEME_COLOR_TEXT_SECONDARY directly and losing the "this IS the neutral
 * status color" intent at the call site. Same value, zero repaint -- see
 * UI_THEME.md's "Web dashboard parity" section for the mirrored note. */
#define UI_THEME_COLOR_NEUTRAL_HEX   UI_THEME_COLOR_TEXT_SECONDARY_HEX
#define UI_THEME_COLOR_NEUTRAL       UI_THEME_COLOR_TEXT_SECONDARY

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

/* Accent 3: teal/cyan. WEB_UI_RESPONSIVE.md sec 7 (2026-09-03): also
 * designated the dominant/primary-action accent for the web UI's focus ring
 * (theme.css's button:focus-visible), since it was the one accent carrying
 * no existing safety meaning (accent-1 is pause, accent-5 is stop/danger).
 * Same #3ec6c6 value, no repaint here -- if kiln_ui.c's still-placeholder
 * screen grows real button chrome, its primary/confirm buttons should reach
 * for this accent too, for parity with the web side. */
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

/* Spacing scale, Phase 7 theme/style pass (TODO.md 1223-1225): mirrors
 * theme.css's `--ui-space-1..5` (WEB_UI_RESPONSIVE.md sec 7.2 item 2,
 * 2026-09-03), same five pixel values, so a new LCD layout choosing a gap
 * has the same named rungs the web side reaches for instead of a fresh
 * literal. Additive only, like the web side's own adoption note: this does
 * NOT go back and rewrite every existing `lv_obj_set_style_pad_*(x, N, 0)`
 * call across the ui_page_*.c files to spell N as one of these -- most of
 * those numbers (many are 0, 2, 3, 4) are load-bearing against the no-scroll
 * budget (UI_THEME_PAGE_CONTENT_BUDGET_PX below), hand-fit to a specific
 * page's arithmetic in its own comment, not spacing debt to clean up; a mass
 * find/replace risks silently nudging a page's worst-case height past its
 * _Static_assert with no compiler error to catch it (the assert only checks
 * the constants IT was written against, not a rename of what the call sites
 * pass). New spacing decisions should reach for these; existing ones stay as
 * the page's own budget arithmetic already documents them.
 * UI_THEME_PADDING_PX is kept as the existing name, redefined as an alias
 * for UI_THEME_SPACE_2 so its value (8) and every pre-existing reference to
 * it are unchanged -- same "rename not a rewrite" the web side's own
 * `--ui-padding: var(--ui-space-2)` alias uses. */
#define UI_THEME_SPACE_1                4
#define UI_THEME_SPACE_2                8
#define UI_THEME_SPACE_3                12
#define UI_THEME_SPACE_4                20
#define UI_THEME_SPACE_5                32

/* Standard padding/gap between grouped elements (grid cells, card interior
 * padding), in pixels. Alias for UI_THEME_SPACE_2 -- see the spacing-scale
 * comment above. */
#define UI_THEME_PADDING_PX            UI_THEME_SPACE_2

/* Persistent top status bar height, in pixels. Sized to comfortably hold a
 * status icon row without eating too much of the 320px height budget the
 * rest of the page (10.3's content) needs. */
#define UI_THEME_STATUS_BAR_HEIGHT_PX  32

/* ---- Card shadow, Phase 7 theme/style pass (TODO.md 1223-1225) ----------
 * theme.css adopted two translucent-black drop shadows, `--ui-shadow-1`/`-2`
 * (WEB_UI_RESPONSIVE.md sec 7.2 item 3, 2026-09-03):
 *   --ui-shadow-1: 0 1px 2px rgba(0,0,0,.12), 0 1px 1px rgba(0,0,0,.08);
 *   --ui-shadow-2: 0 2px 6px rgba(0,0,0,.18), 0 1px 2px rgba(0,0,0,.1);
 * LVGL has no multi-layer box-shadow syntax to copy verbatim -- one
 * lv_style shadow (color/width/spread/ofs_y/opa) is the closest single-layer
 * analog, so this picks the STRONGER of each pair's two alpha figures
 * (0.12 -> LV_OPA (12%), 0.18 -> ~46 LV_OPA (18%)) as that level's shadow
 * opacity rather than trying to stack two lv_style shadows for one widget.
 * Same intent as the web pair -- level 1 is a subtle resting lift for a
 * static card, level 2 a slightly stronger lift -- not a byte-identical
 * render, which is not achievable across the two rendering backends. Pure
 * paint: lv_style shadow properties draw outside the widget's own box and do
 * NOT participate in flex/grid layout sizing (LVGL, like CSS box-shadow,
 * never reserves layout space for a shadow), so applying this to any
 * existing card costs zero page-budget pixels -- see this file's own
 * UI_THEME_PAGE_CONTENT_BUDGET_PX section below for why that budget is the
 * one thing a Phase 7 change must never move. */
#define UI_THEME_SHADOW_1_OPA   LV_OPA_10
#define UI_THEME_SHADOW_2_OPA   LV_OPA_20
#define UI_THEME_SHADOW_1_WIDTH_PX   4
#define UI_THEME_SHADOW_2_WIDTH_PX   8
#define UI_THEME_SHADOW_OFS_Y_PX     2

/**
 * Apply the Phase 7 card-shadow treatment (see the block comment above) to
 * `card`. `level` selects UI_THEME_SHADOW_1_OPA/WIDTH_PX (level == 1, the
 * common "resting card" case -- info cards, status cards, chart backgrounds)
 * or UI_THEME_SHADOW_2_OPA/WIDTH_PX (level == 2, a slightly stronger lift for
 * a card meant to read as raised above its neighbors, e.g. a modal/overlay
 * surface). Any other `level` value is treated as 1. Pure lv_style_t
 * property writes on `card`'s own LV_PART_MAIN/LV_STATE_DEFAULT selector --
 * does not touch `card`'s size, position, or any other object, and does not
 * allocate (no lv_style_t is created; this writes directly via
 * lv_obj_set_style_shadow_*(), the same local-style pattern every other
 * ui_page_*.c call site already uses for bg_color/radius/etc, so this is
 * safe to call repeatedly, including from a build() function that also sets
 * bg_color/radius on the same object in the usual order).
 */
void ui_theme_apply_card_shadow(lv_obj_t *card, int level);

/* ---- No-scroll content budget -- TODO.md 10.3's hard rule ("every page
 * must fit its content height without scrolling; overflow is split into
 * another page, never scrolled") -- see UI_PLAN.md.
 *
 * Every page comment across this codebase has cited this budget as
 * "~264px", derived by prose rather than by a #define -- which is exactly
 * how ui_page_network.c's worst case drifted to a few px over it without
 * anyone noticing (nothing forced the prose and the real ui_theme.h
 * constants to agree). Computed here instead, from the same constants
 * every page's own arithmetic already uses:
 *
 *   landscape panel height (480x320, DISPLAY_WIDTH -- settings.h's
 *   Kconfig-driven CONFIG_KILNCTL_DISPLAY_WIDTH is the panel's UNROTATED
 *   short edge, 320px, which becomes the landscape HEIGHT once rotated)
 *     - 2 * UI_THEME_PADDING_PX   (scr's own top+bottom outer pad)
 *     - UI_THEME_STATUS_BAR_HEIGHT_PX   (the fixed top bar)
 *     - UI_THEME_PADDING_PX / 2   (scr's pad_gap between the bar and content)
 *   = 320 - 16 - 32 - 4 = 268px.
 *
 * That real number is 268, not the ~264 everyone has been citing -- close
 * enough that nobody's arithmetic was ever wildly wrong, but every
 * "~264px" comment in this codebase is now off by 4px from what the code
 * actually computes. Left un-edited elsewhere (they are correct in spirit
 * and this constant is now the single source of truth going forward); new
 * budget arithmetic should reference UI_THEME_PAGE_CONTENT_BUDGET_PX
 * directly rather than copying either number as a literal.
 *
 * GEOMETRY INVARIANT (DISPLAY_ST7796_PLAN.md Sec.6 Step 4): this budget is a
 * compile-time constant, enforced per-page by _Static_assert
 * (ui_page_temperature.c:150, ui_page_network.c:191/195,
 * ui_page_network_manage.c:56 -- also grep-checked by
 * App/test/check_ui_budget_asserts.ps1), but the panel it is computed for is
 * chosen at RUNTIME once display panel auto-detection lands (ILI9488 vs
 * ST7796, see DISPLAY_ST7796_PLAN.md Sec.6 Step 3). That only stays sound
 * because both panels currently share the SAME geometry: 320x480 native,
 * 480x320 landscape -- so DISPLAY_WIDTH and every _Static_assert built from
 * it are correct no matter which panel actually answered the RDDID probe.
 * A future THIRD panel with a different native size breaks this enforcement
 * mechanism, not just some page's layout: the asserts would keep passing at
 * compile time while checking the wrong panel's budget at runtime. Do not
 * silently weaken or drop the asserts to accommodate such a panel -- that
 * mismatch needs solving explicitly (e.g. a runtime budget check, or making
 * the constant genuinely per-panel), not papered over. */
#define UI_THEME_PAGE_CONTENT_BUDGET_PX \
    (DISPLAY_WIDTH - (2 * UI_THEME_PADDING_PX) - UI_THEME_STATUS_BAR_HEIGHT_PX - (UI_THEME_PADDING_PX / 2))

/* LV_FONT_DEFAULT (montserrat_14)'s real single-line height with LVGL's
 * default line spacing, per ui_page_home.c's own budget derivation. Used by
 * every per-page worst-case _Static_assert below rather than each page
 * re-guessing "~20px" independently. */
#define UI_THEME_FONT_LINE_HEIGHT_PX   20

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
 * per density case, and TODO.md 10.4 for the open item this leaves.
 *
 * This does NOT guarantee that calling ui_theme_apply_touch_area() on a
 * widget alone makes its full extended area tappable. lv_indev_search_obj()
 * (lv_indev.c) only recurses from a parent into its children if the tap
 * point is inside the PARENT's raw, un-extended obj->coords (lv_area_is_
 * point_on() against obj->coords, not the click-area-expanded box) --
 * ext_click_area is read only once search has already descended to the
 * widget itself. A parent sized exactly to its child's drawn box (e.g.
 * LV_SIZE_CONTENT with pad_all(0)) therefore makes that child's extension
 * unreachable dead space on the side(s) the parent doesn't cover. Callers
 * relying on ui_theme_apply_touch_area() to reach UI_THEME_MIN_TOUCH_TARGET_PX
 * on a widget smaller than that must independently ensure the parent's own
 * box is at least as large as the extended child box -- see
 * ui_page_profile_detail.c's action_row pad_ver for a worked example. */

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

/* ---- Touch-group arbitration -- TODO.md 10.4's "open item" -------------
 *
 * ui_theme_apply_touch_area() above answers 10.4's *sizing* question, not
 * its *arbitration* question: if two widgets' expanded click areas genuinely
 * overlap, LVGL's own lv_indev_search_obj() (components/lvgl/src/indev/
 * lv_indev.c) hands the touch to whichever is tested first in z-order --
 * plain rectangle containment via lv_obj_hit_test()/lv_area_is_point_on(),
 * no distance-to-center comparison anywhere in that path. 10.4's status
 * update left that case as a deliberately-unbuilt open item: no dense grid
 * existed yet to prove the arbitration logic was actually needed.
 *
 * This section builds that arbiter, opt-in, as infrastructure ahead of a
 * real consumer (same position ui_theme_apply_touch_area() itself was in
 * when it was written -- nothing called it yet either). It does NOT replace
 * LVGL's default hit-test: lv_indev_search_obj() still runs first and is
 * correct for the overwhelming majority of layouts (sparse buttons,
 * non-overlapping extended click areas). It only matters for a widget that
 * opts into a registered *group*: if LVGL's normal search resolves a touch
 * to a widget that is a member of a registered group, every group member
 * whose own (already-extended, via lv_obj_get_click_area()) click area
 * contains the touch point becomes a candidate, and whichever candidate's
 * *actual* (unexpanded, lv_obj_get_coords()) center is closest to the touch
 * point wins -- overriding LVGL's z-order pick with a proximity pick, but
 * only within that one group.
 *
 * How the override actually reaches LVGL: lv_indev.c's _lv_indev_read()
 * copies the read callback's `data->point` straight into
 * `indev->pointer.act_point` (lv_indev.c ~line 765) before indev_proc_press()
 * calls lv_indev_search_obj() again, for real, off of that exact point
 * (lv_indev.c's pointer_search_obj(), ~line 1656, walks sys/top/screen/
 * bottom layers with the same function). So touch_read_cb() doesn't need to
 * fight LVGL's press/release state machine, synthesize an LV_EVENT_CLICKED,
 * or reach into any private indev field: it only needs to rewrite
 * `data->point` to the arbitration winner's own center *before returning*.
 * LVGL's normal pipeline then re-resolves that (now-corrected) point on its
 * own, through its own public search path, and every other behavior --
 * press/release edges, dragging, long-press, scrolling -- keeps working
 * exactly as LVGL implements it, because nothing about that state machine
 * was touched.
 *
 * Nothing calls ui_theme_register_touch_group() yet -- same situation
 * ui_theme_apply_touch_area() was in when it was first written: no dense
 * grid page (10.3) exists yet to have overlapping extended click areas in
 * the first place. This is reusable infrastructure for the day one does.
 */

/* Registry capacity. Small and fixed on purpose -- this is meant for one or
 * two genuinely-dense clusters (a keypad, a tightly packed settings row),
 * not a general-purpose replacement for LVGL's own tree search. */
#define UI_THEME_TOUCH_GROUP_MAX_GROUPS    4
#define UI_THEME_TOUCH_GROUP_MAX_WIDGETS   16

/**
 * Register a group of widgets that should arbitrate touches between
 * themselves by nearest-center, instead of relying solely on LVGL's
 * z-order-first-match default (see the block comment above for exactly how
 * and when this kicks in).
 *
 * `widgets` is copied into an internal fixed-size slot -- the caller's array
 * itself does not need to outlive the call, but the `lv_obj_t *` pointers it
 * contains must outlive the group's registration (i.e. don't register a
 * group of widgets and then delete one of them without also caring that
 * this registry still points at freed memory; there is no unregister call
 * today because nothing needs one yet).
 *
 * A group of fewer than 2 widgets is a no-op (arbitration between fewer than
 * two candidates is meaningless) and is silently ignored, as is registering
 * past UI_THEME_TOUCH_GROUP_MAX_GROUPS or a `count` past
 * UI_THEME_TOUCH_GROUP_MAX_WIDGETS -- deliberately loud limits (small,
 * `#define`d, greppable) rather than a dynamic allocation for a mechanism
 * that only exists to serve one or two dense clusters at a time.
 *
 * Widgets NOT registered in any group are entirely unaffected -- they keep
 * relying on LVGL's normal hit-test alone, exactly as before this existed.
 */
void ui_theme_register_touch_group(lv_obj_t **widgets, size_t count);

/**
 * True once at least one group has been registered. lvgl_port.c's
 * touch_read_cb() checks this before doing any of the extra work below, so
 * the common case (no dense grid page built yet, or a build that never
 * calls ui_theme_register_touch_group() at all) costs nothing beyond this
 * one flag check per touch poll.
 */
bool ui_theme_touch_groups_active(void);

/**
 * Given the widget LVGL's own lv_indev_search_obj() resolved a touch point
 * to (`default_target`, may be NULL if nothing was hit), return the widget
 * the touch should actually be attributed to.
 *
 * If `default_target` is NULL, or is not a member of any registered group,
 * this returns `default_target` unchanged -- LVGL's own answer stands. If it
 * IS a member of a registered group, every member of that same group whose
 * click area (lv_obj_get_click_area(), i.e. its own bounds expanded by
 * whatever ui_theme_apply_touch_area() gave it) contains `point` becomes a
 * candidate, and the candidate whose actual on-screen center is closest to
 * `point` (squared Euclidean distance, no sqrt needed since only the
 * ordering matters) is returned. `default_target` itself is always a valid
 * candidate (LVGL already decided its click area contains the point), so
 * this never returns NULL when `default_target` was non-NULL.
 */
lv_obj_t *ui_theme_resolve_touch_target(lv_obj_t *default_target, lv_point_t point);
