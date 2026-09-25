#include "ui_page_network_manage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "wifi_prov.h"

// See ui_page_network_manage.h's header comment for why this page exists
// (split out of ui_page_network.c 2026-08-24, TODO.md's no-scroll budget
// item for that page). Every wifi_prov.h call here is a straight move from
// ui_page_network.c -- same functions, same async-worker-task shape to keep
// a blocking scan/connect off lvgl_port_task (see that file's git history
// for the original freeze bug and fix), just relocated to their own page so
// this page's own content budget is independent of ui_page_network.c's
// status/mode/QR content.
static const char *TAG __attribute__((unused)) = "ui_page_network_manage";

#define UI_PAGE_NETWORK_MANAGE_REFRESH_MS 1000
#define UI_PAGE_NETWORK_MANAGE_SCAN_MAX 20
#define UI_PAGE_NETWORK_MANAGE_SAVED_MAX 8

/* ---- No-scroll budget arithmetic (TODO.md's "ui_page_network.c's
 * worst-case fit is ~268px against a ~264px budget" item -- see
 * ui_theme.h's UI_THEME_PAGE_CONTENT_BUDGET_PX for the real, computed
 * number and why it differs slightly from every "~264px" comment elsewhere
 * in this codebase).
 *
 * Unlike the old combined page, this page has exactly ONE state to budget:
 * the toggle row, the Scan button, the scan-status label, and whichever of
 * Scan/Saved is showing are ALWAYS all visible together -- there is no
 * connected-vs-not-connected/manage-open branching left to enumerate, so
 * the worst case is simply "whichever list block is taller" (Saved, since
 * it has an extra title line Scan doesn't). Mirrors the real lv_obj_set_*
 * calls in ui_page_network_manage_build()/build_list_block() below exactly
 * -- keep this arithmetic and those calls in sync. */
#define UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX 36
#define UI_PAGE_NETWORK_MANAGE_SCAN_BTN_HEIGHT_PX   44
#define UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX       70

/* toggle_row + scan_btn + scan_status_label(1 line) + saved_title(1 line) +
 * saved_list, plus one UI_THEME_PADDING_PX/2 inter-child gap after each of
 * the 5 `content` children (4 gaps). */
#define UI_PAGE_NETWORK_MANAGE_WORST_CASE_HEIGHT_PX \
    (UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX + UI_PAGE_NETWORK_MANAGE_SCAN_BTN_HEIGHT_PX + \
     UI_THEME_FONT_LINE_HEIGHT_PX + UI_THEME_FONT_LINE_HEIGHT_PX + UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX + \
     (4 * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_NETWORK_MANAGE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_network_manage.c: the Scan/Saved list block exceeds "
               "UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink "
               "UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX or the toggle/Scan button heights, don't "
               "widen the budget to match.");

/* ---- Scan/Saved toggle (mutually exclusive) ---- */
static lv_obj_t *s_scan_toggle_btn;
static lv_obj_t *s_saved_toggle_btn;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_scan_status_label;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_saved_title;
static lv_obj_t *s_saved_list;
static bool s_list_showing_saved = true; /* which of Scan/Saved is visible -- default Saved */

/* Last wifi_prov_scan() results -- kept alive as long as s_scan_list's
 * buttons exist (their LV_EVENT_CLICKED user_data points into this array by
 * index), overwritten only by the next Scan tap, which also rebuilds the
 * list itself. Only ever touched from lvgl_port_task, same single-task-owned
 * discipline as ui_page_network.c's original. */
static wifi_prov_scan_result_t s_scan_results[UI_PAGE_NETWORK_MANAGE_SCAN_MAX];

/* ---- Async scan job -- see ui_page_network.c's original header comment for
 * the freeze bug this fixes (unchanged by the move: wifi_prov_scan() is a
 * real blocking radio scan, so it still can't run on lvgl_port_task). */
typedef struct {
    SemaphoreHandle_t lock;
    bool busy;
    bool done;
    esp_err_t err;
    size_t count;
    wifi_prov_scan_result_t results[UI_PAGE_NETWORK_MANAGE_SCAN_MAX];
} scan_job_t;

static scan_job_t s_scan_job;

static void scan_row_clicked_cb(lv_event_t *e);

static void scan_worker_task(void *arg)
{
    (void)arg;
    esp_err_t err;
    size_t count = 0;
    wifi_prov_scan_result_t results[UI_PAGE_NETWORK_MANAGE_SCAN_MAX];

    err = wifi_prov_scan(results, UI_PAGE_NETWORK_MANAGE_SCAN_MAX, &count);

    xSemaphoreTake(s_scan_job.lock, portMAX_DELAY);
    s_scan_job.err = err;
    s_scan_job.count = count;
    memcpy(s_scan_job.results, results, count * sizeof(results[0]));
    s_scan_job.done = true;
    s_scan_job.busy = false;
    xSemaphoreGive(s_scan_job.lock);

    vTaskDelete(NULL);
}

static void apply_scan_job_result(void)
{
    esp_err_t err;
    size_t count;

    if (!s_scan_job.lock) return;
    xSemaphoreTake(s_scan_job.lock, portMAX_DELAY);
    bool done = s_scan_job.done;
    if (done) {
        err = s_scan_job.err;
        count = s_scan_job.count;
        memcpy(s_scan_results, s_scan_job.results, count * sizeof(s_scan_results[0]));
        s_scan_job.done = false;
    }
    xSemaphoreGive(s_scan_job.lock);
    if (!done) return;

    lv_obj_clean(s_scan_list);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        lv_label_set_text(s_scan_status_label, "Scanning disabled in AP mode");
        return;
    }
    if (err != ESP_OK) {
        lv_label_set_text(s_scan_status_label, "Scan failed");
        return;
    }
    if (count == 0) {
        lv_label_set_text(s_scan_status_label, "No networks found");
        return;
    }

    char status[32];
    snprintf(status, sizeof(status), "%u network%s found", (unsigned)count, count == 1 ? "" : "s");
    lv_label_set_text(s_scan_status_label, status);

    for (size_t i = 0; i < count; i++) {
        /* Explicit %.*s width -- see ui_page_network.c's original comment on
         * this exact snprintf() for why (-Wformat-truncation/-Werror). */
        char text[WIFI_PROV_SSID_MAX_LEN + 16];
        snprintf(text, sizeof(text), "%.*s%s  %d dBm", WIFI_PROV_SSID_MAX_LEN, s_scan_results[i].ssid,
                 s_scan_results[i].secure ? " *" : "", (int)s_scan_results[i].rssi);
        lv_obj_t *btn = lv_list_add_button(s_scan_list, NULL, text);
        lv_obj_add_event_cb(btn, scan_row_clicked_cb, LV_EVENT_CLICKED, &s_scan_results[i]);
        lv_obj_update_layout(btn);
        ui_theme_apply_touch_area(btn, true);
    }
}

/* ---- Connect modal (scan-tap -> password entry -> wifi_prov_add_network()) ----
 * Built once, hidden, same "pages/overlays never torn down" widget lifetime
 * as every other page (kiln_ui.h's header comment). */
static lv_obj_t *s_connect_modal;
static lv_obj_t *s_connect_title;
static lv_obj_t *s_connect_ta;
static lv_obj_t *s_connect_status_label;
static lv_obj_t *s_connect_kb;
static char s_connect_ssid[WIFI_PROV_SSID_MAX_LEN + 1];

typedef struct {
    SemaphoreHandle_t lock;
    bool busy;
    bool done;
    esp_err_t err;
} connect_job_t;

static connect_job_t s_connect_job;
static char s_connect_job_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
static char s_connect_job_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];

static void connect_worker_task(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_prov_add_network(s_connect_job_ssid, strlen(s_connect_job_ssid),
                                           s_connect_job_password, strlen(s_connect_job_password));
    xSemaphoreTake(s_connect_job.lock, portMAX_DELAY);
    s_connect_job.err = err;
    s_connect_job.done = true;
    s_connect_job.busy = false;
    xSemaphoreGive(s_connect_job.lock);
    vTaskDelete(NULL);
}

static void connect_modal_close(void);
static void refresh_saved_list(void);

static void apply_connect_job_result(void)
{
    if (!s_connect_job.lock) return;
    xSemaphoreTake(s_connect_job.lock, portMAX_DELAY);
    bool done = s_connect_job.done;
    esp_err_t err = s_connect_job.err;
    if (done) s_connect_job.done = false;
    xSemaphoreGive(s_connect_job.lock);
    if (!done) return;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_add_network(%s) failed: %s", s_connect_job_ssid, esp_err_to_name(err));
        const char *msg = (err == ESP_ERR_NO_MEM) ? "Saved network list is full" : "Could not save credentials";
        lv_label_set_text(s_connect_status_label, msg);
        return;
    }
    connect_modal_close();
    refresh_saved_list();
}

/* ---- Pending-forget context, for the confirm msgbox's footer button cb.
 * Only one confirm dialog can be open at a time, so a single static buffer
 * is enough -- set right before lv_msgbox_create() below. */
static char s_pending_forget_ssid[WIFI_PROV_SSID_MAX_LEN + 1];

static void refresh_cb(lv_timer_t *timer);

/* ---- Scan/Saved toggle ---- */

static void apply_toggle_style(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
}

static void scan_toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    s_list_showing_saved = false;
    refresh_cb(NULL);
}

static void saved_toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    s_list_showing_saved = true;
    refresh_cb(NULL);
}

/* ---- Connect modal ---- */

static void connect_modal_open(const char *ssid)
{
    snprintf(s_connect_ssid, sizeof(s_connect_ssid), "%s", ssid);
    char title[48];
    snprintf(title, sizeof(title), "Connect to %s", s_connect_ssid);
    lv_label_set_text(s_connect_title, title);
    lv_textarea_set_text(s_connect_ta, "");
    lv_label_set_text(s_connect_status_label, "");
    lv_obj_remove_flag(s_connect_modal, LV_OBJ_FLAG_HIDDEN);
}

static void connect_modal_close(void)
{
    lv_obj_add_flag(s_connect_modal, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(s_connect_ta, "");
}

static void connect_cancel_cb(lv_event_t *e)
{
    (void)e;
    connect_modal_close();
}

static void connect_submit_cb(lv_event_t *e)
{
    (void)e;
    if (!s_connect_job.lock) {
        lv_label_set_text(s_connect_status_label, "Could not save credentials");
        return;
    }

    xSemaphoreTake(s_connect_job.lock, portMAX_DELAY);
    bool already_busy = s_connect_job.busy;
    if (!already_busy) {
        s_connect_job.busy = true;
        s_connect_job.done = false;
    }
    xSemaphoreGive(s_connect_job.lock);
    if (already_busy) return;

    const char *password = lv_textarea_get_text(s_connect_ta);
    snprintf(s_connect_job_ssid, sizeof(s_connect_job_ssid), "%s", s_connect_ssid);
    snprintf(s_connect_job_password, sizeof(s_connect_job_password), "%s", password);
    lv_label_set_text(s_connect_status_label, "Connecting...");

    BaseType_t created = xTaskCreate(connect_worker_task, "wifi_connect_ui", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_connect_job.lock, portMAX_DELAY);
        s_connect_job.busy = false;
        xSemaphoreGive(s_connect_job.lock);
        lv_label_set_text(s_connect_status_label, "Could not start");
    }
}

static void scan_row_clicked_cb(lv_event_t *e)
{
    wifi_prov_scan_result_t *result = (wifi_prov_scan_result_t *)lv_event_get_user_data(e);
    connect_modal_open(result->ssid);
}

/* ---- Forget confirmation ---- */

static void forget_confirm_yes_cb(lv_event_t *e)
{
    /* Deliberately left synchronous -- see ui_page_network.c's original
     * comment on this exact call: wifi_prov_forget_network() is a plain NVS
     * write, no radio operation. */
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    esp_err_t err = wifi_prov_forget_network(s_pending_forget_ssid, strlen(s_pending_forget_ssid));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_forget_network(%s) failed: %s", s_pending_forget_ssid, esp_err_to_name(err));
    }
    lv_msgbox_close(mbox);
    refresh_saved_list();
}

static void forget_confirm_no_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    lv_msgbox_close(mbox);
}

static void forget_row_clicked_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    snprintf(s_pending_forget_ssid, sizeof(s_pending_forget_ssid), "%s", ssid);

    /* Cached, non-blocking -- see refresh_saved_list()'s comment below for
     * why this page never calls wifi_prov_get_saved_networks() directly. */
    wifi_prov_saved_network_t saved[UI_PAGE_NETWORK_MANAGE_SAVED_MAX];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks_cached(saved, UI_PAGE_NETWORK_MANAGE_SAVED_MAX, &saved_count);

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Forget network");
    if (saved_count <= 1) {
        lv_msgbox_add_text(mbox, "This is the last saved network. Forgetting it will switch this "
                                  "board to Access Point mode. Continue?");
    } else {
        lv_msgbox_add_text_fmt(mbox, "Forget \"%s\"?", s_pending_forget_ssid);
    }
    lv_obj_t *yes = lv_msgbox_add_footer_button(mbox, "Forget");
    lv_obj_add_event_cb(yes, forget_confirm_yes_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_t *no = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_event_cb(no, forget_confirm_no_cb, LV_EVENT_CLICKED, mbox);
}

/* ---- Scan ---- */

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_scan_job.lock) {
        lv_label_set_text(s_scan_status_label, "Scan failed to start");
        return;
    }

    xSemaphoreTake(s_scan_job.lock, portMAX_DELAY);
    bool already_busy = s_scan_job.busy;
    if (!already_busy) {
        s_scan_job.busy = true;
        s_scan_job.done = false;
    }
    xSemaphoreGive(s_scan_job.lock);
    if (already_busy) return; /* one scan at a time */

    lv_obj_clean(s_scan_list);
    lv_label_set_text(s_scan_status_label, "Scanning...");

    BaseType_t created = xTaskCreate(scan_worker_task, "wifi_scan_ui", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_scan_job.lock, portMAX_DELAY);
        s_scan_job.busy = false;
        xSemaphoreGive(s_scan_job.lock);
        lv_label_set_text(s_scan_status_label, "Scan failed to start");
    }
}

/* ---- Saved networks ---- */

static void refresh_saved_list(void)
{
    lv_obj_clean(s_saved_list);

    static wifi_prov_saved_network_t saved[UI_PAGE_NETWORK_MANAGE_SAVED_MAX];
    /* Copies of each SSID that outlive this function -- see
     * ui_page_network.c's original comment on this exact array for why. */
    static char ssid_ctx[UI_PAGE_NETWORK_MANAGE_SAVED_MAX][WIFI_PROV_SSID_MAX_LEN + 1];
    size_t count = 0;
    /* Non-blocking cached read (2026-09-25 LCD freeze fix) -- this runs from
     * refresh_cb() on lvgl_port_task every UI_PAGE_NETWORK_MANAGE_REFRESH_MS,
     * and the real wifi_prov_get_saved_networks() is a queued call to the
     * Wi-Fi owner task sized to wait up to WIFI_OWNER_WAIT_MS (12s, see
     * wifi_prov_api.c) behind a scan or connect already in flight ahead of
     * it -- calling that here froze the whole LCD for the duration of any
     * concurrent scan/connect. This shows the last-known list instead;
     * see wifi_prov.h's wifi_prov_get_saved_networks_cached() doc comment. */
    wifi_prov_get_saved_networks_cached(saved, UI_PAGE_NETWORK_MANAGE_SAVED_MAX, &count);

    if (count == 0) {
        lv_list_add_text(s_saved_list, "No saved networks");
        return;
    }

    const char *active_ssid = wifi_prov_get_saved_ssid();
    bool sta_connected = wifi_prov_is_sta_connected();

    for (size_t i = 0; i < count; i++) {
        snprintf(ssid_ctx[i], sizeof(ssid_ctx[i]), "%s", saved[i].ssid);

        bool connected = sta_connected && strcmp(saved[i].ssid, active_ssid) == 0;
        char text[48];
        snprintf(text, sizeof(text), "%s%s", saved[i].ssid, connected ? "  (connected)" : "");

        lv_obj_t *row = lv_obj_create(s_saved_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_color(label, connected ? UI_THEME_ACCENT_4 : UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, text);

        lv_obj_t *forget_btn = lv_button_create(row);
        lv_obj_set_style_bg_color(forget_btn, UI_THEME_ACCENT_5, 0);
        lv_obj_set_style_radius(forget_btn, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(forget_btn, forget_row_clicked_cb, LV_EVENT_CLICKED, ssid_ctx[i]);
        lv_obj_t *forget_label = lv_label_create(forget_btn);
        lv_obj_set_style_text_color(forget_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(forget_label, "Forget");
        lv_obj_center(forget_label);
        lv_obj_update_layout(forget_btn);
        ui_theme_apply_touch_area(forget_btn, true);
    }
}

/* ---- Refresh ---- */

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    apply_scan_job_result();
    apply_connect_job_result();

    if (s_list_showing_saved) {
        lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_saved_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_saved_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_scan_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_saved_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_saved_list, LV_OBJ_FLAG_HIDDEN);
    }
    apply_toggle_style(s_scan_toggle_btn, !s_list_showing_saved);
    apply_toggle_style(s_saved_toggle_btn, s_list_showing_saved);

    /* Cheap (<=8 rows), gated on nothing -- same reasoning as
     * ui_page_network.c's original: keeps the "connected" highlight and any
     * web-side change in sync every tick. */
    refresh_saved_list();
}

/* ---- Build ---- */

static void build_connect_modal(lv_obj_t *scr)
{
    s_connect_modal = lv_obj_create(scr);
    lv_obj_add_flag(s_connect_modal, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_connect_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_connect_modal, 0, 0);
    lv_obj_set_style_bg_color(s_connect_modal, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_connect_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_connect_modal, 0, 0);
    lv_obj_set_style_pad_all(s_connect_modal, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_connect_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_connect_modal, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_connect_modal, LV_OBJ_FLAG_SCROLLABLE);

    s_connect_title = lv_label_create(s_connect_modal);
    lv_obj_set_style_text_color(s_connect_title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_connect_title, "Connect");

    lv_obj_t *pw_label = lv_label_create(s_connect_modal);
    lv_obj_set_style_text_color(pw_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(pw_label, "Password (blank for open network):");

    s_connect_ta = lv_textarea_create(s_connect_modal);
    lv_obj_set_width(s_connect_ta, lv_pct(100));
    lv_textarea_set_one_line(s_connect_ta, true);
    lv_textarea_set_password_mode(s_connect_ta, true);
    lv_textarea_set_max_length(s_connect_ta, WIFI_PROV_PASSWORD_MAX_LEN);

    s_connect_status_label = lv_label_create(s_connect_modal);
    lv_obj_set_style_text_color(s_connect_status_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_connect_status_label, "");

    lv_obj_t *btn_row = lv_obj_create(s_connect_modal);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, UI_THEME_PADDING_PX, 0);

    lv_obj_t *connect_btn = lv_button_create(btn_row);
    lv_obj_set_height(connect_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(connect_btn, 1);
    lv_obj_set_style_bg_color(connect_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_add_event_cb(connect_btn, connect_submit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_update_layout(connect_btn);
    ui_theme_apply_touch_area(connect_btn, false);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_height(cancel_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_add_event_cb(cancel_btn, connect_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_update_layout(cancel_btn);
    ui_theme_apply_touch_area(cancel_btn, false);

    s_connect_kb = lv_keyboard_create(s_connect_modal);
    lv_keyboard_set_textarea(s_connect_kb, s_connect_ta);
}

lv_obj_t *ui_page_network_manage_build(void)
{
    /* Created up front, before refresh_cb() ever runs -- see
     * ui_page_network.c's original scan_job_t comment for why lazy creation
     * on first tap was a bug. */
    s_scan_job.lock = xSemaphoreCreateMutex();
    s_connect_job.lock = xSemaphoreCreateMutex();
    if (!s_scan_job.lock || !s_connect_job.lock) {
        ESP_LOGE(TAG, "ui_page_network_manage: mutex allocation failed -- Scan/connect disabled");
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Manage networks",
        .back_page = "network",
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

    lv_obj_t *toggle_row = lv_obj_create(content);
    lv_obj_set_width(toggle_row, lv_pct(100));
    lv_obj_set_height(toggle_row, UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX);
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toggle_row, 0, 0);
    lv_obj_set_style_pad_all(toggle_row, 0, 0);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(toggle_row, UI_THEME_PADDING_PX, 0);

    s_scan_toggle_btn = lv_button_create(toggle_row);
    lv_obj_set_height(s_scan_toggle_btn, UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX);
    lv_obj_set_flex_grow(s_scan_toggle_btn, 1);
    lv_obj_set_style_radius(s_scan_toggle_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_border_width(s_scan_toggle_btn, 1, 0);
    lv_obj_set_style_border_color(s_scan_toggle_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_scan_toggle_btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(s_scan_toggle_btn, scan_toggle_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_toggle_label = lv_label_create(s_scan_toggle_btn);
    lv_obj_set_style_text_color(scan_toggle_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(scan_toggle_label, "Scan");
    lv_obj_center(scan_toggle_label);
    lv_obj_update_layout(s_scan_toggle_btn);
    ui_theme_apply_touch_area(s_scan_toggle_btn, false);

    s_saved_toggle_btn = lv_button_create(toggle_row);
    lv_obj_set_height(s_saved_toggle_btn, UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX);
    lv_obj_set_flex_grow(s_saved_toggle_btn, 1);
    lv_obj_set_style_radius(s_saved_toggle_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_border_width(s_saved_toggle_btn, 1, 0);
    lv_obj_set_style_border_color(s_saved_toggle_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_saved_toggle_btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(s_saved_toggle_btn, saved_toggle_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *saved_toggle_label = lv_label_create(s_saved_toggle_btn);
    lv_obj_set_style_text_color(saved_toggle_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(saved_toggle_label, "Saved");
    lv_obj_center(saved_toggle_label);
    lv_obj_update_layout(s_saved_toggle_btn);
    ui_theme_apply_touch_area(s_saved_toggle_btn, false);

    s_scan_btn = lv_button_create(content);
    lv_obj_set_width(s_scan_btn, lv_pct(100));
    lv_obj_set_height(s_scan_btn, UI_PAGE_NETWORK_MANAGE_SCAN_BTN_HEIGHT_PX);
    lv_obj_set_style_bg_color(s_scan_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_scan_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_border_width(s_scan_btn, 1, 0);
    lv_obj_set_style_border_color(s_scan_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_scan_btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_label = lv_label_create(s_scan_btn);
    lv_obj_set_style_text_color(scan_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(scan_label, "Scan for networks");
    lv_obj_center(scan_label);
    lv_obj_update_layout(s_scan_btn);
    ui_theme_apply_touch_area(s_scan_btn, false);

    s_scan_status_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_scan_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_scan_status_label, "Tap Scan to search for networks");

    s_scan_list = lv_list_create(content);
    lv_obj_set_width(s_scan_list, lv_pct(100));
    lv_obj_set_height(s_scan_list, UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX);

    s_saved_title = lv_label_create(content);
    lv_obj_set_style_text_color(s_saved_title, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_saved_title, "Saved networks:");

    s_saved_list = lv_list_create(content);
    lv_obj_set_width(s_saved_list, lv_pct(100));
    lv_obj_set_height(s_saved_list, UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX);

    ui_topbar_raise(&tb);

    /* Connect modal built last so it's topmost in z-order while shown --
     * same reasoning as ui_page_network.c's original build_connect_modal()
     * call site. */
    build_connect_modal(scr);

    lv_timer_create(refresh_cb, UI_PAGE_NETWORK_MANAGE_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real state immediately instead of waiting one tick */

    return scr;
}
