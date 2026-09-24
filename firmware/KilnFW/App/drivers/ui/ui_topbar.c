#include "ui_topbar.h"

#include <string.h>

#include "kiln_ui.h"
#include "ui_theme.h"

/* Navigates to whatever page name was stored as the event's user data. One
 * shared callback rather than a per-page back_btn_cb: the destination is
 * data (cfg->back_page), so there is nothing for a per-page function to do
 * except hold a different string literal. The literal is not copied -- see
 * ui_topbar_cfg_t's comment. */
static void nav_cb(lv_event_t *e)
{
    const char *page = (const char *)lv_event_get_user_data(e);
    if (page) {
        kiln_ui_show(page);
    }
}

/* One icon button. Fixed size rather than LV_SIZE_CONTENT so the proxy's
 * total width is known before layout resolves -- ui_topbar_create() needs
 * that number to cap the title's width, and reading it back post-layout is
 * not possible for a page built DETACHED (kiln_ui.c builds a page before it
 * is ever shown, and geometry read before the first render is all zero).
 *
 * ui_theme_apply_touch_area(btn, true) -- COMPACT, not the sparse case this
 * comment used to argue for. That earlier reasoning ("nothing else is
 * adjacent... the worst case is a tap in the gap landing on one of the two
 * icons the user was aiming between") was never checked against hardware
 * and was wrong: measured live on the panel, the non-compact extension
 * (toward UI_THEME_MIN_TOUCH_TARGET_PX=72px, i.e. ~24px past each 36x26
 * icon's own edge) is many times wider than the UI_TOPBAR_ICON_GAP_PX=4px
 * gap between icons. A tap dead-center on an icon's OWN drawn rectangle
 * still lands inside a LATER-BUILT neighbour's expanded box, and LVGL's
 * z-order-first-match hit test (see ui_theme.h) hands it to that neighbour,
 * not the icon under the finger. Icons are built left-to-right (Back, Home,
 * Prev, Next, Gear -- see ui_topbar_create() below), so every icon except
 * the last in a row was shadowed by whichever came after it: Back was
 * ALWAYS shadowed (something always follows it -- Home at minimum), which
 * is exactly the reported "none of the back buttons work". Compact caps the
 * extension at UI_THEME_PADDING_PX/2 either side, small enough that
 * adjacent icons' expanded boxes no longer swallow each other -- see
 * ui_theme_apply_touch_area()'s own comment for why compact is capped that
 * way. ui_topbar_create() below additionally registers the whole icon row
 * as a touch group (ui_theme_register_touch_group()) so that even the thin
 * sliver where two icons' compact-expanded boxes still meet resolves by
 * nearest real center instead of z-order, as defense in depth. */
/* tap_tag, when non-NULL, is stashed in the button's own lv_obj user data
 * (lv_obj_set_user_data() -- distinct from the *event* user_data set two
 * lines below, which is per-icon event payload such as the page string
 * nav_cb() reads and already varies per caller). kiln_ui.c's
 * log_tap_targets()/kiln_ui_collect_tap_targets() prefer this override over
 * a button's label text: for an icon-only button (no visible caption, e.g.
 * the gear) the label text is an opaque LVGL symbol glyph
 * (LV_SYMBOL_SETTINGS et al.), not a human/test-harness-readable name.
 *
 * A raw `const char *` was deliberately rejected here: user_data on a
 * clickable lv_obj is NOT this file's field alone to use -- ui_confirm.c
 * stashes a heap `ui_confirm_ctx_t *` in the same field on its msgbox, which
 * is also clickable and also reachable by kiln_ui.c's walk (via
 * lv_layer_top()). kiln_ui_tap_name_tag_t's magic word (kiln_ui.h) lets the
 * reader tell "this is really one of my tags" apart from "this is someone
 * else's unrelated pointer" before ever treating it as a string; the reader
 * additionally only looks at this field on a confirmed lv_button_class
 * object (the msgbox never is one) as a second, independent guard. Callers
 * pass a `static const kiln_ui_tap_name_tag_t` (rodata, not .bss/.data --
 * zero RAM cost) rather than building one per call. */
static lv_obj_t *build_icon_named(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb,
                                   void *user_data, const kiln_ui_tap_name_tag_t *tap_tag)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_TOPBAR_ICON_W_PX, UI_TOPBAR_ICON_H_PX);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    if (tap_tag) {
        lv_obj_set_user_data(btn, (void *)tap_tag);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);

    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, true);
    return btn;
}

static lv_obj_t *build_icon(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user_data)
{
    return build_icon_named(parent, symbol, cb, user_data, NULL);
}

/* "settings" tap name for the topbar gear -- see build_icon_named()'s
 * comment for why this is a tagged struct, not a bare string, and why it
 * lives at file scope in rodata rather than being built per call. */
static const kiln_ui_tap_name_tag_t kUiTopbarGearTapName = {
    .magic = KILN_UI_TAP_NAME_MAGIC,
    .name = "settings",
};

/* Non-clickable indicator icon (the relay-life warning). Same fixed size and
 * card background as build_icon()'s buttons so it sits in the row without
 * looking out of place, but no button widget, no event callback, and no
 * touch-area extension -- it navigates nowhere, so it must not steal a tap
 * from a real neighbour. Starts hidden; ui_topbar_set_warning() toggles it
 * at runtime. */
static lv_obj_t *build_indicator(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, UI_TOPBAR_ICON_W_PX, UI_TOPBAR_ICON_H_PX);
    lv_obj_set_style_bg_color(box, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(box, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);

    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    return box;
}

static void set_icon_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        /* Dimmed, not hidden. Hiding would reflow the icon row and move
         * every other icon sideways as the user pages, so the coordinates
         * the tap-target dump published a moment ago would go stale on a
         * press that did not navigate anywhere. */
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
    }
}

void ui_topbar_create(lv_obj_t *scr, const ui_topbar_cfg_t *cfg, ui_topbar_t *out)
{
    if (!scr || !cfg || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* The in-flow bar. Transparent and borderless: it is a layout slot for
     * the title, not a drawn element. */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    out->bar = bar;

    /* Count the icons first: the proxy's width is icon_count-derived and
     * both the proxy and the title cap need it before either is built. */
    int icon_count = 0;
    if (cfg->back_page) icon_count++;
    if (cfg->show_home)  icon_count++;
    if (cfg->prev_cb)    icon_count++;
    if (cfg->next_cb)    icon_count++;
    if (cfg->add_cb)     icon_count++;
    if (cfg->gear_cb)    icon_count++;
    if (cfg->warning_icon) icon_count++;

    if (icon_count > 0) {
        const int32_t icons_w = (int32_t)icon_count * UI_TOPBAR_ICON_W_PX +
                                (int32_t)(icon_count - 1) * UI_TOPBAR_ICON_GAP_PX;
        out->icons_w = icons_w;

        /* Height is the bar plus the page root's own inter-child gap
         * (UI_THEME_PADDING_PX / 2, which every page sets as scr's pad_gap).
         * That gap is dead space between the bar and the content area, so
         * claiming it for touch reach steals nothing: the proxy stops
         * exactly where content begins and can never swallow a tap meant
         * for the first content row. */
        lv_obj_t *icons = lv_obj_create(scr);
        /* FLOATING FIRST, before anything reads this object's geometry --
         * see trap #2 in ui_topbar.h. */
        lv_obj_add_flag(icons, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(icons, icons_w, UI_THEME_STATUS_BAR_HEIGHT_PX + (UI_THEME_PADDING_PX / 2));
        lv_obj_set_style_bg_opa(icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icons, 0, 0);
        lv_obj_set_style_pad_all(icons, 0, 0);
        lv_obj_remove_flag(icons, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(icons, UI_TOPBAR_ICON_GAP_PX, 0);
        lv_obj_align(icons, LV_ALIGN_TOP_RIGHT, 0, 0);
        /* NOT clickable itself. It only has to CONTAIN the touch point so
         * the hit test can descend into the icons (trap #1); making the
         * proxy itself clickable would let a tap in the gap between two
         * icons be swallowed by a container that does nothing. */
        out->icons = icons;

        /* Left-to-right order: Back, Home, Prev, Next, Warning, Gear. Back
         * leftmost puts the most-used control furthest from the screen edge,
         * where a finger is least likely to slip off the glass. The warning
         * indicator is built before the gear (and the row is
         * LV_FLEX_ALIGN_END) so the gear is always the rightmost child and
         * stays flush with the container's right edge even while the hidden
         * warning slot collapses to zero width. */
        if (cfg->back_page) {
            out->back_btn = build_icon(icons, LV_SYMBOL_LEFT, nav_cb, (void *)cfg->back_page);
        }
        if (cfg->show_home) {
            out->home_btn = build_icon(icons, LV_SYMBOL_HOME, nav_cb, (void *)"home");
        }
        if (cfg->prev_cb) {
            /* LV_SYMBOL_PREV/NEXT (the media skip glyphs), not
             * LV_SYMBOL_LEFT/RIGHT: Back already owns a plain left arrow in
             * this same row, and two near-identical arrows side by side is
             * exactly the ambiguity an icon-only bar cannot afford. */
            out->prev_btn = build_icon(icons, LV_SYMBOL_PREV, cfg->prev_cb, NULL);
        }
        if (cfg->next_cb) {
            out->next_btn = build_icon(icons, LV_SYMBOL_NEXT, cfg->next_cb, NULL);
        }
        if (cfg->warning_icon) {
            out->warning_btn = build_indicator(icons, LV_SYMBOL_WARNING);
        }
        if (cfg->add_cb) {
            out->add_btn = build_icon(icons, LV_SYMBOL_FILE, cfg->add_cb, NULL);
        }
        if (cfg->gear_cb) {
            /* "settings" tap name: the 2026-08-20 Menu-button removal left
             * this icon-only gear with no readable label (see
             * build_icon_named()'s comment) -- a bench test harness needs a
             * name to click by. */
            out->gear_btn = build_icon_named(icons, LV_SYMBOL_SETTINGS, cfg->gear_cb, NULL, &kUiTopbarGearTapName);
        }

        /* Every icon here got its click area extended toward
         * UI_THEME_MIN_TOUCH_TARGET_PX (72px) by ui_theme_apply_touch_area()
         * above, but each icon is only UI_TOPBAR_ICON_W_PX (36px) wide with
         * a UI_TOPBAR_ICON_GAP_PX (4px) gap to its neighbor -- the expanded
         * boxes overlap by a wide margin. Measured on hardware: with no
         * arbitration, LVGL's z-order-first-match hit test (see
         * ui_theme.h's touch-group block comment) always resolves that
         * overlap to whichever icon was BUILT LATER (higher z), so every
         * icon except the last in the row was untappable -- Back, built
         * first, was shadowed by Home/Prev/Next/Gear on every page that had
         * any of them, which is exactly "none of the back buttons work".
         * Registering the whole row as one touch group switches overlap
         * resolution to nearest-center instead of z-order, using the
         * arbiter ui_theme.c already provides for exactly this case. */
        lv_obj_t *members[6];
        size_t member_count = 0;
        if (out->back_btn) members[member_count++] = out->back_btn;
        if (out->home_btn) members[member_count++] = out->home_btn;
        if (out->prev_btn) members[member_count++] = out->prev_btn;
        if (out->next_btn) members[member_count++] = out->next_btn;
        if (out->add_btn) members[member_count++] = out->add_btn;
        if (out->gear_btn) members[member_count++] = out->gear_btn;
        if (member_count >= 2) {
            ui_theme_register_touch_group(members, member_count);
        }
    }

    if (cfg->title) {
        /* WIDTH MUST BE SET EXPLICITLY. lv_obj_align()/align_to() set a
         * POSITION only and never constrain width; a label left at its
         * parent's full width still renders text under the icons. That
         * exact bug shipped on the home page (a long "WiFi: 192.168.1.156
         * (kilnctl.local) -- Signal: -45 dBm" string ran under the gear)
         * and is why this is derived here, once, for every page.
         *
         * The bar's own width is read post-update_layout, but a page is
         * BUILT DETACHED (kiln_ui.c builds before showing), so lv_pct(100)
         * may still resolve to 0. Falling back to LV_SIZE_CONTENT in that
         * case is the honest failure: a title that overlaps an icon is
         * cosmetic, a title that vanishes is functional. */
        lv_obj_update_layout(bar);
        int32_t bar_w = lv_obj_get_width(bar);
        int32_t title_w = bar_w - out->icons_w - (UI_THEME_PADDING_PX / 2);

        lv_obj_t *title = lv_label_create(bar);
        lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        if (title_w > 0) {
            lv_obj_set_width(title, title_w);
            /* Ellipsise rather than scroll. This is an always-on kiln
             * panel; a perpetually scrolling label is a needless
             * distraction and a needless redraw cost. */
            lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        }
        lv_label_set_text(title, cfg->title);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
        out->title = title;
    }
}

void ui_topbar_raise(const ui_topbar_t *tb)
{
    if (tb && tb->icons) {
        lv_obj_move_foreground(tb->icons);
    }
}

void ui_topbar_set_title(const ui_topbar_t *tb, const char *text)
{
    if (tb && tb->title && text) {
        lv_label_set_text(tb->title, text);
    }
}

void ui_topbar_set_prev_enabled(const ui_topbar_t *tb, bool enabled)
{
    if (tb) {
        set_icon_enabled(tb->prev_btn, enabled);
    }
}

void ui_topbar_set_next_enabled(const ui_topbar_t *tb, bool enabled)
{
    if (tb) {
        set_icon_enabled(tb->next_btn, enabled);
    }
}

void ui_topbar_set_warning(const ui_topbar_t *tb, ui_topbar_warning_tier_t tier)
{
    if (!tb || !tb->warning_btn) {
        return;
    }
    if (tier == UI_TOPBAR_WARNING_NONE) {
        lv_obj_add_flag(tb->warning_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* No dedicated warn/bad colour tokens exist in ui_theme.h -- reuse the
     * same two accents ui_page_diagnostics.c already treats as
     * warning/fault colours (ACCENT_1 orange, ACCENT_5 red) rather than add
     * a new hex value the contrast memory note warns against. */
    lv_color_t color = (tier == UI_TOPBAR_WARNING_ERROR) ? UI_THEME_ACCENT_5 : UI_THEME_ACCENT_1;
    lv_obj_set_style_bg_color(tb->warning_btn, color, 0);
    lv_obj_remove_flag(tb->warning_btn, LV_OBJ_FLAG_HIDDEN);
}
