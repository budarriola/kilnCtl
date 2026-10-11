#include "ui_page_network_manage.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR: s_scan_results is LVGL-task + copy only, no ISR/DMA */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiln_ui.h"
#include "ui_confirm.h"
#include "ui_lcd_lock.h"
#include "ui_page_profile_picker_format.h"
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
/* L4 (LCD UI audit 2026-10-09): the Scan and Saved lists page (fixed rows per
 * page with prev/next, like the profile picker) instead of scrolling. Both
 * list containers have LV_OBJ_FLAG_SCROLLABLE cleared. */
#define UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE        2
#define UI_PAGE_NETWORK_MANAGE_ROW_HEIGHT_PX        40
#define UI_PAGE_NETWORK_MANAGE_PAGER_HEIGHT_PX      36
#define UI_PAGE_NETWORK_MANAGE_ROW_GAP_PX           4
#define UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX \
    ((UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE * UI_PAGE_NETWORK_MANAGE_ROW_HEIGHT_PX) + \
     ((UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE - 1) * UI_PAGE_NETWORK_MANAGE_ROW_GAP_PX))

/* toggle_row + scan_btn + scan_status_label(1 line) + saved_title(1 line) +
 * list + pager row, plus one UI_THEME_PADDING_PX/2 inter-child gap after each
 * of the 6 `content` children (5 gaps). */
#define UI_PAGE_NETWORK_MANAGE_WORST_CASE_HEIGHT_PX \
    (UI_PAGE_NETWORK_MANAGE_TOGGLE_ROW_HEIGHT_PX + UI_PAGE_NETWORK_MANAGE_SCAN_BTN_HEIGHT_PX + \
     UI_THEME_FONT_LINE_HEIGHT_PX + UI_THEME_FONT_LINE_HEIGHT_PX + UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX + \
     UI_PAGE_NETWORK_MANAGE_PAGER_HEIGHT_PX + (5 * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_NETWORK_MANAGE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_network_manage.c: the Scan/Saved list block exceeds "
               "UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink "
               "UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE or the toggle/Scan button heights, don't "
               "widen the budget to match.");
_Static_assert(UI_PAGE_NETWORK_MANAGE_SCAN_MAX <= 255 && UI_PAGE_NETWORK_MANAGE_SAVED_MAX <= 255,
               "page helpers take uint8_t counts");

/* ---- Scan/Saved toggle (mutually exclusive) ---- */
static lv_obj_t *s_scan_toggle_btn;
static lv_obj_t *s_saved_toggle_btn;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_scan_status_label;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_saved_title;
static lv_obj_t *s_saved_list;
static lv_obj_t *s_pager_prev_btn;
static lv_obj_t *s_pager_next_btn;
static lv_obj_t *s_pager_label;
static uint8_t s_scan_page;
static uint8_t s_saved_page;
static size_t s_scan_count;
static size_t s_saved_count;
static bool s_list_showing_saved = true; /* which of Scan/Saved is visible -- default Saved */

/* Last wifi_prov_scan() results -- kept alive as long as s_scan_list's
 * buttons exist (their LV_EVENT_CLICKED user_data points into this array by
 * index), overwritten only by the next Scan tap, which also rebuilds the
 * list itself. Only ever touched from lvgl_port_task, same single-task-owned
 * discipline as ui_page_network.c's original. */
EXT_RAM_BSS_ATTR static wifi_prov_scan_result_t s_scan_results[UI_PAGE_NETWORK_MANAGE_SCAN_MAX];

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
static void render_scan_page(void);

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

/* Renders the current page of s_scan_results into s_scan_list (L4). */
static void render_scan_page(void)
{
    lv_obj_clean(s_scan_list);
    s_scan_page = ui_page_profile_picker_format_clamp_page(s_scan_page, (uint8_t)s_scan_count,
                                                           UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE);
    for (size_t slot = 0; slot < UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE; slot++) {
        size_t i = ui_page_profile_picker_format_row_index(s_scan_page, (uint8_t)slot,
                                                            UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE);
        if (i >= s_scan_count) break;
        /* Explicit %.*s width -- see ui_page_network.c's original comment on
         * this exact snprintf() for why (-Wformat-truncation/-Werror). */
        char text[WIFI_PROV_SSID_MAX_LEN + 16];
        snprintf(text, sizeof(text), "%.*s%s  %d dBm", WIFI_PROV_SSID_MAX_LEN, s_scan_results[i].ssid,
                 s_scan_results[i].secure ? " *" : "", (int)s_scan_results[i].rssi);
        lv_obj_t *btn = lv_button_create(s_scan_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, UI_PAGE_NETWORK_MANAGE_ROW_HEIGHT_PX);
        lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, scan_row_clicked_cb, LV_EVENT_CLICKED, &s_scan_results[i]);
        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_update_layout(btn);
        ui_theme_apply_touch_area(btn, true);
    }
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
    s_scan_count = 0;
    s_scan_page = 0;

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

    s_scan_count = count;
    s_scan_page = 0;
    render_scan_page();
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
        const char *msg = (err == ESP_ERR_NO_MEM) ? "Saved network list is full"
                          : (err == ESP_ERR_NOT_SUPPORTED && wifi_prov_saved_nets_recovery_hint())
                                ? "Saved Wi-Fi record unreadable. Factory reset scope wifi to recover"
                                : "Could not save credentials";
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

/* ---- Forget job -- 2026-09-25 LCD freeze follow-up. wifi_prov_forget_
 * network() is a plain NVS write with no radio operation, but it still
 * posts through wifi_prov_post_and_wait() and can queue behind a scan or
 * connect already in flight on the single owner task, blocking the caller
 * up to WIFI_OWNER_WAIT_MS (12s) -- the same freeze class as the saved-list
 * read this file already fixed. Moved off lvgl_port_task, same shape as
 * s_connect_job/connect_worker_task above. */
typedef struct {
    SemaphoreHandle_t lock;
    bool busy;
    bool done;
    esp_err_t err;
} forget_job_t;

static forget_job_t s_forget_job;
static char s_forget_job_ssid[WIFI_PROV_SSID_MAX_LEN + 1];

static void forget_worker_task(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_prov_forget_network(s_forget_job_ssid, strlen(s_forget_job_ssid));
    xSemaphoreTake(s_forget_job.lock, portMAX_DELAY);
    s_forget_job.err = err;
    s_forget_job.done = true;
    s_forget_job.busy = false;
    xSemaphoreGive(s_forget_job.lock);
    vTaskDelete(NULL);
}

static void apply_forget_job_result(void)
{
    if (!s_forget_job.lock) return;
    xSemaphoreTake(s_forget_job.lock, portMAX_DELAY);
    bool done = s_forget_job.done;
    esp_err_t err = s_forget_job.err;
    if (done) s_forget_job.done = false;
    xSemaphoreGive(s_forget_job.lock);
    if (!done) return;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_forget_network(%s) failed: %s", s_forget_job_ssid, esp_err_to_name(err));
    }
    refresh_saved_list();
}

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

/* Relock hook (kiln_ui.c): see ui_page_network_relock_close(). Safe before the page is built. */
void ui_page_network_manage_relock_close(void)
{
    if (!s_connect_modal) {
        return;
    }
    connect_modal_close();
    s_connect_ssid[0] = '\0';
}

static void connect_cancel_cb(lv_event_t *e)
{
    (void)e;
    connect_modal_close();
}

/* L1: wifi_prov_add_network() is ROUTE_TIER_ADMIN on the web (/provision), so
 * the LCD needs the same admin gate (open when LCD auth is off). */
static void connect_submit_apply(void *user_data)
{
    (void)user_data;
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

static void connect_submit_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Admin PIN to add network", LCD_PIN_ROLE_ADMIN, connect_submit_apply, NULL);
}

static void scan_row_open_apply(void *user_data)
{
    wifi_prov_scan_result_t *result = (wifi_prov_scan_result_t *)user_data;
    connect_modal_open(result->ssid);
}

static void scan_row_clicked_cb(lv_event_t *e)
{
    /* Gate before the operator types a password, not only at submit. */
    ui_lcd_lock_run_gated("Admin PIN to add network", LCD_PIN_ROLE_ADMIN, scan_row_open_apply,
                          lv_event_get_user_data(e));
}

/* ---- Forget confirmation ---- */

/* L1: wifi_prov_forget_network() is ROUTE_TIER_ADMIN on the web (/forget). */
static void forget_apply(void *user_data)
{
    (void)user_data;
    if (!s_forget_job.lock) {
        ESP_LOGW(TAG, "wifi_prov_forget_network(%s) not started: no job lock", s_pending_forget_ssid);
        return;
    }

    xSemaphoreTake(s_forget_job.lock, portMAX_DELAY);
    bool already_busy = s_forget_job.busy;
    if (!already_busy) {
        s_forget_job.busy = true;
        s_forget_job.done = false;
    }
    xSemaphoreGive(s_forget_job.lock);
    if (already_busy) {
        ESP_LOGW(TAG, "forget(%s) dropped: a previous forget is still running", s_pending_forget_ssid);
        return; /* one forget at a time */
    }

    snprintf(s_forget_job_ssid, sizeof(s_forget_job_ssid), "%s", s_pending_forget_ssid);

    BaseType_t created = xTaskCreate(forget_worker_task, "wifi_forget_ui", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_forget_job.lock, portMAX_DELAY);
        s_forget_job.busy = false;
        xSemaphoreGive(s_forget_job.lock);
        ESP_LOGW(TAG, "wifi_prov_forget_network(%s) failed to start worker task", s_forget_job_ssid);
    }
}

/* L10: the Forget dialog is a ui_confirm so ui_confirm_close_open() closes it
 * when the LCD session relocks (a raw msgbox would stay tappable). */
static void forget_confirm_yes_cb(void *user_data)
{
    (void)user_data;
    ui_lcd_lock_run_gated("Admin PIN to forget network", LCD_PIN_ROLE_ADMIN, forget_apply, NULL);
}

static char s_forget_body[128];

static void forget_row_clicked_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    snprintf(s_pending_forget_ssid, sizeof(s_pending_forget_ssid), "%s", ssid);

    /* Cached, non-blocking -- see refresh_saved_list()'s comment below for
     * why this page never calls wifi_prov_get_saved_networks() directly. */
    wifi_prov_saved_network_t saved[UI_PAGE_NETWORK_MANAGE_SAVED_MAX];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks_cached(saved, UI_PAGE_NETWORK_MANAGE_SAVED_MAX, &saved_count);

    if (saved_count <= 1) {
        snprintf(s_forget_body, sizeof(s_forget_body),
                 "This is the last saved network. Forgetting it will switch this board to Access Point mode. Continue?");
    } else {
        snprintf(s_forget_body, sizeof(s_forget_body), "Forget \"%s\"?", s_pending_forget_ssid);
    }
    ui_confirm_params_t p = {
        .title = "Forget network",
        .body = s_forget_body,
        .confirm_label = "Forget",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = forget_confirm_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&p);
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

/* L14: signature of what the saved list last rendered; the list is rebuilt
 * only when it changes (and only while the Saved view is showing), so a tap on
 * a row is never destroyed under the finger by a periodic rebuild. */
static uint32_t s_saved_sig;
static bool s_saved_sig_valid;

static void refresh_saved_list(void)
{
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

    s_saved_count = count;
    if (count == 0) {
        s_saved_page = 0;
    } else {
        s_saved_page = ui_page_profile_picker_format_clamp_page(s_saved_page, (uint8_t)count,
                                                               UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE);
    }
    const char *sig_active = wifi_prov_get_saved_ssid();
    bool sig_connected = wifi_prov_is_sta_connected();
    uint32_t sig = 2166136261u;
    sig = (sig ^ (uint32_t)count) * 16777619u;
    sig = (sig ^ s_saved_page) * 16777619u;
    sig = (sig ^ (sig_connected ? 1u : 0u)) * 16777619u;
    for (const char *c = sig_active ? sig_active : ""; *c; c++) sig = (sig ^ (uint8_t)*c) * 16777619u;
    for (size_t k = 0; k < count; k++) {
        sig = (sig ^ 0xFFu) * 16777619u;
        for (const char *c = saved[k].ssid; *c; c++) sig = (sig ^ (uint8_t)*c) * 16777619u;
    }
    if (s_saved_sig_valid && sig == s_saved_sig) {
        return;
    }
    s_saved_sig = sig;
    s_saved_sig_valid = true;
    lv_obj_clean(s_saved_list);
    if (count == 0) {
        lv_obj_t *none = lv_label_create(s_saved_list);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "No saved networks");
        return;
    }

    const char *active_ssid = wifi_prov_get_saved_ssid();
    bool sta_connected = wifi_prov_is_sta_connected();

    for (size_t slot = 0; slot < UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE; slot++) {
        size_t i = ui_page_profile_picker_format_row_index(s_saved_page, (uint8_t)slot,
                                                            UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE);
        if (i >= count) break;
        snprintf(ssid_ctx[i], sizeof(ssid_ctx[i]), "%s", saved[i].ssid);

        bool connected = sta_connected && strcmp(saved[i].ssid, active_ssid) == 0;
        char text[48];
        snprintf(text, sizeof(text), "%s%s", saved[i].ssid, connected ? "  (connected)" : "");

        lv_obj_t *row = lv_obj_create(s_saved_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, UI_PAGE_NETWORK_MANAGE_ROW_HEIGHT_PX);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_color(label, connected ? UI_THEME_ACCENT_4 : UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, text);

        lv_obj_t *forget_btn = lv_button_create(row);
        lv_obj_set_height(forget_btn, UI_PAGE_NETWORK_MANAGE_ROW_HEIGHT_PX - UI_THEME_PADDING_PX);
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

/* ---- Pager (L4) ---- */

static void update_pager(void)
{
    size_t count = s_list_showing_saved ? s_saved_count : s_scan_count;
    uint8_t page = s_list_showing_saved ? s_saved_page : s_scan_page;
    uint8_t pages = ui_page_profile_picker_format_page_count((uint8_t)count, UI_PAGE_NETWORK_MANAGE_ROWS_PER_PAGE);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)page + 1u, (unsigned)pages);
    lv_label_set_text(s_pager_label, buf);
    if (page == 0) lv_obj_add_state(s_pager_prev_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_pager_prev_btn, LV_STATE_DISABLED);
    if ((uint8_t)(page + 1u) >= pages) lv_obj_add_state(s_pager_next_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_pager_next_btn, LV_STATE_DISABLED);
}

static void pager_step(int step)
{
    if (s_list_showing_saved) {
        int p = (int)s_saved_page + step;
        s_saved_page = (uint8_t)(p < 0 ? 0 : p);
    } else {
        int p = (int)s_scan_page + step;
        s_scan_page = (uint8_t)(p < 0 ? 0 : p);
        render_scan_page();
    }
    refresh_cb(NULL);
}

static void pager_prev_cb(lv_event_t *e) { (void)e; pager_step(-1); }
static void pager_next_cb(lv_event_t *e) { (void)e; pager_step(+1); }

/* ---- Refresh ---- */

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    apply_scan_job_result();
    apply_connect_job_result();
    apply_forget_job_result();

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

    /* L14: rebuild only while the Saved view shows, and only on change. */
    if (s_list_showing_saved) {
        refresh_saved_list();
    }
    update_pager();
}

/* Fixed-height, non-scrolling row container (L4). */
static lv_obj_t *build_page_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, UI_PAGE_NETWORK_MANAGE_ROW_GAP_PX, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    return list;
}

static lv_obj_t *build_pager_btn(lv_obj_t *row, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, 80, UI_PAGE_NETWORK_MANAGE_PAGER_HEIGHT_PX);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, true);
    return btn;
}

static void build_pager(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, UI_PAGE_NETWORK_MANAGE_PAGER_HEIGHT_PX);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_pager_prev_btn = build_pager_btn(row, "Prev", pager_prev_cb);
    s_pager_label = lv_label_create(row);
    lv_obj_set_style_text_color(s_pager_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_pager_label, "1 / 1");
    s_pager_next_btn = build_pager_btn(row, "Next", pager_next_cb);
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
    s_forget_job.lock = xSemaphoreCreateMutex();
    if (!s_scan_job.lock || !s_connect_job.lock || !s_forget_job.lock) {
        ESP_LOGE(TAG, "ui_page_network_manage: mutex allocation failed -- Scan/connect/forget disabled");
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

    s_scan_list = build_page_list(content);

    s_saved_title = lv_label_create(content);
    lv_obj_set_style_text_color(s_saved_title, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_saved_title, "Saved networks:");

    s_saved_list = build_page_list(content);
    s_saved_sig_valid = false; /* new list object: force the first fill (L14) */

    build_pager(content);

    ui_topbar_raise(&tb);

    /* Connect modal built last so it's topmost in z-order while shown --
     * same reasoning as ui_page_network.c's original build_connect_modal()
     * call site. */
    build_connect_modal(scr);

    lv_timer_create(refresh_cb, UI_PAGE_NETWORK_MANAGE_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real state immediately instead of waiting one tick */

    return scr;
}
