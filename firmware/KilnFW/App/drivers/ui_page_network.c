#include "ui_page_network.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiln_ui.h" /* kiln_ui_show("network_manage") -- manage_btn_cb() below */
#include "ui_confirm.h"
#include "ui_theme.h"
#include "ui_topbar.h"
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
//   - sta_connected: http://kilnctl.local (mDNS hostname read the same way
//     wifi_status_ui.c does) plus a second QR for the raw IP, since mDNS
//     isn't reliable on every phone/network.
// Neither lv_qrcode_update() call runs on every UI_PAGE_NETWORK_REFRESH_MS
// tick -- each is gated on the underlying string actually having changed
// (s_ap_qr_last/s_dashboard_qr_last), since re-encoding a QR
// code is real work the panel doesn't need to repeat every second for data
// that changes on the order of minutes, if ever, during one boot.
//
// 2026-08-21 AP-identity edit (TODO.md 10.9): the board's own AP SSID/
// password is no longer read-only here -- a small "Edit" button next to
// "This board's access point" opens a full-screen modal (build_ap_edit_modal(),
// two lv_textarea fields sharing one lv_keyboard, focus-switched via
// LV_EVENT_FOCUSED) pre-filled with the current
// wifi_prov_get_ap_ssid()/_get_ap_password() values. Reuses the existing
// wifi_prov_set_ap_ssid()/_set_ap_password() setters wifi_provision_http.c's
// POST handler already calls -- no parallel setter is added here, per
// TODO.md 10.1a's shared-backend rule. Save validates length client-side
// against the exact same rule the setters themselves enforce
// (WIFI_PROV_SSID_MAX_LEN=32, ssid 1-32 chars; WIFI_PROV_PASSWORD_MAX_LEN=64,
// password either blank/open or 8-63 chars, WPA2-PSK's real floor) so a
// rejection is explained on the spot rather than surfacing as an opaque
// ESP_ERR_INVALID_SIZE from the setter. Since "already-associated clients are
// kicked and must rejoin" per wifi_prov_set_ap_ssid()/_set_ap_password()'s own
// header comments -- including this very panel if it happens to be tethered
// through the AP being edited -- Save does not apply anything itself: it
// opens ui_confirm.c's shared Yes/Cancel dialog with that exact consequence
// spelled out, and only the confirm callback (ap_edit_confirm_apply_cb) kicks
// off the actual change, on a worker task (ap_identity_job_t) -- applying the
// new identity plus a forced AP-mode reapply to kick clients is real radio
// work, not a cheap NVS write.
// 2026-08-18 no-scroll pass: `scr`/`content`/the connect modal below
// explicitly clear LV_OBJ_FLAG_SCROLLABLE, and several rows/buttons were
// shrunk (mode toggle and Scan button from UI_THEME_MIN_TOUCH_TARGET_PX/72px
// to 44px, Back from 72px to 36px) against this page's content budget
// (480x320 landscape -- see ui_theme.h's UI_THEME_PAGE_CONTENT_BUDGET_PX).
//
// 2026-08-19 freeze/back-button fix: two bugs reported from the bench.
// (1) "No visible back button": this page's Back button was the last child
// of the variable-height `content` column, same as every other page -- but
// unlike those, this page's content was close enough to its no-scroll
// budget that in the worst-case state (list block visible) Back got pushed
// past the bottom of the fixed-height screen with page-level scrolling
// deliberately disabled -- there was no way to reach it. Fixed by moving
// Back into the fixed-height title `bar` instead of the variable-height
// `content` column: it's now always on screen regardless of how tall the
// content below happens to be, and it also gets content's budget back (one
// less 36-44px row to fit).
//
// 2026-08-21 icon-topbar pass: the hand-rolled title `bar` + in-bar Back
// button above were replaced with the shared ui_topbar.c module (also adds a
// Home icon, per the "Back/Prev/Next/Home should always be icons at the top"
// ask). Because Back was already living in the fixed-height bar rather than
// in `content` (see the 2026-08-19 fix just above), this swap frees no
// additional content px -- the win here is consistency (one nav-icon
// implementation for every page) and the Home icon, not new content budget.
//
// 2026-08-18/19 Scan-freeze fix + the several list-block budget passes that
// followed it (mutual-exclusion toggle, combining status_card onto one
// line, moving Back into the topbar) all lived in THIS file through
// 2026-08-23. Every one of those passes' own arithmetic ended in "computed,
// not hardware-confirmed" -- and the last one (2026-08-19) never accounted
// for the "Change network"/"Show QR" button being visible AT THE SAME TIME
// as the list block once connected (a real, reachable state: connect, then
// tap "Change network"), which pushes the true worst case well past what
// that arithmetic claimed. Rather than re-chase that arithmetic yet again,
// 2026-08-24 moved the whole Scan/Saved list block -- the toggle row, Scan
// button + its async worker (the freeze fix), the two lists, Forget +
// its confirm dialog, and the Connect modal -- to its own page,
// ui_page_network_manage.c, reachable from a single "Manage networks"
// button here via kiln_ui_show("network_manage"). See that file's own
// header comment for the freeze-fix history (unchanged by the move) and
// its own _Static_assert. This page no longer has a scan/connect/forget
// code path at all; wifi_prov.c's own "no lock on s_wifi" caveat mentioned
// in earlier revisions of this comment applies to ui_page_network_manage.c
// now, not here.
static const char *TAG __attribute__((unused)) = "ui_page_network";

#define UI_PAGE_NETWORK_REFRESH_MS 1000
#define UI_PAGE_NETWORK_QR_SIZE_PX 100 /* was 140 -- shrunk in the 2026-08-18 no-scroll pass, see this file's
                                         * header comment. STA-connected dashboard QR only, see
                                         * UI_PAGE_NETWORK_AP_QR_SIZE_PX below for the separate AP-mode one. */

/* ---- No-scroll budget arithmetic (TODO.md's "ui_page_network.c's
 * worst-case fit is ~268px against a ~264px budget" item -- see
 * ui_theme.h's UI_THEME_PAGE_CONTENT_BUDGET_PX for the real, computed
 * budget number).
 *
 * Two mutually exclusive states share `content` (see
 * ui_page_network_build()): the STA-mode section (status_card + mode_row +
 * s_home_section, the latter now just the QR row + one "Manage networks"
 * nav button after the 2026-08-24 split) and the AP-mode section
 * (status_card + mode_row + s_ap_section). Both must independently fit the
 * budget -- `content` only ever shows one at a time, but each is a real
 * reachable state on its own.
 *
 * A THIRD, previously undocumented overflow was found while deriving this:
 * s_ap_section alone (title row + edit button + two identity rows + a
 * 100px QR + caption, at the pre-2026-08-24 padding/QR size) summed to
 * ~302px total content against the real 268px budget -- 34px over, and
 * nothing in TODO.md or this file's own prior comments ever named it (every
 * earlier budget pass here was chasing the Scan/Saved list, never the
 * AP-identity section). UI_PAGE_NETWORK_AP_QR_SIZE_PX and the shrunk
 * ap_edit_btn/s_ap_section padding below (see ui_page_network_build()) fix
 * it in the same pass, since a _Static_assert covering this page's real
 * worst case has to cover BOTH branches to mean anything. */
#define UI_PAGE_NETWORK_AP_QR_SIZE_PX 72 /* separate from UI_PAGE_NETWORK_QR_SIZE_PX -- see above */

#define UI_PAGE_NETWORK_STATUS_CARD_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) + UI_THEME_FONT_LINE_HEIGHT_PX) /* pad top+bottom + one line */
#define UI_PAGE_NETWORK_MODE_ROW_HEIGHT_PX      44 /* s_mode_home_btn/s_mode_ap_btn height, no row padding */
#define UI_PAGE_NETWORK_MANAGE_BTN_HEIGHT_PX    36
#define UI_PAGE_NETWORK_STA_QR_ROW_HEIGHT_PX \
    ((UI_THEME_PADDING_PX * 2) /* s_sta_qr_row's own top+bottom pad_all */ + UI_PAGE_NETWORK_QR_SIZE_PX + \
     (UI_THEME_PADDING_PX / 4) /* dash_col's qr<->caption gap */ + UI_THEME_FONT_LINE_HEIGHT_PX /* caption */)
#define UI_PAGE_NETWORK_AP_EDIT_BTN_HEIGHT_PX   24
#define UI_PAGE_NETWORK_AP_ROWS_HEIGHT_PX \
    ((2 * UI_THEME_FONT_LINE_HEIGHT_PX) + (UI_THEME_PADDING_PX / 4)) /* SSID + Password rows, one gap */
#define UI_PAGE_NETWORK_AP_SECTION_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) /* s_ap_section's own top+bottom pad_all */ + \
     UI_PAGE_NETWORK_AP_EDIT_BTN_HEIGHT_PX /* ap_title_row, tallest child */ + \
     (UI_THEME_PADDING_PX / 2) /* gap */ + UI_PAGE_NETWORK_AP_ROWS_HEIGHT_PX + (UI_THEME_PADDING_PX / 2) + \
     UI_PAGE_NETWORK_AP_QR_SIZE_PX + (UI_THEME_PADDING_PX / 2) + UI_THEME_FONT_LINE_HEIGHT_PX /* caption */)

/* `content`'s own pad_gap (UI_THEME_PADDING_PX/2) between each of its 3
 * visible children (status_card, mode_row, and whichever of
 * s_home_section/s_ap_section is shown). */
#define UI_PAGE_NETWORK_CONTENT_GAPS_PX (2 * (UI_THEME_PADDING_PX / 2))

#define UI_PAGE_NETWORK_STA_WORST_CASE_HEIGHT_PX \
    (UI_PAGE_NETWORK_STATUS_CARD_HEIGHT_PX + UI_PAGE_NETWORK_MODE_ROW_HEIGHT_PX + \
     (UI_PAGE_NETWORK_STA_QR_ROW_HEIGHT_PX + (UI_THEME_PADDING_PX / 2) + UI_PAGE_NETWORK_MANAGE_BTN_HEIGHT_PX) + \
     UI_PAGE_NETWORK_CONTENT_GAPS_PX)
#define UI_PAGE_NETWORK_AP_WORST_CASE_HEIGHT_PX \
    (UI_PAGE_NETWORK_STATUS_CARD_HEIGHT_PX + UI_PAGE_NETWORK_MODE_ROW_HEIGHT_PX + \
     UI_PAGE_NETWORK_AP_SECTION_HEIGHT_PX + UI_PAGE_NETWORK_CONTENT_GAPS_PX)

_Static_assert(UI_PAGE_NETWORK_STA_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_network.c: the STA-connected (QR + Manage networks) state exceeds "
               "UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink UI_PAGE_NETWORK_QR_SIZE_PX or "
               "UI_PAGE_NETWORK_MANAGE_BTN_HEIGHT_PX, don't widen the budget to match.");
_Static_assert(UI_PAGE_NETWORK_AP_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_network.c: the AP-mode identity section exceeds UI_THEME_PAGE_CONTENT_BUDGET_PX "
               "(ui_theme.h) -- shrink UI_PAGE_NETWORK_AP_QR_SIZE_PX or the ap_section padding, don't "
               "widen the budget to match.");

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

/* ---- Home-mode section: STA-connected QR + "Manage networks" nav button ----
 * Scan/Saved/Connect/Forget all moved to ui_page_network_manage.c
 * (2026-08-24, this file's header comment) -- s_manage_btn now navigates
 * there via kiln_ui_show("network_manage") instead of toggling a local list
 * view, so this section is unconditionally just the QR row plus that one
 * button while connected, or just the button while not connected (there is
 * nothing else useful to show without a network to join). */
static lv_obj_t *s_home_section;
static lv_obj_t *s_manage_btn;
static lv_obj_t *s_manage_btn_label;
static lv_obj_t *s_sta_qr_row;
static lv_obj_t *s_dashboard_qr;
static lv_obj_t *s_dashboard_qr_caption;
static char s_dashboard_qr_last[80];

/* ---- Async mode-switch job -- same shape/reasoning as
 * ui_page_network_manage.c's scan_job_t (the original freeze fix this whole
 * async-worker-task pattern comes from). mode_home_btn_cb()/mode_ap_btn_cb()
 * used to call wifi_prov_set_mode()
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

/* ---- AP-mode section: identity display + AP-join QR ---- */
static lv_obj_t *s_ap_section;
static lv_obj_t *s_ap_ssid_label;
static lv_obj_t *s_ap_password_label;
static lv_obj_t *s_ap_qr;
static char s_ap_qr_last[16 + WIFI_PROV_SSID_MAX_LEN + WIFI_PROV_PASSWORD_MAX_LEN];

/* ---- AP identity edit modal -- see this file's 2026-08-21 header comment.
 * A "full-screen overlay built once, toggled hidden" shape, same pattern
 * ui_page_network_manage.c's connect modal uses. */
static lv_obj_t *s_ap_edit_modal;
static lv_obj_t *s_ap_edit_ssid_ta;
static lv_obj_t *s_ap_edit_password_ta;
static lv_obj_t *s_ap_edit_status_label;
static lv_obj_t *s_ap_edit_kb;

/* ---- Async AP-identity-apply job -- same shape as
 * ui_page_network_manage.c's connect_job_t.
 * ssid/password are copied into these module statics (while still on
 * lvgl_port_task, from the confirm dialog's on_confirm callback) before the
 * worker task starts; the worker never touches an LVGL widget. */
typedef struct {
    SemaphoreHandle_t lock;
    bool busy;
    bool done;
    esp_err_t err;
} ap_identity_job_t;

static ap_identity_job_t s_ap_identity_job;
static char s_ap_identity_job_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
static char s_ap_identity_job_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];

static void ap_identity_worker_task(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_prov_set_ap_ssid(s_ap_identity_job_ssid, strlen(s_ap_identity_job_ssid));
    if (err == ESP_OK) {
        /* Only apply the password if the SSID change actually took --
         * partial application (new name, old password silently kept) would
         * be a confusing half-applied state to hand back to the operator. */
        err = wifi_prov_set_ap_password(s_ap_identity_job_password, strlen(s_ap_identity_job_password));
    }
    xSemaphoreTake(s_ap_identity_job.lock, portMAX_DELAY);
    s_ap_identity_job.err = err;
    s_ap_identity_job.done = true;
    s_ap_identity_job.busy = false;
    xSemaphoreGive(s_ap_identity_job.lock);
    vTaskDelete(NULL);
}

static void ap_edit_modal_close(void);

/* Applies a completed ap_identity_worker_task result -- polled from
 * refresh_cb(), same "poll from the timer" shape as
 * ui_page_network_manage.c's apply_connect_job_result(). */
static void apply_ap_identity_job_result(void)
{
    if (!s_ap_identity_job.lock) return;
    xSemaphoreTake(s_ap_identity_job.lock, portMAX_DELAY);
    bool done = s_ap_identity_job.done;
    esp_err_t err = s_ap_identity_job.err;
    if (done) s_ap_identity_job.done = false;
    xSemaphoreGive(s_ap_identity_job.lock);
    if (!done) return;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP identity change failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_ap_edit_status_label, "Could not apply -- try again");
        return;
    }
    ap_edit_modal_close();
}

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

/* ---- "Manage networks" nav button -- navigates to ui_page_network_manage.c
 * (2026-08-24 split, this file's header comment) instead of toggling a
 * local list view. kiln_ui_show() logs and no-ops if the page were ever
 * somehow unregistered (kiln_ui.c's own documented failure mode), so a
 * missed registration fails soft here too, same as every other nav button
 * in this codebase. */

static void manage_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("network_manage");
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

    apply_ap_identity_job_result();

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

    /* s_home_section is now unconditionally "QR row (if connected) + one
     * Manage networks nav button" -- see this file's header comment for
     * what used to branch here (list-block-vs-QR, s_manage_open) before the
     * 2026-08-24 split moved the list block to its own page. The button is
     * always shown in STA mode (not just once connected) now, since it is
     * the only way to reach Scan/Saved/Connect at all any more. */
    lv_obj_remove_flag(s_manage_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_manage_btn_label, "Manage networks");

    if (sta_connected) {
        int8_t rssi = wifi_prov_get_sta_rssi();
        snprintf(detail_buf, sizeof(detail_buf), "Signal: %d dBm", (int)rssi);
        lv_obj_remove_flag(s_sta_qr_row, LV_OBJ_FLAG_HIDDEN);

        char mdns_host[MDNS_NAME_BUF_LEN];
        char dashboard_url[80];
        if (mdns_hostname_get(mdns_host) == ESP_OK) {
            snprintf(dashboard_url, sizeof(dashboard_url), "http://%s.local", mdns_host);
        } else {
            snprintf(dashboard_url, sizeof(dashboard_url), "http://kilnctl.local");
        }
        update_qr_if_changed(s_dashboard_qr, s_dashboard_qr_last, sizeof(s_dashboard_qr_last), dashboard_url);
        lv_label_set_text(s_dashboard_qr_caption, dashboard_url);
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
         * separator (same fix ui_page_network_manage.c's
         * apply_scan_job_result() uses, for the same reason). */
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

/* ---- AP identity edit modal ---- */

static void ap_edit_modal_close(void)
{
    lv_obj_add_flag(s_ap_edit_modal, LV_OBJ_FLAG_HIDDEN);
}

static void ap_edit_cancel_cb(lv_event_t *e)
{
    (void)e;
    ap_edit_modal_close();
}

/* Focus-switches the shared keyboard to whichever field the operator just
 * tapped -- same "one lv_keyboard, two textareas" trick a single-field modal
 * (ui_page_network_manage.c's connect modal) doesn't need, since that one
 * only ever has one textarea. */
static void ap_edit_ssid_focus_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_textarea(s_ap_edit_kb, s_ap_edit_ssid_ta);
}

static void ap_edit_password_focus_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_textarea(s_ap_edit_kb, s_ap_edit_password_ta);
}

/* Called after the operator confirms the "this will disconnect clients"
 * dialog (see ap_edit_save_cb() below) -- ssid/password have already been
 * validated and copied into the job's module buffers by then, so this only
 * has to kick off the worker task. */
static void ap_edit_confirm_apply_cb(void *user_data)
{
    (void)user_data;
    if (!s_ap_identity_job.lock) {
        lv_label_set_text(s_ap_edit_status_label, "Could not apply -- try again");
        return;
    }

    xSemaphoreTake(s_ap_identity_job.lock, portMAX_DELAY);
    bool already_busy = s_ap_identity_job.busy;
    if (!already_busy) {
        s_ap_identity_job.busy = true;
        s_ap_identity_job.done = false;
    }
    xSemaphoreGive(s_ap_identity_job.lock);
    if (already_busy) return;

    lv_label_set_text(s_ap_edit_status_label, "Applying...");

    BaseType_t created = xTaskCreate(ap_identity_worker_task, "wifi_ap_id_ui", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_ap_identity_job.lock, portMAX_DELAY);
        s_ap_identity_job.busy = false;
        xSemaphoreGive(s_ap_identity_job.lock);
        lv_label_set_text(s_ap_edit_status_label, "Could not start");
    }
}

/* Save: validates against the exact same rule wifi_prov_set_ap_ssid()/
 * _set_ap_password() themselves enforce (see this file's 2026-08-21 header
 * comment) so a bad value is explained here rather than surfacing as a bare
 * ESP_ERR_INVALID_SIZE from the setter, then hands off to ui_confirm.c's
 * shared dialog -- changing the AP's own identity drops any client connected
 * through it (possibly this very panel), so nothing is applied until the
 * operator explicitly confirms that consequence. */
static void ap_edit_save_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(s_ap_edit_ssid_ta);
    const char *password = lv_textarea_get_text(s_ap_edit_password_ta);
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);

    if (ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        lv_label_set_text(s_ap_edit_status_label, "SSID must be 1-32 characters");
        return;
    }
    if (password_len > 0 && password_len < 8) {
        lv_label_set_text(s_ap_edit_status_label, "Password must be blank (open) or 8-63 characters");
        return;
    }
    if (password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        lv_label_set_text(s_ap_edit_status_label, "Password must be blank (open) or 8-63 characters");
        return;
    }

    snprintf(s_ap_identity_job_ssid, sizeof(s_ap_identity_job_ssid), "%s", ssid);
    snprintf(s_ap_identity_job_password, sizeof(s_ap_identity_job_password), "%s", password);
    lv_label_set_text(s_ap_edit_status_label, "");

    ui_confirm_params_t confirm = {
        .title = "Change access point identity?",
        .body = "This changes the board's own Wi-Fi network name and/or password. "
                "Any device connected through it -- including this panel, if it is "
                "connected over this AP -- will be disconnected immediately and must "
                "rejoin using the new name/password.",
        .confirm_label = "Apply",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = ap_edit_confirm_apply_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&confirm);
}

/* Edit button -- prefills both fields with the identity currently in effect
 * (wifi_prov_get_ap_ssid()/_get_ap_password(), the same values this page
 * already shows in plain text just above the button) rather than opening to
 * blank fields the operator would have to re-type from scratch. */
static void ap_edit_open_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_ap_edit_ssid_ta, wifi_prov_get_ap_ssid());
    lv_textarea_set_text(s_ap_edit_password_ta, wifi_prov_get_ap_password());
    lv_label_set_text(s_ap_edit_status_label, "");
    lv_obj_remove_flag(s_ap_edit_modal, LV_OBJ_FLAG_HIDDEN);
}

/* Same full-screen-overlay shape as ui_page_network_manage.c's connect
 * modal, with a second textarea (SSID, plain text -- not
 * lv_textarea_set_password_mode()) sharing the one keyboard via
 * focus-switching (ap_edit_ssid_focus_cb()/
 * ap_edit_password_focus_cb() above). */
static void build_ap_edit_modal(lv_obj_t *scr)
{
    s_ap_edit_modal = lv_obj_create(scr);
    lv_obj_add_flag(s_ap_edit_modal, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_ap_edit_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_ap_edit_modal, 0, 0);
    lv_obj_set_style_bg_color(s_ap_edit_modal, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ap_edit_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ap_edit_modal, 0, 0);
    lv_obj_set_style_pad_all(s_ap_edit_modal, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_ap_edit_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_ap_edit_modal, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(s_ap_edit_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_ap_edit_modal);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Edit access point identity");

    s_ap_edit_ssid_ta = lv_textarea_create(s_ap_edit_modal);
    lv_obj_set_width(s_ap_edit_ssid_ta, lv_pct(100));
    lv_textarea_set_one_line(s_ap_edit_ssid_ta, true);
    lv_textarea_set_max_length(s_ap_edit_ssid_ta, WIFI_PROV_SSID_MAX_LEN);
    lv_textarea_set_placeholder_text(s_ap_edit_ssid_ta, "Network name (1-32 chars)");
    lv_obj_add_event_cb(s_ap_edit_ssid_ta, ap_edit_ssid_focus_cb, LV_EVENT_FOCUSED, NULL);

    s_ap_edit_password_ta = lv_textarea_create(s_ap_edit_modal);
    lv_obj_set_width(s_ap_edit_password_ta, lv_pct(100));
    lv_textarea_set_one_line(s_ap_edit_password_ta, true);
    lv_textarea_set_password_mode(s_ap_edit_password_ta, true);
    lv_textarea_set_max_length(s_ap_edit_password_ta, WIFI_PROV_PASSWORD_MAX_LEN);
    lv_textarea_set_placeholder_text(s_ap_edit_password_ta, "Password (blank = open, else 8-63 chars)");
    lv_obj_add_event_cb(s_ap_edit_password_ta, ap_edit_password_focus_cb, LV_EVENT_FOCUSED, NULL);

    s_ap_edit_status_label = lv_label_create(s_ap_edit_modal);
    lv_obj_set_style_text_color(s_ap_edit_status_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_ap_edit_status_label, "");

    lv_obj_t *btn_row = lv_obj_create(s_ap_edit_modal);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, UI_THEME_PADDING_PX, 0);

    lv_obj_t *save_btn = lv_button_create(btn_row);
    lv_obj_set_height(save_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(save_btn, 1);
    lv_obj_set_style_bg_color(save_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_add_event_cb(save_btn, ap_edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    lv_obj_update_layout(save_btn);
    ui_theme_apply_touch_area(save_btn, false);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_height(cancel_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_add_event_cb(cancel_btn, ap_edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_update_layout(cancel_btn);
    ui_theme_apply_touch_area(cancel_btn, false);

    s_ap_edit_kb = lv_keyboard_create(s_ap_edit_modal);
    lv_keyboard_set_textarea(s_ap_edit_kb, s_ap_edit_ssid_ta);
}

lv_obj_t *ui_page_network_build(void)
{
    /* Created up front, before refresh_cb() ever runs -- ap_identity_job_t's
     * header comment has the "why lazy creation on first tap was a bug"
     * reasoning (the poll functions run from the very first tick, before
     * any tap). Pages are built exactly once (kiln_ui.h's header comment),
     * so this never runs twice. */
    s_mode_job.lock = xSemaphoreCreateMutex();
    s_ap_identity_job.lock = xSemaphoreCreateMutex();
    if (!s_mode_job.lock || !s_ap_identity_job.lock) {
        /* Never observed to actually fail (two tiny allocations, this
         * late in boot) -- but per this codebase's "never crash on a
         * resource failure" convention (see wifi_prov.h's header comment),
         * every job-entry point below checks its own lock for NULL and
         * no-ops rather than trusting this always succeeds. */
        ESP_LOGE(TAG, "ui_page_network: mutex allocation failed -- mode/AP-identity disabled");
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
        .title = "Network / Wi-Fi",
        .back_page = "config",
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

    /* Status card. */
    lv_obj_t *status_card = lv_obj_create(content);
    lv_obj_set_width(status_card, lv_pct(100));
    lv_obj_set_height(status_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(status_card, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Phase 7 theme/style pass (TODO.md 1223-1225): pure-paint shadow, see
     * ui_theme.h -- costs no page-budget height. */
    ui_theme_apply_card_shadow(status_card, 1);
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

    /* STA-connected QR row -- dashboard (kilnctl.local), hidden until
     * sta_connected (this file's header comment). Scan/Saved/Connect/Forget
     * all live on ui_page_network_manage.c now (2026-08-24 split) -- this
     * section no longer builds any list/toggle widgets at all. */
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
    lv_label_set_text(s_dashboard_qr_caption, "http://kilnctl.local");
    s_dashboard_qr_last[0] = '\0';

    /* No raw-IP QR here. It existed as a fallback for clients that cannot
     * resolve .local, but kilnctl.local was confirmed working from an Android
     * phone on the bench 2026-08-20, and a second QR next to the first mostly
     * invites scanning the wrong one. The IP is still on this page as text,
     * which is what someone typing it by hand needs anyway. */

    /* "Manage networks" nav button -- always shown in STA mode (not just
     * once connected: it's the only way to reach Scan/Saved/Connect at all
     * now), navigates to ui_page_network_manage.c via manage_btn_cb()
     * above. 36px, not 40 -- see UI_PAGE_NETWORK_MANAGE_BTN_HEIGHT_PX's
     * budget arithmetic near the top of this file. */
    s_manage_btn = lv_button_create(s_home_section);
    lv_obj_set_width(s_manage_btn, lv_pct(100));
    lv_obj_set_height(s_manage_btn, UI_PAGE_NETWORK_MANAGE_BTN_HEIGHT_PX);
    lv_obj_set_style_bg_color(s_manage_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_manage_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_manage_btn, manage_btn_cb, LV_EVENT_CLICKED, NULL);
    s_manage_btn_label = lv_label_create(s_manage_btn);
    lv_obj_set_style_text_color(s_manage_btn_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_manage_btn_label, "Manage networks");
    lv_obj_center(s_manage_btn_label);
    lv_obj_update_layout(s_manage_btn);
    ui_theme_apply_touch_area(s_manage_btn, false);

    /* AP-mode section: identity display + AP-join QR. Padding halved
     * (UI_THEME_PADDING_PX/2, not a full UI_THEME_PADDING_PX) and the QR
     * shrunk to UI_PAGE_NETWORK_AP_QR_SIZE_PX -- see this file's header
     * comment for the previously-undocumented ~34px overflow this section
     * had on its own, found while deriving the _Static_assert above. */
    s_ap_section = lv_obj_create(content);
    lv_obj_add_flag(s_ap_section, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ap_section, lv_pct(100));
    lv_obj_set_height(s_ap_section, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_ap_section, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_ap_section, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Phase 7 theme/style pass (TODO.md 1223-1225): pure-paint shadow, see
     * ui_theme.h -- costs no page-budget height. */
    ui_theme_apply_card_shadow(s_ap_section, 1);
    lv_obj_set_style_pad_all(s_ap_section, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(s_ap_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ap_section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_ap_section, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_ap_section, LV_OBJ_FLAG_SCROLLABLE);

    /* Title + Edit button share one row rather than Edit getting its own
     * full-width row below -- keeps this section's height essentially
     * unchanged from before the edit feature existed (this page's content
     * budget has no slack to spare, see this file's header comment). */
    lv_obj_t *ap_title_row = lv_obj_create(s_ap_section);
    lv_obj_set_width(ap_title_row, lv_pct(100));
    lv_obj_set_height(ap_title_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ap_title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_title_row, 0, 0);
    lv_obj_set_style_pad_all(ap_title_row, 0, 0);
    lv_obj_set_flex_flow(ap_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ap_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ap_title = lv_label_create(ap_title_row);
    lv_obj_set_style_text_color(ap_title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(ap_title, "This board's access point");
    lv_obj_set_flex_grow(ap_title, 1);

    lv_obj_t *ap_edit_btn = lv_button_create(ap_title_row);
    lv_obj_set_height(ap_edit_btn, UI_PAGE_NETWORK_AP_EDIT_BTN_HEIGHT_PX); /* was 32 -- see this file's
                                                                              * header comment on the AP
                                                                              * section's budget fix */
    lv_obj_set_width(ap_edit_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ap_edit_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(ap_edit_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(ap_edit_btn, ap_edit_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ap_edit_label = lv_label_create(ap_edit_btn);
    lv_obj_set_style_text_color(ap_edit_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(ap_edit_label, "Edit");
    lv_obj_center(ap_edit_label);
    lv_obj_set_style_pad_hor(ap_edit_btn, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_update_layout(ap_edit_btn);
    ui_theme_apply_touch_area(ap_edit_btn, true);

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
    lv_qrcode_set_size(s_ap_qr, UI_PAGE_NETWORK_AP_QR_SIZE_PX);
    s_ap_qr_last[0] = '\0';
    lv_obj_t *ap_qr_caption = lv_label_create(s_ap_section);
    lv_obj_set_style_text_color(ap_qr_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(ap_qr_caption, "Scan to join from a phone");

    /* Raise the top bar's icon proxy BEFORE the AP-identity-edit modal is
     * built, not after -- ui_topbar.h requires raise() to run after the
     * content area exists, but this page also has a full-screen modal
     * (s_ap_edit_modal) that must stay ABOVE the icons when it is shown
     * (both are ordinary children of `scr`, and LVGL's hit-test/paint order
     * is z-order-by-child-index, so whichever is added later wins). Raising
     * first, then building the modal last, preserves that. */
    ui_topbar_raise(&tb);

    /* AP-identity-edit modal -- built last so it's the topmost child in
     * z-order (LVGL's hit-test walks children highest-index-first,
     * ui_theme.h's header comment), covering the whole screen while shown.
     * The Connect modal that used to be built here too moved to
     * ui_page_network_manage.c along with the rest of the Scan/Saved flow
     * (2026-08-24 split, this file's header comment). */
    build_ap_edit_modal(scr);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as every other
     * page. */
    lv_timer_create(refresh_cb, UI_PAGE_NETWORK_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
