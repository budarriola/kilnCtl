#include "ui_page_kiln_cfg_setup.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "kiln_cfg_store.h"
#include "kiln_ui.h"
#include "ota_http.h"
#include "ui_confirm.h"
#include "ui_num_pad.h"
#include "ui_theme.h"
#include "ui_topbar.h"

// "Kiln Config" management screen -- see ui_page_kiln_cfg_setup.h and
// kiln_cfg_store.h's header comments for what a kiln config IS (a named
// snapshot of the whole zones config: relay wiring, thermocouple assignment,
// PID gains, guard thresholds, limits) and why it is never called a
// "profile" anywhere on this screen.
//
// LAYOUT / NO-SCROLL BUDGET (480x320, real content area y=44..311 = 267px,
// same hardware-measured figure ui_page_home.c's/ui_page_network.c's own
// header comments use -- see ui_page_home.c's header comment for how that
// number was obtained):
//
//     active-config label ................. ~20px (one montserrat_14 line)
//     gap .................................. UI_THEME_PADDING_PX/2 = 4px
//     config list (internally scrollable) . 100px fixed
//     gap .................................. 4px
//     status/error label ................... ~18px
//     gap .................................. 4px
//     action row 1: Apply | Save Current As  40px
//     gap .................................. 4px
//     action row 2: Clone | Rename | Delete   40px
//                                            ------
//                                             234px  <= 267px, ~33px margin
//
// Every row above is a FIXED height (no flex_grow anywhere in this page,
// unlike ui_page_home.c's state_card) -- the list is the only thing that can
// grow past its box, and it is explicitly bounded + internally scrollable
// (lv_list, same as ui_page_network.c's Scan/Saved lists), which this
// codebase's standing rule treats as fine: only PAGE-level scrolling is
// forbidden, a small self-contained list scrolling itself is not that.
//
// SAFETY UX (all four of the task's explicit requirements):
//   - Apply goes through ui_confirm.c, wording states plainly that the
//     current relay/thermocouple/PID/guard setup will be overwritten (see
//     apply_confirm_yes_cb()/build's confirm body below).
//   - Apply is refused (not silently allowed) while a firing is running or
//     heaters are on: this file calls the EXACT SAME ota_http_check_
//     interlocks() (ota_http.h) kiln_cfg_http.c's own apply handler is
//     required to call, per kiln_cfg_store_apply()'s own doc comment ("EVERY
//     caller MUST call ota_http_check_interlocks() itself... BEFORE ever
//     calling this" -- kiln_cfg_store_apply() itself does NOT check). A
//     refusal (either from the interlock check or from the store call
//     itself, e.g. a corrupt/oversized blob) is shown on s_status_label with
//     the SPECIFIC reason text, never treated as success.
//   - The active-config label is repainted from kiln_cfg_store_get_active_id()
//     read AGAIN after every apply attempt (apply_confirm_yes_cb() below),
//     not assumed from the id just requested -- so a refused apply can never
//     leave the screen showing the new config as active.
//   - Delete and Save-Current-As-over-an-existing-name ("overwrite") both go
//     through ui_confirm.c. Rename does not (renaming loses no data -- see
//     this file's rename_submit_cb() comment for why that reading was
//     chosen) but a duplicate/over-length name is still rejected and
//     reported via s_status_label, never silently truncated or merged.
static const char *TAG = "ui_page_kiln_cfg_setup";

#define UI_PAGE_KILN_CFG_LIST_HEIGHT_PX 100
#define UI_PAGE_KILN_CFG_ROW_HEIGHT_PX  40

static lv_obj_t *s_active_label;
static lv_obj_t *s_list;
static lv_obj_t *s_status_label;
static lv_obj_t *s_delete_btn; /* disabled when the selection is the active config -- see refresh() */

/* Cached copy of the last kiln_cfg_store_list() call -- the list buttons'
 * user_data point into this array by index, same lifetime convention
 * ui_page_network.c's s_scan_results uses: overwritten wholesale on every
 * refresh(), which also rebuilds every button, so a stale index can never be
 * clicked after the array it pointed into changed under it. */
static kiln_cfg_summary_t s_cfg_list[KILN_CFG_MAX_COUNT];
static uint8_t s_cfg_count;
static int32_t s_selected_id = KILN_CFG_NO_ACTIVE_ID; /* no selection yet */

static void refresh(void);

static void set_status(const char *text, bool is_error)
{
    lv_label_set_text(s_status_label, text ? text : "");
    lv_obj_set_style_text_color(s_status_label, is_error ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_SECONDARY, 0);
}

/* Looks up `id` in the cached list -- used by every action below to name the
 * config in a confirm dialog/status message without a second store call. */
static const kiln_cfg_summary_t *find_cached(int32_t id)
{
    for (uint8_t i = 0; i < s_cfg_count; i++) {
        if (s_cfg_list[i].id == id) {
            return &s_cfg_list[i];
        }
    }
    return NULL;
}

/* Duplicate-name lookup among cached entries, optionally excluding one id
 * (rename/clone-onto-self checks) -- returns the colliding entry, or NULL. */
static const kiln_cfg_summary_t *find_by_name(const char *name, int32_t exclude_id)
{
    for (uint8_t i = 0; i < s_cfg_count; i++) {
        if (s_cfg_list[i].id == exclude_id) {
            continue;
        }
        if (strcmp(s_cfg_list[i].name, name) == 0) {
            return &s_cfg_list[i];
        }
    }
    return NULL;
}

static void row_clicked_cb(lv_event_t *e)
{
    int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);
    s_selected_id = id;
    set_status("", false);
    refresh();
}

/* ---- Apply -------------------------------------------------------------- */

static void apply_confirm_yes_cb(void *user_data)
{
    int32_t id = (int32_t)(intptr_t)user_data;

    /* kiln_cfg_store_apply() itself does NOT check whether a firing is
     * running or heaters are on (kiln_cfg_store.h's own header comment) --
     * this call is the LCD's half of the "every caller must call
     * ota_http_check_interlocks() first" requirement, same predicate
     * kiln_cfg_http.c's own apply handler is required to use rather than a
     * second, possibly-diverging check. */
    char reason[128];
    ota_interlock_result_t gate = ota_http_check_interlocks(reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        set_status(reason, true);
        ESP_LOGW(TAG, "apply(%ld) refused by interlocks: %s", (long)id, reason);
        /* Re-read active id regardless -- never assume the refused apply left
         * anything unchanged just because THIS path refused it; some other
         * caller could have changed it concurrently (web dashboard). */
        refresh();
        return;
    }

    char store_reason[OTA_INTERLOCK_REASON_MAX] = "";
    bool ok = kiln_cfg_store_apply(id, store_reason, sizeof(store_reason));
    if (!ok) {
        set_status(store_reason[0] ? store_reason : "Apply failed", true);
        ESP_LOGW(TAG, "kiln_cfg_store_apply(%ld) failed: %s", (long)id, store_reason);
    } else {
        set_status("Applied.", false);
    }
    /* Re-read the real active id rather than assuming `id` is now active --
     * required regardless of `ok` (see this file's header comment: "never
     * leave the screen showing the new config as active when it was not
     * applied"). */
    refresh();
}

static void apply_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_selected_id == KILN_CFG_NO_ACTIVE_ID) {
        set_status("Select a kiln config first", true);
        return;
    }
    const kiln_cfg_summary_t *cfg = find_cached(s_selected_id);
    char body[224];
    snprintf(body, sizeof(body),
             "Apply kiln config \"%s\"? This will overwrite the CURRENT relay wiring, "
             "thermocouple assignment, PID gains, and guard thresholds with the values saved "
             "in \"%s\". This cannot be undone.",
             cfg ? cfg->name : "?", cfg ? cfg->name : "?");

    ui_confirm_params_t params = {
        .title = "Apply Kiln Config?",
        .body = body,
        .confirm_label = "Apply",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = apply_confirm_yes_cb,
        .user_data = (void *)(intptr_t)s_selected_id,
    };
    ui_confirm_show(&params);
}

/* ---- Save Current As ----------------------------------------------------- */

/* Set right before either the direct save call or the overwrite confirm --
 * ui_confirm.c's dialog closes the num_pad modal, so both must not be open
 * at once; the entered name is copied here so the confirm's on_confirm
 * callback (save_overwrite_confirm_yes_cb) still has it. */
static char s_pending_save_name[KILN_CFG_NAME_MAX_LEN + 1];

static void do_save_current_as(const char *name, int32_t overwrite_id)
{
    int32_t out_id = 0;
    char reason[96] = "";
    bool ok = kiln_cfg_store_save_current(name, overwrite_id, &out_id, reason, sizeof(reason));
    if (!ok) {
        set_status(reason[0] ? reason : "Save failed", true);
        ESP_LOGW(TAG, "kiln_cfg_store_save_current(\"%s\", %ld) failed: %s", name, (long)overwrite_id, reason);
        return;
    }
    s_selected_id = out_id;
    set_status("Saved.", false);
    refresh();
}

static void save_overwrite_confirm_yes_cb(void *user_data)
{
    int32_t existing_id = (int32_t)(intptr_t)user_data;
    do_save_current_as(s_pending_save_name, existing_id);
}

static void save_as_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)value;
    (void)user_data;
    if (!accepted) {
        return;
    }
    if (text[0] == '\0') {
        set_status("Name cannot be blank", true);
        return;
    }
    if (strlen(text) > KILN_CFG_NAME_MAX_LEN) {
        /* ui_num_pad's max_len already caps typed length at
         * KILN_CFG_NAME_MAX_LEN, so this is a defensive belt-and-braces
         * check, not the primary enforcement -- see this file's header
         * comment: "an over-long or duplicate name must be reported, not
         * silently truncated". */
        set_status("Name is too long", true);
        return;
    }

    const kiln_cfg_summary_t *collision = find_by_name(text, KILN_CFG_NO_ACTIVE_ID);
    if (collision) {
        snprintf(s_pending_save_name, sizeof(s_pending_save_name), "%s", text);
        char body[224];
        snprintf(body, sizeof(body),
                 "A kiln config named \"%s\" already exists. Saving will OVERWRITE its saved "
                 "relay wiring, thermocouple assignment, PID gains, and guard thresholds. This "
                 "cannot be undone.",
                 text);
        ui_confirm_params_t params = {
            .title = "Overwrite Kiln Config?",
            .body = body,
            .confirm_label = "Overwrite",
            .confirm_color = UI_THEME_ACCENT_5,
            .on_confirm = save_overwrite_confirm_yes_cb,
            .user_data = (void *)(intptr_t)collision->id,
        };
        ui_confirm_show(&params);
        return;
    }

    if (s_cfg_count >= KILN_CFG_MAX_COUNT) {
        set_status("Store is full -- delete a saved config first", true);
        return;
    }

    do_save_current_as(text, KILN_CFG_NO_ACTIVE_ID);
}

static void save_as_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_num_pad_params_t params = {
        .caption = "Save Current As",
        .mode = UI_NUM_PAD_MODE_TEXT,
        .initial_text = "",
        .max_len = KILN_CFG_NAME_MAX_LEN,
        .on_done = save_as_done_cb,
        .user_data = NULL,
    };
    ui_num_pad_show(&params);
}

/* ---- Clone ---------------------------------------------------------------- */

static void clone_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)value;
    int32_t src_id = (int32_t)(intptr_t)user_data;
    if (!accepted) {
        return;
    }
    if (text[0] == '\0' || strlen(text) > KILN_CFG_NAME_MAX_LEN) {
        set_status(text[0] == '\0' ? "Name cannot be blank" : "Name is too long", true);
        return;
    }
    const kiln_cfg_summary_t *collision = find_by_name(text, KILN_CFG_NO_ACTIVE_ID);
    if (collision) {
        /* Cloning onto an existing name overwrites a DIFFERENT saved config's
         * contents -- same destructive-overwrite reasoning as save-as, just
         * with the source being another saved config instead of the live
         * setup. kiln_cfg_store.h has no "clone with overwrite" call, so this
         * is done as delete-then-clone would be unsafe (loses the target on
         * a clone failure); instead this is refused outright and reported --
         * simpler and never destructive by surprise. A rename of the target
         * out of the way, or a different name, are the ways forward. */
        char msg[192];
        snprintf(msg, sizeof(msg), "\"%s\" already exists -- rename it or choose a different name", text);
        set_status(msg, true);
        return;
    }
    if (s_cfg_count >= KILN_CFG_MAX_COUNT) {
        set_status("Store is full -- delete a saved config first", true);
        return;
    }

    int32_t out_id = 0;
    char reason[96] = "";
    bool ok = kiln_cfg_store_clone(src_id, text, &out_id, reason, sizeof(reason));
    if (!ok) {
        set_status(reason[0] ? reason : "Clone failed", true);
        ESP_LOGW(TAG, "kiln_cfg_store_clone(%ld, \"%s\") failed: %s", (long)src_id, text, reason);
        return;
    }
    s_selected_id = out_id;
    set_status("Cloned.", false);
    refresh();
}

static void clone_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_selected_id == KILN_CFG_NO_ACTIVE_ID) {
        set_status("Select a kiln config first", true);
        return;
    }
    const kiln_cfg_summary_t *cfg = find_cached(s_selected_id);
    char suggested[KILN_CFG_NAME_MAX_LEN + 1] = "";
    if (cfg) {
        /* Best-effort "<name> copy" suggestion, truncated to fit -- this is
         * only a prefill the operator can edit before confirming, not a
         * silent auto-rename, so truncation here is harmless. */
        snprintf(suggested, sizeof(suggested), "%.*s copy", (int)(KILN_CFG_NAME_MAX_LEN - 5), cfg->name);
    }
    ui_num_pad_params_t params = {
        .caption = "Clone As",
        .mode = UI_NUM_PAD_MODE_TEXT,
        .initial_text = suggested,
        .max_len = KILN_CFG_NAME_MAX_LEN,
        .on_done = clone_done_cb,
        .user_data = (void *)(intptr_t)s_selected_id,
    };
    ui_num_pad_show(&params);
}

/* ---- Rename ---------------------------------------------------------------
 * Deliberately no ui_confirm.c dialog here, unlike Apply/Delete/Overwrite:
 * renaming a config changes only its label, never its stored relay/
 * thermocouple/PID/guard contents and never the live setup -- there is
 * nothing destroyed to warn about. A duplicate/over-length name is still
 * refused and reported (never silently truncated or merged into the
 * colliding entry), per this task's explicit requirement. */
static void rename_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)value;
    int32_t id = (int32_t)(intptr_t)user_data;
    if (!accepted) {
        return;
    }
    if (text[0] == '\0') {
        set_status("Name cannot be blank", true);
        return;
    }
    if (strlen(text) > KILN_CFG_NAME_MAX_LEN) {
        set_status("Name is too long", true);
        return;
    }
    if (find_by_name(text, id)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "\"%s\" is already in use by another kiln config", text);
        set_status(msg, true);
        return;
    }

    bool ok = kiln_cfg_store_rename(id, text);
    if (!ok) {
        set_status("Rename failed", true);
        ESP_LOGW(TAG, "kiln_cfg_store_rename(%ld, \"%s\") failed", (long)id, text);
        return;
    }
    set_status("Renamed.", false);
    refresh();
}

static void rename_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_selected_id == KILN_CFG_NO_ACTIVE_ID) {
        set_status("Select a kiln config first", true);
        return;
    }
    const kiln_cfg_summary_t *cfg = find_cached(s_selected_id);
    ui_num_pad_params_t params = {
        .caption = "Rename Kiln Config",
        .mode = UI_NUM_PAD_MODE_TEXT,
        .initial_text = cfg ? cfg->name : "",
        .max_len = KILN_CFG_NAME_MAX_LEN,
        .on_done = rename_done_cb,
        .user_data = (void *)(intptr_t)s_selected_id,
    };
    ui_num_pad_show(&params);
}

/* ---- Delete ---------------------------------------------------------------- */

static void delete_confirm_yes_cb(void *user_data)
{
    int32_t id = (int32_t)(intptr_t)user_data;
    bool ok = kiln_cfg_store_delete(id);
    if (!ok) {
        set_status("Delete failed", true);
        ESP_LOGW(TAG, "kiln_cfg_store_delete(%ld) failed", (long)id);
        return;
    }
    if (s_selected_id == id) {
        s_selected_id = KILN_CFG_NO_ACTIVE_ID;
    }
    set_status("Deleted.", false);
    refresh();
}

static void delete_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_selected_id == KILN_CFG_NO_ACTIVE_ID) {
        set_status("Select a kiln config first", true);
        return;
    }
    const kiln_cfg_summary_t *cfg = find_cached(s_selected_id);
    if (cfg && cfg->is_active) {
        /* kiln_cfg_store_delete() itself allows deleting the active entry
         * (it just clears the active id, per its own doc comment) -- this
         * page still refuses it up front with a clearer, kiln-config-
         * specific explanation than a bare store failure would give, since
         * the live setup came FROM this entry and an operator deleting their
         * only record of it is very likely a mistake, not a benign cleanup. */
        set_status("Cannot delete the active kiln config -- apply a different one first", true);
        return;
    }

    /* 192, not 160 -- GCC's -Wformat-truncation (-Werror in this build)
     * cannot bound %s by KILN_CFG_NAME_MAX_LEN from a bare "%s" the way a
     * "%.*s" width would, so it assumes the widest string literal it can
     * legally hold and flags 160 as too small even though a real name never
     * exceeds KILN_CFG_NAME_MAX_LEN+1 bytes -- rounded up with real margin
     * rather than fought with a %.*s that would (harmlessly) truncate a
     * legitimately-long name display. */
    char body[224];
    snprintf(body, sizeof(body),
             "Delete kiln config \"%s\"? This permanently removes its saved relay wiring, "
             "thermocouple assignment, PID gains, and guard thresholds. This cannot be undone.",
             cfg ? cfg->name : "?");

    ui_confirm_params_t params = {
        .title = "Delete Kiln Config?",
        .body = body,
        .confirm_label = "Delete",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = delete_confirm_yes_cb,
        .user_data = (void *)(intptr_t)s_selected_id,
    };
    ui_confirm_show(&params);
}

/* ---- Refresh / list rebuild ------------------------------------------------ */

static void refresh(void)
{
    int32_t active_id = kiln_cfg_store_get_active_id();

    char active_buf[48];
    if (active_id == KILN_CFG_NO_ACTIVE_ID) {
        snprintf(active_buf, sizeof(active_buf), "Active kiln config: none");
    } else {
        char name[KILN_CFG_NAME_MAX_LEN + 1] = "";
        if (kiln_cfg_store_get_name(active_id, name, sizeof(name))) {
            snprintf(active_buf, sizeof(active_buf), "Active kiln config: %s", name);
        } else {
            snprintf(active_buf, sizeof(active_buf), "Active kiln config: (id %ld)", (long)active_id);
        }
    }
    lv_label_set_text(s_active_label, active_buf);

    s_cfg_count = kiln_cfg_store_list(s_cfg_list, KILN_CFG_MAX_COUNT);

    /* Selection may no longer exist (deleted elsewhere, e.g. from the web
     * dashboard) -- drop it rather than keep pointing Apply/Clone/Rename/
     * Delete at a stale id. */
    if (s_selected_id != KILN_CFG_NO_ACTIVE_ID && !find_cached(s_selected_id)) {
        s_selected_id = KILN_CFG_NO_ACTIVE_ID;
    }

    lv_obj_clean(s_list);
    if (s_cfg_count == 0) {
        /* Empty store (never saved one, or the backend not yet present) --
         * an honest "nothing saved yet" line, not a blank/broken-looking
         * list. */
        lv_list_add_text(s_list, "No saved kiln configs yet");
    } else {
        for (uint8_t i = 0; i < s_cfg_count; i++) {
            char text[KILN_CFG_NAME_MAX_LEN + 16];
            snprintf(text, sizeof(text), "%s%s", s_cfg_list[i].name, s_cfg_list[i].is_active ? "  (active)" : "");
            lv_obj_t *btn = lv_list_add_button(s_list, NULL, text);
            bool selected = (s_cfg_list[i].id == s_selected_id);
            lv_obj_set_style_bg_color(btn, selected ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
            lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s_cfg_list[i].id);
            lv_obj_update_layout(btn);
            ui_theme_apply_touch_area(btn, true);
        }
    }

    bool have_selection = (s_selected_id != KILN_CFG_NO_ACTIVE_ID);
    const kiln_cfg_summary_t *sel = have_selection ? find_cached(s_selected_id) : NULL;
    if (have_selection && sel && sel->is_active) {
        lv_obj_add_state(s_delete_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_delete_btn, LV_STATE_DISABLED);
    }
}

/* ---- Build ------------------------------------------------------------- */

static lv_obj_t *build_action_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, UI_PAGE_KILN_CFG_ROW_HEIGHT_PX);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);
    return btn;
}

lv_obj_t *ui_page_kiln_cfg_setup_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Kiln Config",
        .back_page = "kiln_setup",
        .show_home = true,
    }, &tb);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    s_active_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_active_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_active_label, "Active kiln config: --");

    s_list = lv_list_create(content);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_height(s_list, UI_PAGE_KILN_CFG_LIST_HEIGHT_PX);

    s_status_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_status_label, "");

    lv_obj_t *row1 = lv_obj_create(content);
    lv_obj_set_width(row1, lv_pct(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row1, UI_THEME_PADDING_PX / 2, 0);
    build_action_btn(row1, "Apply", apply_btn_cb);
    build_action_btn(row1, "Save Current As", save_as_btn_cb);

    lv_obj_t *row2 = lv_obj_create(content);
    lv_obj_set_width(row2, lv_pct(100));
    lv_obj_set_height(row2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_pad_all(row2, 0, 0);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row2, UI_THEME_PADDING_PX / 2, 0);
    build_action_btn(row2, "Clone", clone_btn_cb);
    build_action_btn(row2, "Rename", rename_btn_cb);
    s_delete_btn = build_action_btn(row2, "Delete", delete_btn_cb);

    ui_topbar_raise(&tb);

    refresh();

    return scr;
}
