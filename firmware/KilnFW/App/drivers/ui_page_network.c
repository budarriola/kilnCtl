#include "ui_page_network.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiln_ui.h"
#include "ui_theme.h"
#include "wifi_prov.h"
#include "wifi_status_ui.h"

// TODO.md 10.9's LCD-side network settings page, linked from
// ui_page_config.c's "Network / Wi-Fi" nav item (previously a "not built
// yet" placeholder row -- see ui_page_config.c's header comment). Mirrors
// what wifi_provision_page.html already does on the web, per 10.9's own
// bullet list. Per TODO.md 10.1a's shared-backend rule, every control here
// calls the exact same wifi_prov.h getters/setters wifi_provision_http.c's
// handlers already call -- no parallel read of wifi_prov.c's internals:
//   - Mode + state readout: wifi_status_ui_get_text() (shared with
//     ui_page_home.c's status bar, TODO.md 10.9's explicit ask -- see
//     wifi_status_ui.c) plus wifi_prov_get_mode()/_get_state()/
//     _is_sta_connected()/_get_sta_rssi() directly for the parts that
//     formatter doesn't expose (RSSI, AP client count).
//   - Scan: wifi_prov_scan() -- same call scan_get_handler() makes, same
//     ESP_ERR_NOT_SUPPORTED-in-AP-mode refusal shown as a status message
//     rather than a silent empty list.
//   - Saved networks: wifi_prov_get_saved_networks()/_forget() -- same
//     calls networks_get_handler()/forget_post_handler() make.
//   - AP identity: wifi_prov_get_ap_ssid()/_get_ap_password() -- same
//     values status_get_handler() already exposes on the web status JSON.
//   - Add-network: wifi_prov_add_network() -- same call
//     provision_post_handler() makes for a plain ssid/password POST body.
//   - Mode switch: wifi_prov_set_mode() -- same call provision_post_handler()
//     makes for a "mode" POST body.
//
// QR codes (TODO.md 10.9's second/third bullets): LV_USE_QRCODE was off in
// this project's LVGL Kconfig (confirmed via sdkconfig -- "# CONFIG_LV_USE_
// QRCODE is not set") and is now flipped on (sdkconfig is gitignored/
// per-checkout generated in this project, no sdkconfig.defaults exists to
// carry the flip forward -- see this pass's status note in TODO.md 10.9 for
// what that means for a fresh clone). Two mutually-exclusive QR states,
// matching the AP-vs-STA-connected split the rest of this page already makes:
//   - AP/AP-fallback (wifi_prov_get_mode() == WIFI_PROV_MODE_AP or
//     wifi_prov_get_state() == WIFI_PROV_STATE_UNPROVISIONED/AP_MODE):
//     WIFI:T:WPA;S:<ap_ssid>;P:<ap_password>;; -- both iOS and Android
//     camera apps parse this natively, no app needed.
//   - sta_connected: http://kiln.local (mDNS hostname read the same way
//     wifi_status_ui.c does) plus a second QR for the raw IP, since mDNS
//     isn't reliable on every phone/network.
// Neither lv_qrcode_update() call runs on every UI_PAGE_NETWORK_REFRESH_MS
// tick -- each is gated on the underlying string actually having changed
// (s_ap_qr_last/s_dashboard_qr_last/s_ip_qr_last), since re-encoding a QR
// code is real work the panel doesn't need to repeat every second for data
// that changes on the order of minutes, if ever, during one boot.
//
// Known gap, documented rather than silently missing (TODO.md 10.9's own
// "needs its own design pass" note): the board's own AP identity
// (SSID/password) is READ-ONLY on this page. 10.9's bullet list only asks
// for AP identity "display", not an edit form, and unlike the scan-then-
// connect flow (SSID pre-filled from the tap, only the password needs a
// keyboard) editing the AP's own SSID would need free-text SSID entry with
// no scan result to pre-fill it from -- a second keyboard flow this pass
// does not build. wifi_provision_page.html remains the only way to change
// the board's own AP identity.
// 2026-08-18 no-scroll pass: `scr`/`content`/the connect modal below
// explicitly clear LV_OBJ_FLAG_SCROLLABLE, and several rows/buttons were
// shrunk (mode toggle and Scan button from UI_THEME_MIN_TOUCH_TARGET_PX/72px
// to 44px, Back from 72px to 36px) against this page's ~264px content
// budget (480x320 landscape -- see ui_page_home.c's header comment for that
// number's derivation).
//
// 2026-08-19 freeze/back-button fix: two bugs reported from the bench.
// (1) "No visible back button": this page's Back button was the last child
// of the variable-height `content` column, same as every other page -- but
// unlike those, this page's content is close enough to its no-scroll budget
// (see the follow-up-pass note below, "~268px against ~264px, computed not
// measured") that in the worst-case state (list block visible, one of
// Scan/Saved populated) Back gets pushed past the bottom of the fixed-height
// screen with page-level scrolling deliberately disabled -- there's no way
// to reach it. Fixed by moving Back into the fixed-height title `bar`
// instead of the variable-height `content` column: it's now always on
// screen regardless of how tall the content below happens to be, and it
// also gets content's budget back (one less 36-44px row to fit).
// (2) "Freezes often": scan_btn_cb() used to call wifi_prov_scan() directly,
// which calls esp_wifi_scan_start(..., block=true) -- a genuinely blocking
// full-channel scan that wifi_prov.c's own header comment (see "CAUTION"
// above wifi_prov_scan()'s definition) already flags as slow. Every lv_*
// call in this whole firmware runs on the single lvgl_port_task
// (lvgl_port.c's header comment), so blocking inside an LV_EVENT_CLICKED
// handler blocks lv_timer_handler() itself -- no redraw, no flush, no touch
// input, on ANY page, for however long the scan takes (can be seconds).
// That is the freeze, not a data race in the usual sense (corrupted state) --
// the whole UI task is simply not running. Fixed by moving the actual
// wifi_prov_scan() call onto a short-lived worker task; scan_btn_cb only
// starts it and returns immediately, and refresh_cb (already polled every
// UI_PAGE_NETWORK_REFRESH_MS) picks up the result once the worker posts it.
// A small mutex (s_scan_job.lock) guards the handoff since the worker task
// and lvgl_port_task now genuinely run concurrently -- see scan_worker_task()
// below.
// Known, NOT fixed here: wifi_prov.c's own module state (s_wifi) has no
// lock at all -- every wifi_prov_get_*() call this page makes from
// lvgl_port_task races against the Wi-Fi driver's event-loop task, which is
// the writer. Scalars mostly read/write atomically on this hardware, so
// this has not been observed to corrupt anything, but it's not proven safe
// either -- flagged as a real follow-up in TODO.md rather than silently
// left unmentioned. Fixing it properly is a wifi_prov.c-wide change (every
// getter and every event-handler write), out of scope for this pass, which
// targets the one call site that reliably freezes the display.
//
// 2026-08-18 follow-up pass: the first no-scroll pass left Scan and Saved
// Networks BOTH stacked at once (90px lists + titles + Scan button/status),
// which measured out to ~300-350px on its own -- a real overflow even
// before the STA-connected QR row was added on top. Fixed here by making
// Scan/Saved mutually exclusive (a small toggle row picks exactly one list
// to show at a time, default "Saved") and, once connected, hiding the whole
// scan/saved section behind a "Change network" button so the QR row (the
// thing actually useful once connected) gets the space instead -- tapping
// it swaps back to the list view via the same s_manage_open flag, with a
// "Show QR" button to swap back. `s_scan_list`/`s_saved_list` remain
// internally touch-drag-scrollable at their fixed height (this is fine and
// intended -- a bounded, self-contained list scrolling itself is not "the
// page scrolling"; only page-level containers must stay fixed).
// Worst case at the time, with Back still living at the bottom of
// `content`: status_card(~40) + mode_row(44) + toggle_row(40) + one list
// block (title ~18 + list 70 = ~88) + Back(36) = ~248px of content plus
// ~20px of inter-item gaps ~= 268px against the ~264px budget -- already
// over. The 2026-08-19 pass above moved Back into the fixed-height title
// bar specifically because this arithmetic never had margin to spare;
// content's worst case is now the same sum minus Back and its gap, ~228px,
// with real margin against the ~264px budget for the first time. Still
// NOT verified against real hardware (no ILI9488 panel attached in this
// environment) -- this is arithmetic against ui_theme.h's real constants,
// not a hardware-confirmed fit.
//
// 2026-08-19 budget-fix pass (UI_PLAN.md section 3, LCD item 1): status_card
// used to hold two label rows -- status_label ("WiFi: --" style text) and
// detail_label (RSSI/client-count) -- contributing to the "~40px" figure the
// paragraph above cites for status_card. Combined onto one label/one line
// (see "Top status readout" comment on s_status_label's declaration and
// refresh_cb()'s combined_buf build), which removes one montserrat_14 line
// height (~20px, this file's own established per-line estimate) from
// status_card. Re-summing the ~228px content-only figure from the paragraph
// above: status_card(~40 -> ~20) + mode_row(44) + toggle_row(40) + one list
// block (title ~18 + list 70 = ~88) = ~192px of content plus ~15px of
// inter-item gaps (one fewer row means one fewer gap) =~ 207px against the
// ~264px content budget, real margin rather than barely under. Still NOT
// verified against real hardware, same caveat as the paragraph above.
static const char *TAG __attribute__((unused)) = "ui_page_network";

#define UI_PAGE_NETWORK_REFRESH_MS 1000
#define UI_PAGE_NETWORK_SCAN_MAX 20
#define UI_PAGE_NETWORK_SAVED_MAX 8
#define UI_PAGE_NETWORK_QR_SIZE_PX 100 /* was 140 -- shrunk in the 2026-08-18 no-scroll pass, see this file's header comment */

/* ---- Top status readout ----
 * 2026-08-19 budget-fix pass: status_label and detail_label used to be two
 * separate label rows inside status_card (see this file's "2026-08-18
 * follow-up pass" comment's ~268px arithmetic, which counted status_card at
 * ~40px for exactly that reason -- two montserrat_14 lines). Combined into
 * one label/one line here to claw back the missing ~20px of budget margin;
 * s_detail_label no longer exists as a separate widget, its text is appended
 * onto s_status_label's single line instead (see refresh_cb()'s combined-
 * text build below). */
static lv_obj_t *s_status_label;

/* ---- Mode toggle ---- */
static lv_obj_t *s_mode_home_btn;
static lv_obj_t *s_mode_ap_btn;

/* ---- Home-mode section: scan + saved networks + STA-connected QRs ----
 * Scan and Saved are mutually exclusive (s_list_showing_saved picks which
 * one is visible) and, once connected, the whole list block hides behind
 * s_manage_open in favor of the QR row -- see this file's header comment
 * for the height arithmetic this exists to satisfy. */
static lv_obj_t *s_home_section;
static lv_obj_t *s_list_toggle_row;
static lv_obj_t *s_scan_toggle_btn;
static lv_obj_t *s_saved_toggle_btn;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_scan_status_label;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_saved_title;
static lv_obj_t *s_saved_list;
static lv_obj_t *s_manage_btn;
static lv_obj_t *s_manage_btn_label;
static lv_obj_t *s_sta_qr_row;
static lv_obj_t *s_dashboard_qr;
static lv_obj_t *s_dashboard_qr_caption;
static lv_obj_t *s_ip_qr;
static lv_obj_t *s_ip_qr_caption;
static char s_dashboard_qr_last[80];
static char s_ip_qr_last[40];
static bool s_list_showing_saved = true;  /* which of Scan/Saved is visible */
static bool s_manage_open = false;        /* connected-mode: list view vs QR view */

/* Last wifi_prov_scan() results -- kept alive as long as s_scan_list's
 * buttons exist (their LV_EVENT_CLICKED user_data points into this array by
 * index), overwritten only by the next Scan tap, which also rebuilds the
 * list itself. Only ever touched from lvgl_port_task (written by
 * apply_scan_job_result(), read by scan_row_clicked_cb()) -- the worker task
 * below writes into s_scan_job.results, a separate buffer, precisely so this
 * one stays single-task-owned like every other LVGL-facing static here. */
static wifi_prov_scan_result_t s_scan_results[UI_PAGE_NETWORK_SCAN_MAX];

/* ---- Async scan job -- see this file's 2026-08-19 header comment ----
 * scan_worker_task runs wifi_prov_scan() (a real blocking radio scan) off
 * lvgl_port_task, so a Scan tap can no longer freeze the whole display.
 * `lock` is the only thing shared between that task and lvgl_port_task;
 * everything it guards is small and copied out promptly on either side.
 * `lock` is created once, up front, in ui_page_network_build() -- NOT
 * lazily on first Scan tap the way an earlier version of this pass had it:
 * refresh_cb() (and therefore apply_scan_job_result()) runs from the very
 * first tick, before any button has ever been tapped, and
 * xSemaphoreTake(NULL, ...) on a not-yet-created mutex is undefined
 * behavior -- found and fixed same-session, before it ever reached
 * hardware. */
typedef struct {
    SemaphoreHandle_t lock;
    bool busy;              /* worker task is running */
    bool done;               /* worker finished, result below is ready to consume */
    esp_err_t err;
    size_t count;
    wifi_prov_scan_result_t results[UI_PAGE_NETWORK_SCAN_MAX];
} scan_job_t;

static scan_job_t s_scan_job;

/* ---- Async mode-switch job -- same shape/reasoning as scan_job_t above.
 * mode_home_btn_cb()/mode_ap_btn_cb() used to call wifi_prov_set_mode()
 * directly on lvgl_port_task; it does an NVS write and an esp_wifi_set_mode()
 * radio-mode change, either of which is a real (if usually brief) block on
 * the one task that also owns every page's redraw. No "done" flag is needed
 * here -- refresh_cb() already polls wifi_prov_get_mode() every tick
 * regardless, so the eventual result shows up on its own within
 * UI_PAGE_NETWORK_REFRESH_MS of the worker finishing; `busy` alone is enough
 * to stop a second tap from piling up a second worker task. */
typedef struct {
    SemaphoreHandle_t lock;
    bool busy;
} mode_job_t;

static mode_job_t s_mode_job;

static void mode_worker_task(void *arg)
{
    wifi_prov_mode_t mode = (wifi_prov_mode_t)(intptr_t)arg;
    esp_err_t err = wifi_prov_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_set_mode(%d) failed: %s", (int)mode, esp_err_to_name(err));
    }
    xSemaphoreTake(s_mode_job.lock, portMAX_DELAY);
    s_mode_job.busy = false;
    xSemaphoreGive(s_mode_job.lock);
    vTaskDelete(NULL);
}

static void request_mode_change(wifi_prov_mode_t mode)
{
    if (!s_mode_job.lock) return;
    xSemaphoreTake(s_mode_job.lock, portMAX_DELAY);
    bool already_busy = s_mode_job.busy;
    if (!already_busy) s_mode_job.busy = true;
    xSemaphoreGive(s_mode_job.lock);
    if (already_busy) return; /* one mode switch at a time */

    BaseType_t created =
        xTaskCreate(mode_worker_task, "wifi_mode_ui", 4096, (void *)(intptr_t)mode, 5, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_mode_job.lock, portMAX_DELAY);
        s_mode_job.busy = false;
        xSemaphoreGive(s_mode_job.lock);
    }
}

/* ---- Async connect (add-network) job -- same shape again. connect_submit_cb()
 * used to call wifi_prov_add_network() directly on lvgl_port_task; that
 * function can itself trigger a blocking wifi_prov_scan() internally
 * (select_and_apply_join_candidate()'s auto-join tie-break, see wifi_prov.c),
 * so this was a second real path to the exact freeze the Scan button had.
 * Unlike mode-switch, the connect modal needs a real result back (close on
 * success, show an error message on failure), so this one keeps a done/err
 * pair like scan_job_t. ssid/password are copied into module statics before
 * the worker task starts, while still on lvgl_port_task -- the worker never
 * touches an LVGL widget. */
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

static void scan_row_clicked_cb(lv_event_t *e);

static void scan_worker_task(void *arg)
{
    (void)arg;
    esp_err_t err;
    size_t count = 0;
    wifi_prov_scan_result_t results[UI_PAGE_NETWORK_SCAN_MAX];

    err = wifi_prov_scan(results, UI_PAGE_NETWORK_SCAN_MAX, &count);

    xSemaphoreTake(s_scan_job.lock, portMAX_DELAY);
    s_scan_job.err = err;
    s_scan_job.count = count;
    memcpy(s_scan_job.results, results, count * sizeof(results[0]));
    s_scan_job.done = true;
    s_scan_job.busy = false;
    xSemaphoreGive(s_scan_job.lock);

    vTaskDelete(NULL);
}

/* Applies a completed scan_worker_task result to the visible list --
 * called from refresh_cb() on lvgl_port_task once s_scan_job.done is seen,
 * so every lv_* call here still obeys the single-task rule. */
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
        /* Explicit %.*s width (WIFI_PROV_SSID_MAX_LEN, the field's real max)
         * rather than a bare %s -- GCC's -Wformat-truncation can't otherwise
         * bound ssid's contribution and assumes worst-case, warning under
         * -Werror even though the field is fixed-size (found building
         * 2026-08-18). */
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
 * Built once, hidden, matching every other page's "pages are never torn
 * down" widget lifetime (kiln_ui.h's header comment) -- this is a full-page
 * overlay, not a separate kiln_ui page, since it only ever makes sense on
 * top of this one. Declared here, ahead of build, rather than down with the
 * other AP-mode/connect-modal statics below -- apply_connect_job_result()
 * just below needs s_connect_status_label and this is its first use in the
 * file. */
static lv_obj_t *s_connect_modal;
static lv_obj_t *s_connect_title;
static lv_obj_t *s_connect_ta;
static lv_obj_t *s_connect_status_label;
static lv_obj_t *s_connect_kb;
static char s_connect_ssid[WIFI_PROV_SSID_MAX_LEN + 1];

/* Applies a completed connect_worker_task result -- same "poll from
 * refresh_cb()" shape as apply_scan_job_result() above. Forward-declares
 * connect_modal_close()/refresh_saved_list() since both are defined later in
 * this file (connect_modal_close() with the rest of the modal build code,
 * refresh_saved_list() with the rest of the saved-list code). */
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

/* ---- AP-mode section: identity display + AP-join QR ---- */
static lv_obj_t *s_ap_section;
static lv_obj_t *s_ap_ssid_label;
static lv_obj_t *s_ap_password_label;
static lv_obj_t *s_ap_qr;
static char s_ap_qr_last[16 + WIFI_PROV_SSID_MAX_LEN + WIFI_PROV_PASSWORD_MAX_LEN];

/* ---- Pending-forget context, for the confirm msgbox's footer button cb.
 * Only one confirm dialog can be open at a time, so a single static buffer
 * is enough -- set right before lv_msgbox_create() below. */
static char s_pending_forget_ssid[WIFI_PROV_SSID_MAX_LEN + 1];

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("config");
}

static void refresh_saved_list(void);
static void refresh_cb(lv_timer_t *timer);

/* ---- Mode toggle ---- */

static void apply_mode_button_style(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
}

static void mode_home_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Async now -- see mode_job_t's header comment. The mode buttons no
     * longer repaint themselves immediately (there is nothing confirmed yet
     * to repaint); refresh_cb()'s normal 1s poll picks up the real mode
     * once wifi_prov_set_mode() actually finishes. */
    request_mode_change(WIFI_PROV_MODE_HOME);
}

static void mode_ap_btn_cb(lv_event_t *e)
{
    (void)e;
    request_mode_change(WIFI_PROV_MODE_AP);
}

/* ---- Scan/Saved toggle (mutually exclusive, see this file's header
 * comment on the height budget this satisfies) ---- */

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

/* ---- Connected-mode: list view vs QR view (mutually exclusive) ---- */

static void manage_btn_cb(lv_event_t *e)
{
    (void)e;
    s_manage_open = !s_manage_open;
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

    /* Async now -- see connect_job_t's header comment. Same
     * wifi_prov_add_network() call provision_post_handler() makes for a
     * plain ssid/password POST body (TODO.md 10.1a), just off
     * lvgl_port_task; apply_connect_job_result() (polled from refresh_cb())
     * closes the modal or shows the error once the worker task finishes. */
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
    /* Deliberately left synchronous, unlike scan/mode/connect above --
     * wifi_prov_forget_network() only does an NVS list write, no radio
     * operation and no internal scan, so its worst case is a brief flash
     * write rather than seconds of blocking. Flagged in TODO.md 10.14 as
     * the one wifi_prov.c call site on this page not yet moved off
     * lvgl_port_task, not silently left inconsistent. */
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

    /* Fresh read, not a cached count -- same "don't trust stale widget
     * state" discipline as ui_page_temperature.c's relay_toggle_cb(). Only
     * the count is needed here, so a small on-stack scratch array is fine. */
    wifi_prov_saved_network_t saved[UI_PAGE_NETWORK_SAVED_MAX];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks(saved, UI_PAGE_NETWORK_SAVED_MAX, &saved_count);

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

    /* Same wifi_prov_scan() call scan_get_handler() makes -- TODO.md
     * 10.1a -- but off lvgl_port_task now (see this file's 2026-08-19
     * header comment): a blocking radio scan on the LVGL task freezes the
     * whole display for however long it takes. apply_scan_job_result(),
     * polled from refresh_cb(), picks the result up once it's ready. */
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

    static wifi_prov_saved_network_t saved[UI_PAGE_NETWORK_SAVED_MAX];
    /* Copies of each SSID that outlive this function -- the forget button's
     * event user_data points into this array, and it must still be valid
     * the next time the operator taps Forget, arbitrarily long after this
     * call returns (pages are never torn down, kiln_ui.h's header
     * comment). Overwritten in place on every rebuild, which is fine: a
     * button whose row no longer exists can't be tapped again. */
    static char ssid_ctx[UI_PAGE_NETWORK_SAVED_MAX][WIFI_PROV_SSID_MAX_LEN + 1];
    size_t count = 0;
    wifi_prov_get_saved_networks(saved, UI_PAGE_NETWORK_SAVED_MAX, &count);

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

/* ---- QR helpers -- gated on the underlying string actually changing, see
 * this file's header comment. ---- */

static void update_qr_if_changed(lv_obj_t *qr, char *last, size_t last_cap, const char *new_data)
{
    if (strcmp(last, new_data) == 0) {
        return; /* unchanged since the last encode -- don't redo the work */
    }
    snprintf(last, last_cap, "%s", new_data);
    lv_qrcode_update(qr, new_data, strlen(new_data));
}

/* ---- Refresh ---- */

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    apply_scan_job_result();
    apply_connect_job_result();

    char status_buf[64];
    wifi_status_ui_get_text(status_buf, sizeof(status_buf));
    /* Not painted yet -- combined with detail_buf below (see this file's
     * "Top status readout" header comment) and set on s_status_label once,
     * after detail_buf is filled in by whichever of the sta_connected/AP/
     * neither branches below applies. */
    char detail_buf[32] = "";

    wifi_prov_mode_t mode = wifi_prov_get_mode();
    wifi_prov_state_t state = wifi_prov_get_state();
    bool sta_connected = wifi_prov_is_sta_connected();

    apply_mode_button_style(s_mode_home_btn, mode != WIFI_PROV_MODE_AP);
    apply_mode_button_style(s_mode_ap_btn, mode == WIFI_PROV_MODE_AP);

    /* TODO.md 10.9's exact AP-QR condition -- see this file's header
     * comment for why WIFI_PROV_STATE_AP_MODE is listed even though it only
     * occurs when mode == WIFI_PROV_MODE_AP already (kept for fidelity to
     * the plan's own wording rather than simplified away). */
    bool ap_active = (mode == WIFI_PROV_MODE_AP) || (state == WIFI_PROV_STATE_UNPROVISIONED) ||
                     (state == WIFI_PROV_STATE_AP_MODE);

    if (mode == WIFI_PROV_MODE_AP) {
        lv_obj_add_flag(s_home_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ap_section, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_home_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ap_section, LV_OBJ_FLAG_HIDDEN);
    }

    /* List block (Scan/Saved toggle + whichever list) vs QR row: mutually
     * exclusive once connected, since both stacked at once is exactly the
     * overflow this pass fixed (see header comment). Not connected -> QR
     * row has nothing useful to show anyway, so the list block always wins.
     * s_manage_open only has an effect while connected. */
    bool show_list_block = !sta_connected || s_manage_open;
    bool show_qr_row_slot = sta_connected && !s_manage_open;

    if (show_list_block) {
        lv_obj_remove_flag(s_list_toggle_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_scan_status_label, LV_OBJ_FLAG_HIDDEN);
        if (s_list_showing_saved) {
            lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_saved_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_saved_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_scan_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_saved_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_saved_list, LV_OBJ_FLAG_HIDDEN);
        }
        apply_mode_button_style(s_scan_toggle_btn, !s_list_showing_saved);
        apply_mode_button_style(s_saved_toggle_btn, s_list_showing_saved);
    } else {
        lv_obj_add_flag(s_list_toggle_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_saved_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_saved_list, LV_OBJ_FLAG_HIDDEN);
    }

    /* Manage button: only meaningful (and only shown) while connected --
     * not connected always shows the list block with nothing to toggle. */
    if (sta_connected) {
        lv_obj_remove_flag(s_manage_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_manage_btn_label, s_manage_open ? "Show QR" : "Change network");
    } else {
        lv_obj_add_flag(s_manage_btn, LV_OBJ_FLAG_HIDDEN);
        s_manage_open = false; /* reset so reconnecting later defaults to the QR view */
    }

    if (sta_connected) {
        int8_t rssi = wifi_prov_get_sta_rssi();
        snprintf(detail_buf, sizeof(detail_buf), "Signal: %d dBm", (int)rssi);
        if (show_qr_row_slot) {
            lv_obj_remove_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);
        }

        char ip[16];
        if (wifi_prov_get_sta_ip(ip, sizeof(ip)) != ESP_OK) {
            ip[0] = '\0';
        }
        char mdns_host[MDNS_NAME_BUF_LEN];
        char dashboard_url[80];
        if (mdns_hostname_get(mdns_host) == ESP_OK) {
            snprintf(dashboard_url, sizeof(dashboard_url), "http://%s.local", mdns_host);
        } else {
            snprintf(dashboard_url, sizeof(dashboard_url), "http://kiln.local");
        }
        update_qr_if_changed(s_dashboard_qr, s_dashboard_qr_last, sizeof(s_dashboard_qr_last), dashboard_url);
        lv_label_set_text(s_dashboard_qr_caption, dashboard_url);

        char ip_url[40];
        snprintf(ip_url, sizeof(ip_url), "http://%s", ip[0] ? ip : "?");
        update_qr_if_changed(s_ip_qr, s_ip_qr_last, sizeof(s_ip_qr_last), ip_url);
        lv_label_set_text(s_ip_qr_caption, ip_url);
    } else if (mode == WIFI_PROV_MODE_AP) {
        uint8_t clients = wifi_prov_get_ap_client_count();
        snprintf(detail_buf, sizeof(detail_buf), "Clients: %u", (unsigned)clients);
        lv_obj_add_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Single combined line -- see this file's "Top status readout" header
     * comment. "%s -- %s" rather than two lines/labels; skip the separator
     * entirely when detail_buf is empty (neither connected nor AP-active
     * yet) so the line doesn't end in a dangling "--". */
    /* sizeof(status_buf)-1 (63) + " -- " (4) + sizeof(detail_buf)-1 (31) + NUL
     * = 99 worst case -- 104 leaves headroom so GCC's -Wformat-truncation
     * can prove the %.*s-bounded snprintf() below never truncates. */
    char combined_buf[104];
    if (detail_buf[0]) {
        /* Explicit %.*s widths (sizeof(status_buf)/detail_buf minus 1, their
         * real max) rather than bare %s -- GCC's -Wformat-truncation can't
         * otherwise bound either argument's contribution and assumes
         * worst-case, warning under -Werror even though combined_buf is
         * sized to comfortably fit both fixed-size buffers plus the " -- "
         * separator (same fix apply_scan_job_result() already uses above
         * for the same reason). */
        snprintf(combined_buf, sizeof(combined_buf), "%.*s -- %.*s", (int)sizeof(status_buf) - 1, status_buf,
                 (int)sizeof(detail_buf) - 1, detail_buf);
    } else {
        snprintf(combined_buf, sizeof(combined_buf), "%.*s", (int)sizeof(status_buf) - 1, status_buf);
    }
    lv_label_set_text(s_status_label, combined_buf);

    if (ap_active) {
        const char *ap_ssid = wifi_prov_get_ap_ssid();
        const char *ap_password = wifi_prov_get_ap_password();
        lv_label_set_text(s_ap_ssid_label, ap_ssid);
        lv_label_set_text(s_ap_password_label, ap_password[0] ? ap_password : "(open network)");

        char uri[sizeof(s_ap_qr_last)];
        /* WIFI:T:WPA;S:<ssid>;P:<password>;; -- password may be empty per
         * an open AP, T:nopass is not used since section 1's AP password is
         * never optional today (see this file's header comment). */
        snprintf(uri, sizeof(uri), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_password);
        update_qr_if_changed(s_ap_qr, s_ap_qr_last, sizeof(s_ap_qr_last), uri);
    }

    /* Saved-network list churn is cheap (<=8 rows) and this rebuild keeps
     * the "connected" highlight and any change made from the web page in
     * sync without a separate diff -- unlike the QR codes above, re-drawing
     * a handful of list rows is not expensive enough to need gating. */
    refresh_saved_list();
}

/* ---- Build ---- */

static lv_obj_t *build_ap_stat_row(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, name);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_color(value, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(value, "--");

    return value;
}

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

lv_obj_t *ui_page_network_build(void)
{
    /* Created up front, before refresh_cb() ever runs -- see scan_job_t's
     * header comment for why lazy creation on first tap was a bug (the
     * poll functions run from the very first tick, before any tap). Pages
     * are built exactly once (kiln_ui.h's header comment), so this never
     * runs twice. */
    s_scan_job.lock = xSemaphoreCreateMutex();
    s_mode_job.lock = xSemaphoreCreateMutex();
    s_connect_job.lock = xSemaphoreCreateMutex();
    if (!s_scan_job.lock || !s_mode_job.lock || !s_connect_job.lock) {
        /* Never observed to actually fail (three tiny allocations, this
         * late in boot) -- but per this codebase's "never crash on a
         * resource failure" convention (see wifi_prov.h's header comment),
         * every job-entry point below checks its own lock for NULL and
         * no-ops rather than trusting this always succeeds. */
        ESP_LOGE(TAG, "ui_page_network: mutex allocation failed -- Scan/mode/connect disabled");
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Network / Wi-Fi");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* Back, in the fixed-height bar rather than at the bottom of the
     * variable-height content column below -- see this file's 2026-08-19
     * header comment. Always on screen regardless of how tall content gets. */
    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* Status card. */
    lv_obj_t *status_card = lv_obj_create(content);
    lv_obj_set_width(status_card, lv_pct(100));
    lv_obj_set_height(status_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(status_card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(status_card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(status_card);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_status_label, "WiFi: --");

    /* Mode toggle. */
    lv_obj_t *mode_row = lv_obj_create(content);
    lv_obj_set_width(mode_row, lv_pct(100));
    lv_obj_set_height(mode_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(mode_row, UI_THEME_PADDING_PX, 0);

    s_mode_home_btn = lv_button_create(mode_row);
    lv_obj_set_height(s_mode_home_btn, 44); /* see this file's no-scroll-pass header comment */
    lv_obj_set_flex_grow(s_mode_home_btn, 1);
    lv_obj_set_style_radius(s_mode_home_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Visual affordance for the 44px-drawn/72px-effective touch target below
     * (UI_PLAN.md section 3, LCD item 3): ui_theme_apply_touch_area() extends
     * the real hit area invisibly, which by itself lets a shrunk button
     * mislead a user about where they can tap. A subtle 1px border (dimmed,
     * not a theme-color change) is the visual half of that story. */
    lv_obj_set_style_border_width(s_mode_home_btn, 1, 0);
    lv_obj_set_style_border_color(s_mode_home_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_mode_home_btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(s_mode_home_btn, mode_home_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(s_mode_home_btn);
    lv_obj_set_style_text_color(home_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(home_label, "Home Wi-Fi");
    lv_obj_center(home_label);
    lv_obj_update_layout(s_mode_home_btn);
    ui_theme_apply_touch_area(s_mode_home_btn, false);

    s_mode_ap_btn = lv_button_create(mode_row);
    lv_obj_set_height(s_mode_ap_btn, 44);
    lv_obj_set_flex_grow(s_mode_ap_btn, 1);
    lv_obj_set_style_radius(s_mode_ap_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Same shrunk-target affordance as s_mode_home_btn above. */
    lv_obj_set_style_border_width(s_mode_ap_btn, 1, 0);
    lv_obj_set_style_border_color(s_mode_ap_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_mode_ap_btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(s_mode_ap_btn, mode_ap_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ap_label = lv_label_create(s_mode_ap_btn);
    lv_obj_set_style_text_color(ap_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(ap_label, "Access Point");
    lv_obj_center(ap_label);
    lv_obj_update_layout(s_mode_ap_btn);
    ui_theme_apply_touch_area(s_mode_ap_btn, false);

    /* Home-mode section: scan + saved networks + STA-connected QRs. */
    s_home_section = lv_obj_create(content);
    lv_obj_set_width(s_home_section, lv_pct(100));
    lv_obj_set_height(s_home_section, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_home_section, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_section, 0, 0);
    lv_obj_set_style_pad_all(s_home_section, 0, 0);
    lv_obj_set_flex_flow(s_home_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_home_section, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_home_section, LV_OBJ_FLAG_SCROLLABLE);

    /* Scan/Saved toggle -- mutually exclusive lists, see this file's header
     * comment on the height budget this satisfies. */
    s_list_toggle_row = lv_obj_create(s_home_section);
    lv_obj_set_width(s_list_toggle_row, lv_pct(100));
    lv_obj_set_height(s_list_toggle_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_list_toggle_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_toggle_row, 0, 0);
    lv_obj_set_style_pad_all(s_list_toggle_row, 0, 0);
    lv_obj_set_flex_flow(s_list_toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(s_list_toggle_row, UI_THEME_PADDING_PX, 0);

    s_scan_toggle_btn = lv_button_create(s_list_toggle_row);
    lv_obj_set_height(s_scan_toggle_btn, 36);
    lv_obj_set_flex_grow(s_scan_toggle_btn, 1);
    lv_obj_set_style_radius(s_scan_toggle_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Same shrunk-target affordance as s_mode_home_btn above. */
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

    s_saved_toggle_btn = lv_button_create(s_list_toggle_row);
    lv_obj_set_height(s_saved_toggle_btn, 36);
    lv_obj_set_flex_grow(s_saved_toggle_btn, 1);
    lv_obj_set_style_radius(s_saved_toggle_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Same shrunk-target affordance as s_mode_home_btn above. */
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

    s_scan_btn = lv_button_create(s_home_section);
    lv_obj_set_width(s_scan_btn, lv_pct(100));
    lv_obj_set_height(s_scan_btn, 44);
    lv_obj_set_style_bg_color(s_scan_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_scan_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Same shrunk-target affordance as s_mode_home_btn above -- this is the
     * "Scan button" the file's own header comment names as shrunk to 44px. */
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

    s_scan_status_label = lv_label_create(s_home_section);
    lv_obj_set_style_text_color(s_scan_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_scan_status_label, "Tap Scan to search for networks");

    /* 70px, not 90px -- shrunk further in the Scan/Saved toggle pass since
     * only one of scan_list/saved_list is ever visible at once now, but the
     * page-level content budget is still tight (see header comment). */
    s_scan_list = lv_list_create(s_home_section);
    lv_obj_set_width(s_scan_list, lv_pct(100));
    lv_obj_set_height(s_scan_list, 70);

    s_saved_title = lv_label_create(s_home_section);
    lv_obj_set_style_text_color(s_saved_title, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_saved_title, "Saved networks:");

    s_saved_list = lv_list_create(s_home_section);
    lv_obj_set_width(s_saved_list, lv_pct(100));
    lv_obj_set_height(s_saved_list, 70);

    /* STA-connected QR row -- dashboard (kiln.local) and raw-IP QRs, hidden
     * until sta_connected (this file's header comment). */
    s_sta_qr_row = lv_obj_create(s_home_section);
    lv_obj_add_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_sta_qr_row, lv_pct(100));
    lv_obj_set_height(s_sta_qr_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_sta_qr_row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_sta_qr_row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_sta_qr_row, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_sta_qr_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_sta_qr_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_sta_qr_row, UI_THEME_PADDING_PX, 0);

    lv_obj_t *dash_col = lv_obj_create(s_sta_qr_row);
    lv_obj_set_size(dash_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dash_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dash_col, 0, 0);
    lv_obj_set_style_pad_all(dash_col, 0, 0);
    lv_obj_set_flex_flow(dash_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dash_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dash_col, UI_THEME_PADDING_PX / 4, 0);
    s_dashboard_qr = lv_qrcode_create(dash_col);
    lv_qrcode_set_size(s_dashboard_qr, UI_PAGE_NETWORK_QR_SIZE_PX);
    s_dashboard_qr_caption = lv_label_create(dash_col);
    lv_obj_set_style_text_color(s_dashboard_qr_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_dashboard_qr_caption, "http://kiln.local");
    s_dashboard_qr_last[0] = '\0';

    lv_obj_t *ip_col = lv_obj_create(s_sta_qr_row);
    lv_obj_set_size(ip_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ip_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ip_col, 0, 0);
    lv_obj_set_style_pad_all(ip_col, 0, 0);
    lv_obj_set_flex_flow(ip_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ip_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ip_col, UI_THEME_PADDING_PX / 4, 0);
    s_ip_qr = lv_qrcode_create(ip_col);
    lv_qrcode_set_size(s_ip_qr, UI_PAGE_NETWORK_QR_SIZE_PX);
    s_ip_qr_caption = lv_label_create(ip_col);
    lv_obj_set_style_text_color(s_ip_qr_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_ip_qr_caption, "http://--");
    s_ip_qr_last[0] = '\0';

    /* Manage-networks toggle -- swaps between the QR row above and the
     * Scan/Saved list block, connected-mode only (refresh_cb hides this
     * button entirely while not connected). See header comment. */
    s_manage_btn = lv_button_create(s_home_section);
    lv_obj_add_flag(s_manage_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_manage_btn, lv_pct(100));
    lv_obj_set_height(s_manage_btn, 40);
    lv_obj_set_style_bg_color(s_manage_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_manage_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_manage_btn, manage_btn_cb, LV_EVENT_CLICKED, NULL);
    s_manage_btn_label = lv_label_create(s_manage_btn);
    lv_obj_set_style_text_color(s_manage_btn_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_manage_btn_label, "Change network");
    lv_obj_center(s_manage_btn_label);
    lv_obj_update_layout(s_manage_btn);
    ui_theme_apply_touch_area(s_manage_btn, false);

    /* AP-mode section: identity display + AP-join QR. */
    s_ap_section = lv_obj_create(content);
    lv_obj_add_flag(s_ap_section, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ap_section, lv_pct(100));
    lv_obj_set_height(s_ap_section, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_ap_section, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_ap_section, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_ap_section, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_ap_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ap_section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_ap_section, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_ap_section, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ap_title = lv_label_create(s_ap_section);
    lv_obj_set_style_text_color(ap_title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(ap_title, "This board's access point");
    lv_obj_set_width(ap_title, lv_pct(100));

    lv_obj_t *ap_rows = lv_obj_create(s_ap_section);
    lv_obj_set_width(ap_rows, lv_pct(100));
    lv_obj_set_height(ap_rows, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ap_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_rows, 0, 0);
    lv_obj_set_style_pad_all(ap_rows, 0, 0);
    lv_obj_set_flex_flow(ap_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ap_rows, UI_THEME_PADDING_PX / 4, 0);
    s_ap_ssid_label = build_ap_stat_row(ap_rows, "SSID");
    s_ap_password_label = build_ap_stat_row(ap_rows, "Password");

    s_ap_qr = lv_qrcode_create(s_ap_section);
    lv_qrcode_set_size(s_ap_qr, UI_PAGE_NETWORK_QR_SIZE_PX);
    s_ap_qr_last[0] = '\0';
    lv_obj_t *ap_qr_caption = lv_label_create(s_ap_section);
    lv_obj_set_style_text_color(ap_qr_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(ap_qr_caption, "Scan to join from a phone");

    /* Connect modal -- built last so it's the topmost child in z-order
     * (LVGL's hit-test walks children highest-index-first, ui_theme.h's
     * header comment), covering the whole screen while shown. */
    build_connect_modal(scr);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as every other
     * page. */
    lv_timer_create(refresh_cb, UI_PAGE_NETWORK_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
