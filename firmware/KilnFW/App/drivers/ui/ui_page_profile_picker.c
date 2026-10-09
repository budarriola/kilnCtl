#include "ui_page_profile_picker.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hal_time.h"
#include "kiln_ui.h"
#include "profiles_builtin.h"
#include "profiles_favorites.h"
#include "profiles_store.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profile_detail.h"
#include "ui_page_profile_picker_format.h"
#include "ui_profile_list_order.h"
#include "ui_lcd_lock.h"
#include "ui_theme.h"
#include "ui_topbar.h"

static const char *TAG = "ui_page_profile_picker";

/* Layout -- UI_PLAN.md 6.2's owner-decided 4x64 column, zero slack:
 *
 *     4 rows * 64px + 3 gaps * 4px = 256 + 12 = 268  <=  268
 *
 * This is exact, not generous -- any later addition to this page's content
 * column (a header, a status line, a wider gap) fails this assert, which is
 * the intended behaviour (see UI_PLAN.md 6.2's "Zero slack" section): this
 * page has no room left and the compiler now says so. Do NOT recover the
 * 8px shortfall against UI_THEME_MIN_TOUCH_TARGET_PX (72) with
 * ui_theme_apply_touch_area() -- the compact extension would have to eat the
 * entire 4px inter-row gap from both sides at once, making two vertically
 * adjacent rows' hit-boxes overlap (the exact mis-tap defect already found
 * and fixed on the relay-life page). 64px rows, plain hit-boxes, no
 * extension. */
#define UI_PAGE_PROFILE_PICKER_ROW_GAP_PX ((int32_t)(UI_THEME_PADDING_PX / 2))
#define UI_PAGE_PROFILE_PICKER_WORST_CASE_HEIGHT_PX                                                                  \
    ((UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE * UI_PAGE_PROFILE_PICKER_ROW_H_PX) +                                      \
     ((UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE - 1) * UI_PAGE_PROFILE_PICKER_ROW_GAP_PX))
_Static_assert(UI_PAGE_PROFILE_PICKER_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_profile_picker's 4x64 column must fit the page content budget -- split across more pages, "
               "do not scroll");

#define UI_PAGE_PROFILE_PICKER_DELETE_BTN_W_PX 64
#define UI_PAGE_PROFILE_PICKER_DELETE_BTN_H_PX 36

/* Two-tap arm/confirm delete, same idiom (and the same constants, by value --
 * these are this page's own copy, not a shared #define, since
 * ui_page_diagnostics.c's are that page's private module state) as the
 * relay-life page's Reset button: first tap arms a 5s window and relabels to
 * "Confirm?", second tap inside that window actually deletes, and a
 * 300ms debounce guards against a bounced double-press being read as
 * arm-then-immediately-confirm. */
#define UI_PAGE_PROFILE_PICKER_DELETE_CONFIRM_US (5 * 1000 * 1000)
#define UI_PAGE_PROFILE_PICKER_DELETE_DEBOUNCE_US (300 * 1000)

/* Total ids this page can ever hold: every user slot plus every builtin
 * catalogue entry (the builtin id range is PROFILE_BUILTIN_ID_BASE..255, so
 * 128 is a hard ceiling regardless of how large the catalogue grows). */
#define UI_PAGE_PROFILE_PICKER_MAX_IDS (PROFILES_MAX_COUNT + (256 - PROFILE_BUILTIN_ID_BASE))

typedef struct picker_ctx_s {
    bool manage;
    ui_topbar_t tb;

    uint8_t ids[UI_PAGE_PROFILE_PICKER_MAX_IDS];
    uint8_t id_count;
    uint8_t page;

    lv_obj_t *rows_col;
    lv_obj_t *row[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
    lv_obj_t *row_label[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
    lv_obj_t *row_del_btn[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
    int64_t del_confirm_deadline_us[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
    uint8_t del_confirm_id[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
} picker_ctx_t;

/* Per-row event user data: which context and which on-page slot (0..3) this
 * widget is. The id a slot refers to is looked up dynamically at click time
 * (ctx->ids[ctx->page * ROWS_PER_PAGE + slot]) rather than captured when the
 * widget was built, so re-rendering the same row objects across pages/
 * refreshes never requires re-binding the event callback. */
typedef struct {
    picker_ctx_t *ctx;
    uint8_t slot;
} row_ud_t;

static picker_ctx_t s_manage_ctx;
static picker_ctx_t s_pick_ctx;
static row_ud_t s_manage_name_ud[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
static row_ud_t s_manage_del_ud[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];
static row_ud_t s_pick_name_ud[UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE];

static ui_page_profile_picker_pick_cb_t s_pick_cb;

/* page_count()/the (page, slot) -> flat index derivation are pure and
 * host-tested in ui_page_profile_picker_format.c/test_ui_page_profile_picker_
 * format.c -- this is a thin wrapper binding in this page's own
 * ROWS_PER_PAGE constant. */
static uint8_t page_count(const picker_ctx_t *ctx)
{
    return ui_page_profile_picker_format_page_count(ctx->id_count, UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE);
}

static uint16_t row_index(const picker_ctx_t *ctx, uint8_t slot)
{
    return ui_page_profile_picker_format_row_index(ctx->page, slot, UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE);
}

/* Loads ids from both namespaces, in the order the plan requires ordering to
 * start from: user slots 0..PROFILES_MAX_COUNT-1 (occupied only), then
 * builtin catalogue entries (skipping hidden ones) -- then stable-partitions
 * favorites to the front via ui_profile_list_order(), matching
 * main_page.html's orderProfilesByFavorite() exactly. */
static void load_ids(picker_ctx_t *ctx)
{
    uint8_t raw[UI_PAGE_PROFILE_PICKER_MAX_IDS];
    size_t n = 0;

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT && n < UI_PAGE_PROFILE_PICKER_MAX_IDS; id++) {
        profile_t prof;
        if (profiles_http_get(id, &prof)) {
            raw[n++] = id;
        }
    }

    for (size_t i = 0; i < g_builtin_profile_count && n < UI_PAGE_PROFILE_PICKER_MAX_IDS; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (profiles_builtin_is_hidden(id)) {
            continue;
        }
        raw[n++] = id;
    }

    ui_profile_list_order(raw, n, profiles_favorites_is, ctx->ids);
    ctx->id_count = (uint8_t)n;
}

static void render(picker_ctx_t *ctx);

/* Delete tap -- arm on first tap, delete on confirm, matching
 * ui_page_diagnostics.c's relay_reset_btn_clicked_cb() idiom. Delete parity
 * with the web (UI_PLAN.md 6.2): profiles_http_delete() THEN
 * profiles_favorites_set(id, false), in that order, so no dangling favorite
 * survives. */
/* L11 (LCD UI audit 2026-10-09): the matching web route is ROUTE_TIER_ADMIN.
 * Pressing the button without an admin session only raises the PIN keypad;
 * the success callback then performs the arm step itself. */
static void delete_do(row_ud_t *ud)
{
    if (!ud || !ud->ctx) {
        return;
    }
    picker_ctx_t *ctx = ud->ctx;
    uint8_t slot = ud->slot;
    uint16_t idx = row_index(ctx, slot);
    if (idx >= ctx->id_count) {
        return;
    }
    uint8_t id = ctx->ids[idx];
    if (!ui_page_profile_picker_is_deletable(id)) {
        return; /* defense in depth -- the button is not built for a builtin row at all */
    }

    int64_t now = (int64_t)hal_time_now_us();
    int64_t deadline = ctx->del_confirm_deadline_us[slot];
    bool armed = deadline != 0 && now < deadline;
    if (armed && now < deadline - UI_PAGE_PROFILE_PICKER_DELETE_CONFIRM_US + UI_PAGE_PROFILE_PICKER_DELETE_DEBOUNCE_US) {
        return; /* debounce: ignore a tap too soon after the arming tap */
    }

    if (deadline != 0 && !armed) {
        /* Stale tap: the 5s confirm window lapsed since the arming tap, but
         * the button label was never reset (no lv_timer watches the
         * deadline). Treat this tap as "acknowledge the lapse" rather than
         * silently re-arming -- relabel back to "Delete" and require a
         * separate tap to arm again, same as if this button had never been
         * armed. */
        ctx->del_confirm_deadline_us[slot] = 0;
        if (ctx->row_del_btn[slot]) {
            lv_obj_t *label = lv_obj_get_child(ctx->row_del_btn[slot], 0);
            if (label) {
                lv_label_set_text(label, "Delete");
            }
        }
        return;
    }

    if (armed) {
        ctx->del_confirm_deadline_us[slot] = 0;
        /* id (above) was just recomputed from ctx->ids at THIS tap, which
         * render()'s load_ids() keeps current -- so it already reflects
         * anything that reordered rows since the arming tap (another delete
         * completing, a favorite toggle from elsewhere, a background
         * refresh). Comparing it against the id armed back then, rather than
         * re-deriving a second copy of "current id at this slot" via a new
         * helper, is the cheapest correct re-check on the lvgl task's
         * already zero-slack stack (UI_PLAN.md 6.2) -- an earlier version
         * that called load_ids() again from a dedicated function measured
         * 80 B over the 4880 B ceiling here. On mismatch, disarm and
         * re-render instead of deleting the wrong profile. */
        if (id != ctx->del_confirm_id[slot]) {
            ESP_LOGW(TAG, "profile picker: armed id %u no longer at slot %u", ctx->del_confirm_id[slot], slot);
            render(ctx);
            return;
        }
        bool ok = profiles_http_delete(id);
        if (ok) {
            esp_err_t fav_err = profiles_favorites_set(id, false);
            if (fav_err != ESP_OK) {
                ESP_LOGW(TAG, "profile %u deleted but favorite clear failed: %s", id, esp_err_to_name(fav_err));
            }
        } else {
            ESP_LOGW(TAG, "profiles_http_delete(%u) failed", id);
        }
        /* Re-render: the deleted id disappears, everything after it shifts
         * up a slot, and the page count may shrink -- render() re-derives
         * both from a fresh load_ids() rather than patching this one row. */
        render(ctx);
    } else {
        ctx->del_confirm_deadline_us[slot] = now + UI_PAGE_PROFILE_PICKER_DELETE_CONFIRM_US;
        ctx->del_confirm_id[slot] = id;
        if (ctx->row_del_btn[slot]) {
            lv_obj_t *label = lv_obj_get_child(ctx->row_del_btn[slot], 0);
            if (label) {
                lv_label_set_text(label, "Confirm?");
            }
        }
    }
}

/* The PIN keypad's success callback runs the same step the tap would have
 * (arm or confirm, role re-checked); a failed/cancelled PIN never reaches it. */
static void delete_unlocked_cb(void *user_data)
{
    if (ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {
        delete_do((row_ud_t *)user_data);
    }
}

static void delete_btn_clicked_cb(lv_event_t *e)
{
    row_ud_t *ud = (row_ud_t *)lv_event_get_user_data(e);
    if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {
        ui_lcd_lock_run_gated("Admin PIN to delete profile", LCD_PIN_ROLE_ADMIN, delete_unlocked_cb, ud);
        return;
    }
    delete_do(ud);
}

/* Name/row tap -- MANAGE mode opens the detail screen; PICK mode invokes the
 * caller's callback. Either way the id is resolved dynamically, same
 * reasoning as delete_btn_clicked_cb() above. */
static void row_name_clicked_cb(lv_event_t *e)
{
    row_ud_t *ud = (row_ud_t *)lv_event_get_user_data(e);
    if (!ud || !ud->ctx) {
        return;
    }
    picker_ctx_t *ctx = ud->ctx;
    uint16_t idx = row_index(ctx, ud->slot);
    if (idx >= ctx->id_count) {
        return;
    }
    uint8_t id = ctx->ids[idx];

    if (ctx->manage) {
        ui_page_profile_detail_set_id(id, "profiles");
        kiln_ui_show("profile_detail");
    } else if (s_pick_cb) {
        s_pick_cb(id);
    }
}

static void update_title(picker_ctx_t *ctx)
{
    char buf[24];
    uint8_t total_pages = page_count(ctx);
    snprintf(buf, sizeof(buf), "%s %u/%u", ctx->manage ? "Profiles" : "Select Profile", (unsigned)(ctx->page + 1),
             (unsigned)total_pages);
    ui_topbar_set_title(&ctx->tb, buf);
    ui_topbar_set_prev_enabled(&ctx->tb, ctx->page > 0);
    ui_topbar_set_next_enabled(&ctx->tb, (uint8_t)(ctx->page + 1) < total_pages);
}

static void render(picker_ctx_t *ctx)
{
    if (!ctx->rows_col) {
        /* Called before build() -- both ui_page_profiles_refresh() callers
         * (ui_page_config.c and ui_page_profile_builder_review.c) invoke the
         * manage refresh before the very first kiln_ui_show("profiles"),
         * when the screen has never been built and this context is still
         * zeroed. Nothing to render yet; build() calls render() itself once
         * the widgets exist. */
        return;
    }

    load_ids(ctx);

    ctx->page = ui_page_profile_picker_format_clamp_page(ctx->page, ctx->id_count, UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE);

    for (uint8_t slot = 0; slot < UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE; slot++) {
        ctx->del_confirm_deadline_us[slot] = 0;

        uint16_t idx = row_index(ctx, slot);
        if (idx >= ctx->id_count) {
            lv_obj_add_flag(ctx->row[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(ctx->row[slot], LV_OBJ_FLAG_HIDDEN);

        uint8_t id = ctx->ids[idx];
        char name[PROFILE_NAME_MAX_LEN + 1 + 64];
        bool is_fav = profiles_favorites_is(id);

        if (id >= PROFILE_BUILTIN_ID_BASE) {
            const builtin_profile_t *entry = profiles_builtin_entry(id);
            snprintf(name, sizeof(name), "%s", entry ? entry->title : "(unknown)");
        } else {
            profile_t prof;
            if (profiles_http_get(id, &prof)) {
                snprintf(name, sizeof(name), "%s", prof.name);
            } else {
                snprintf(name, sizeof(name), "(empty)");
            }
        }

        char label[PROFILE_NAME_MAX_LEN + 1 + 96];
        ui_page_profile_picker_format_label(name, is_fav, label, sizeof(label));
        lv_label_set_text(ctx->row_label[slot], label);

        if (ctx->row_del_btn[slot]) {
            bool deletable = ctx->manage && ui_page_profile_picker_is_deletable(id);
            if (deletable) {
                lv_obj_remove_flag(ctx->row_del_btn[slot], LV_OBJ_FLAG_HIDDEN);
                lv_obj_t *del_label = lv_obj_get_child(ctx->row_del_btn[slot], 0);
                if (del_label) {
                    lv_label_set_text(del_label, "Delete");
                }
            } else {
                lv_obj_add_flag(ctx->row_del_btn[slot], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    update_title(ctx);
}

/* Two fixed screen instances exist for this page's whole lifetime (manage
 * and pick, each built once and cached forever by kiln_ui.c), so Prev/Next
 * are one plain callback pair per instance rather than a single callback
 * threading a context through ui_topbar_cfg_t's user_data (that struct's
 * prev_cb/next_cb are always invoked with NULL user data by
 * ui_topbar.c's build_icon() -- see ui_topbar_create()). */
static void picker_prev(picker_ctx_t *ctx)
{
    if (ctx->page > 0) {
        ctx->page--;
        render(ctx);
    }
}

static void picker_next(picker_ctx_t *ctx)
{
    if ((uint8_t)(ctx->page + 1) < page_count(ctx)) {
        ctx->page++;
        render(ctx);
    }
}

static void manage_prev_cb(lv_event_t *e)
{
    (void)e;
    picker_prev(&s_manage_ctx);
}

static void manage_next_cb(lv_event_t *e)
{
    (void)e;
    picker_next(&s_manage_ctx);
}

static void pick_prev_cb(lv_event_t *e)
{
    (void)e;
    picker_prev(&s_pick_ctx);
}

static void pick_next_cb(lv_event_t *e)
{
    (void)e;
    picker_next(&s_pick_ctx);
}

static void new_profile_cb(lv_event_t *e)
{
    (void)e;
    ui_page_profile_builder_start_new();
    kiln_ui_show("profile_builder_zones");
}

/* One row: star/name label (flex_grow(1), LV_LABEL_LONG_CLIP) then, in
 * manage mode only, a right-aligned 64x36 Delete button -- same "plain
 * container + sibling buttons" shape as ui_page_network_manage.c's
 * saved-network rows, chosen so the name tap target and the Delete tap
 * target are never ambiguous (no button nested inside another button).
 *
 * LONG_CLIP, not LONG_DOT: LONG_DOT's lv_obj_get_self_height() ->
 * lv_label_set_long_mode() -> lv_obj_invalidate() -> lv_event_send() ->
 * cleanup_event_list() -> lv_malloc_core() chain, newly reachable from
 * ui_home_refresh_cb() through lv_obj_update_layout(), would push
 * ui_home_refresh_cb's own contribution higher than its measured 4128 B
 * against the lvgl task's 4880 B stack ceiling this task has carried since
 * the 2026-09-04 panic post-mortem (see check_all_task_stack_budgets.ps1
 * and this file's own history) -- do not reintroduce LONG_DOT here
 * without re-measuring that budget. A name that overflows the column is
 * simply clipped rather than ellipsized; profile names are short enough
 * in practice (PROFILE_NAME_MAX_LEN) that this is a cosmetic trade, not a
 * usability loss. */
static void build_row(picker_ctx_t *ctx, uint8_t slot, row_ud_t *name_ud, row_ud_t *del_ud)
{
    lv_obj_t *row = lv_obj_create(ctx->rows_col);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, UI_PAGE_PROFILE_PICKER_ROW_H_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ctx->row[slot] = row;

    name_ud->ctx = ctx;
    name_ud->slot = slot;

    lv_obj_t *name_btn = lv_button_create(row);
    lv_obj_set_height(name_btn, lv_pct(100));
    lv_obj_set_flex_grow(name_btn, 1);
    lv_obj_set_style_bg_opa(name_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(name_btn, 0, 0);
    lv_obj_set_style_pad_all(name_btn, 0, 0);
    lv_obj_add_event_cb(name_btn, row_name_clicked_cb, LV_EVENT_CLICKED, name_ud);

    lv_obj_t *label = lv_label_create(name_btn);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, "");
    lv_obj_center(label);
    ctx->row_label[slot] = label;

    if (ctx->manage) {
        del_ud->ctx = ctx;
        del_ud->slot = slot;

        lv_obj_t *del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, UI_PAGE_PROFILE_PICKER_DELETE_BTN_W_PX, UI_PAGE_PROFILE_PICKER_DELETE_BTN_H_PX);
        lv_obj_set_style_bg_color(del_btn, UI_THEME_ACCENT_5, 0);
        lv_obj_set_style_radius(del_btn, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_set_style_pad_all(del_btn, 0, 0);
        lv_obj_add_event_cb(del_btn, delete_btn_clicked_cb, LV_EVENT_CLICKED, del_ud);
        lv_obj_add_flag(del_btn, LV_OBJ_FLAG_HIDDEN); /* shown per-row by render() only for deletable ids */

        lv_obj_t *del_label = lv_label_create(del_btn);
        lv_obj_set_style_text_color(del_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(del_label, "Delete");
        lv_obj_center(del_label);

        ctx->row_del_btn[slot] = del_btn;
    } else {
        ctx->row_del_btn[slot] = NULL;
    }
}

static lv_obj_t *build(picker_ctx_t *ctx, bool manage, row_ud_t *name_ud, row_ud_t *del_ud)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->manage = manage;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_cfg_t cfg = {
        .title = manage ? "Profiles" : "Select Profile",
        .back_page = manage ? "config" : "home",
        .show_home = true,
        .prev_cb = manage ? manage_prev_cb : pick_prev_cb,
        .next_cb = manage ? manage_next_cb : pick_next_cb,
    };
    if (manage) {
        cfg.add_cb = new_profile_cb;
    }
    ui_topbar_create(scr, &cfg, &ctx->tb);

    ctx->rows_col = lv_obj_create(scr);
    lv_obj_set_width(ctx->rows_col, lv_pct(100));
    lv_obj_set_flex_grow(ctx->rows_col, 1);
    lv_obj_set_style_bg_opa(ctx->rows_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->rows_col, 0, 0);
    lv_obj_set_style_pad_all(ctx->rows_col, 0, 0);
    lv_obj_set_flex_flow(ctx->rows_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ctx->rows_col, UI_PAGE_PROFILE_PICKER_ROW_GAP_PX, 0);
    lv_obj_remove_flag(ctx->rows_col, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t slot = 0; slot < UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE; slot++) {
        /* del_ud is NULL on the pick (non-manage) build; only form &del_ud[slot]
         * when it's actually backed by an array, since offsetting a NULL
         * pointer is undefined behaviour even though build_row() never
         * dereferences it outside the ctx->manage branch. */
        row_ud_t *row_del_ud = ctx->manage ? &del_ud[slot] : NULL;
        build_row(ctx, slot, &name_ud[slot], row_del_ud);
    }

    ui_topbar_raise(&ctx->tb);

    ctx->page = 0;
    render(ctx);
    return scr;
}

lv_obj_t *ui_page_profile_picker_build_manage(void)
{
    return build(&s_manage_ctx, true, s_manage_name_ud, s_manage_del_ud);
}

void ui_page_profile_picker_manage_refresh(void)
{
    s_manage_ctx.page = 0;
    render(&s_manage_ctx);
}

lv_obj_t *ui_page_profile_picker_build_pick(void)
{
    return build(&s_pick_ctx, false, s_pick_name_ud, NULL);
}

void ui_page_profile_picker_pick_refresh(void)
{
    s_pick_ctx.page = 0;
    render(&s_pick_ctx);
}

void ui_page_profile_picker_set_pick_cb(ui_page_profile_picker_pick_cb_t cb)
{
    s_pick_cb = cb;
}
