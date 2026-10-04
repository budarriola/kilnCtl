// recovery_http.c -- see recovery_http.h.
//
// NO AUTHENTICATION (owner decision 2026-10-02, docs/RECOVERY_IMAGE_PLAN.md):
// every route in this file is open. The only access control is the SoftAP
// itself: recovery_wifi.c brings up a WPA2-PSK access point whose random
// per-boot passphrase is shown ONLY on the LCD (recovery_lcd.c). A client that
// can reach these routes has been told the passphrase by someone who can see
// the screen. The passphrase never appears in any response, log or NVS. The
// station interface is never started in this image, so the SoftAP is the only
// network path to this httpd (it binds all interfaces, and only one exists).
//
// GET /api/boot_guard and GET /api/recovery/status report whether the shared
// `kiln_cfg`/"bootguard" NVS record exists and its raw length, and decode its
// count ONLY when ric_boot_guard_decode() (recovery_image_check.c, host-
// tested against boot_guard.c's 12-byte record layout, version + CRC
// verified) accepts it -- otherwise the count is omitted, never guessed.
// POST .../boot_guard_reset only erases the key (nvs_erase_key) rather
// than writing a specific "cleared" struct value -- erasing is
// format-agnostic and every known reader (boot_guard.c's own hal_kv_open/get
// path) already treats "key not found" as counter-zero.
//
// Upload plumbing (first-chunk validation, streaming, sink abort) lives in
// recovery_upload.c; an upload route calls recovery_upload_stream() with its
// own validator and sink.
#include "recovery_http.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "recovery_health_policy.h"
#include "recovery_image_check.h"
#include "recovery_io.h"
#include "recovery_lcd.h"
#include "recovery_lcd_policy.h"
#include "recovery_pico.h"
#include "recovery_pico_proto.h"
#include "recovery_text.h"
#include "recovery_upload.h"
#include "recovery_wifi.h"

static const char *TAG = "recovery_http";

// The page (recovery_page.html) is linked in via EMBED_FILES.
extern const uint8_t recovery_page_html_start[] asm("_binary_recovery_page_html_start");
extern const uint8_t recovery_page_html_end[] asm("_binary_recovery_page_html_end");

#define APP_PARTITION_LABEL "app"
// Wi-Fi namespace shared with the main app (wifi_prov_nvs.c NVS_NAMESPACE) and
// the keys that describe the HOME network / addressing. ap_ssid is
// deliberately NOT erased (the SoftAP reuses it); a stored ap_pass is ignored
// by this image and left alone.
#define WIFI_NVS_PARTITION "wifi_nvs"
#define WIFI_NVS_NAMESPACE "wifi_cfg"
static const char *const WIFI_RESET_KEYS[] = {
    "ssid", "pass", "has_creds", "saved_nets", "mode", "local_only",
    "ip_mode", "static_ip", "static_netmask", "static_gw",
};

#define KILN_NVS_PARTITION "kiln_nvs"
#define BOOT_GUARD_NAMESPACE "kiln_cfg"
#define BOOT_GUARD_KEY "bootguard"
#define BOOT_GUARD_KEY_LEGACY "count"
#define BOOT_GUARD_NAMESPACE_LEGACY "boot_guard"

static const esp_partition_t *find_app_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                    APP_PARTITION_LABEL);
}

// True when `app` starts with a plausible image (esp_ota_get_partition_
// description checks the image/app-desc magic). A cheap sanity gate, not a
// full verification -- the bootloader still verifies on boot.
static bool app_has_valid_image(const esp_partition_t *part)
{
    if (!part) {
        return false;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        return false;
    }
    // Same identity the upload validator enforces: chip id (u16 at 0x0C of
    // esp_image_header_t) and project name, not just the descriptor magic.
    uint8_t hdr[16];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    uint16_t chip = (uint16_t)(hdr[12] | (hdr[13] << 8));
    return chip == RIC_CHIP_ID_ESP32S3 &&
           strncmp(desc.project_name, RIC_EXPECTED_PROJECT, sizeof(desc.project_name)) == 0;
}

// --- full-image verification (cached) ------------------------------------
// esp_image_verify() on `app` reads the whole image (~2.5 MB of flash) and
// hashes it, so it must never run per status GET. The result is cached and
// invalidated when an upload starts or ends; the next status/exit read refills
// it lazily. httpd is effectively single-task here, so no lock is needed.
static bool s_verify_known;
static bool s_verify_valid;
static uint32_t s_verify_image_len; // meta.image_len of the last successful verify, else 0

static void app_verify_invalidate(void)
{
    s_verify_known = false;
}

static bool app_image_verified(const esp_partition_t *part)
{
    if (!part) {
        return false;
    }
    if (!s_verify_known) {
        const esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
        esp_image_metadata_t meta;
        s_verify_valid = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK;
        s_verify_image_len = s_verify_valid ? meta.image_len : 0;
        s_verify_known = true;
    }
    return s_verify_valid;
}

// Bytes of the image esp_image_verify() accepted (header + segments + padding +
// checksum/digest), or 0 when the image is not verified. Call after
// app_image_verified(), which fills the cache.
static uint32_t app_image_len(void)
{
    return s_verify_valid ? s_verify_image_len : 0;
}

static bool clear_boot_guard(char *msg, size_t cap);

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_restart();
    vTaskDelete(NULL);
}

// Response is already sent by the caller; restart after a short delay so the
// TCP stack can flush it. Falls back to restarting inline if no task fits.
static void restart_soon(uint32_t delay_ms)
{
    if (xTaskCreate(restart_task, "rec_restart", 2048, (void *)(uintptr_t)delay_ms, 5, NULL) !=
        pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_restart();
    }
}

// POST /api/ota/esp -- open, validated, streamed (recovery_upload.c).
// Nothing is written until the first chunk passes ric_validate_first_chunk();
// the boot partition is set only after esp_ota_end() verified the whole image.
static esp_err_t ota_esp_post(httpd_req_t *req)
{
    if (recovery_pico_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "Pico update in progress", HTTPD_RESP_USE_STRLEN);
    }

    const esp_partition_t *target = find_app_partition();
    if (!target) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "no app partition in the flashed table", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL; // body unread: close the socket rather than drain it
    }

    recovery_upload_cfg_t cfg = {
        .max_len = target->size,
        .validate = recovery_upload_validate_esp,
        .vctx = NULL,
    };
    recovery_sink_t sink;
    recovery_esp_sink_state_t sink_state;
    recovery_upload_esp_sink_init(&sink, &sink_state, target);

    app_verify_invalidate(); // `app` is about to change (or be half-erased)
    int http_status = 500;
    const char *msg = "upload failed";
    recovery_upload_result_t r = recovery_upload_stream(req, &cfg, &sink, &http_status, &msg);
    app_verify_invalidate();
    if (r != RECOVERY_UPLOAD_OK) {
        // Sends the error then returns ESP_FAIL so httpd closes the socket.
        return recovery_upload_send_error(req, http_status, msg);
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "esp_ota_set_boot_partition failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Full success: the new image is the boot target, so the next boot must
    // not be counted toward recovery. Verified clear, reported either way.
    char bg_msg[96];
    bool bg_ok = clear_boot_guard(bg_msg, sizeof(bg_msg));
    ESP_LOGI(TAG, "application image accepted and written -- rebooting into it (%s)", bg_msg);
    char body[192];
    snprintf(body, sizeof(body), "ok, rebooting into new application image; boot_guard %s",
             bg_ok ? "cleared and verified" : bg_msg);
    esp_err_t sent = httpd_resp_sendstr(req, body);
    restart_soon(500);
    return sent;
}

// GET / -- the embedded self-contained recovery page.
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(recovery_page_html_end - recovery_page_html_start);
    return httpd_resp_send(req, (const char *)recovery_page_html_start, (ssize_t)len);
}

// GET /api/partitions
static esp_err_t partitions_get(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    char body[256];
    int n = snprintf(body, sizeof(body),
                      "{\"running\":\"%s\",\"running_offset\":\"0x%06x\","
                      "\"next_update\":\"%s\"}",
                      running ? running->label : "?",
                      (unsigned)(running ? running->address : 0),
                      next ? next->label : "?");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

typedef struct {
    bool present;
    size_t len;
    bool count_valid;
    uint32_t count;
    rlcd_bg_state_t state; // none / valid / invalid / unreadable (re-read live per request; the
                           // LCD shows the boot-time snapshot from gather_status())
} boot_guard_info_t;

static void read_boot_guard(boot_guard_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->state = RLCD_BG_UNREADABLE;
    nvs_handle_t h;
    esp_err_t oerr = nvs_open_from_partition(KILN_NVS_PARTITION, BOOT_GUARD_NAMESPACE,
                                             NVS_READONLY, &h);
    if (oerr != ESP_OK) {
        if (oerr == ESP_ERR_NVS_NOT_FOUND) {
            info->state = RLCD_BG_NONE; // namespace never created
        }
        return;
    }
    esp_err_t gerr = nvs_get_blob(h, BOOT_GUARD_KEY, NULL, &info->len);
    bool decoded = false;
    if (gerr == ESP_OK) {
        info->present = true;
        uint8_t blob[16];
        size_t blen = sizeof(blob);
        if (info->len <= sizeof(blob) &&
            nvs_get_blob(h, BOOT_GUARD_KEY, blob, &blen) == ESP_OK) {
            info->count_valid = ric_boot_guard_decode(blob, blen, &info->count) != 0;
            decoded = info->count_valid;
        }
    }
    info->state = rlcd_classify_boot_guard(gerr == ESP_OK ? RLCD_GET_OK
                                           : gerr == ESP_ERR_NVS_NOT_FOUND ? RLCD_GET_NOT_FOUND
                                           : gerr == ESP_ERR_NVS_INVALID_LENGTH ? RLCD_GET_BAD_LENGTH
                                                                                : RLCD_GET_OTHER,
                                           decoded);
    nvs_close(h);
}

// Formats "record_present":..,"record_len":..[,"boot_count":N]; boot_count
// only when the record decoded cleanly (never guessed).
static int fmt_boot_guard(char *out, size_t cap, const boot_guard_info_t *bg)
{
    int n = snprintf(out, cap, "\"record_present\":%s,\"record_len\":%u,"
                               "\"boot_guard_record\":\"%s\"",
                      bg->present ? "true" : "false", (unsigned)bg->len,
                      rlcd_bg_state_name(bg->state));
    if (n > 0 && (size_t)n < cap && bg->count_valid) {
        n += snprintf(out + n, cap - (size_t)n, ",\"boot_count\":%u", (unsigned)bg->count);
    }
    return n;
}

// GET /api/boot_guard
static esp_err_t boot_guard_get(httpd_req_t *req)
{
    boot_guard_info_t bg;
    read_boot_guard(&bg);
    char body[160];
    char inner[128];
    fmt_boot_guard(inner, sizeof(inner), &bg);
    int n = snprintf(body, sizeof(body), "{%s}", inner);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

// printf-style helper: formats into one small static fragment buffer (httpd
// runs handlers on its single task, so a static costs no stack) and sends it as
// one HTTP chunk. The status JSON is streamed through this rather than built in
// one large buffer.
static esp_err_t send_frag(httpd_req_t *req, const char *fmt, ...)
{
    static char frag[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frag, sizeof(frag), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(frag)) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, frag, (ssize_t)n);
}

static const char *ota_state_name(const esp_partition_t *part)
{
    if (!part) {
        return "unknown";
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(part, &st) != ESP_OK) {
        return "unknown"; // no otadata record for this slot (or unsupported)
    }
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "unknown";
    }
}

// 1 = coredump partition holds a dump (first word not erased), 0 = erased,
// -1 = unreadable / no coredump partition.
static int coredump_present(void)
{
    const esp_partition_t *cd = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    uint32_t w = 0;
    if (!cd || esp_partition_read(cd, 0, &w, sizeof(w)) != ESP_OK) {
        return -1;
    }
    return w != 0xFFFFFFFFu;
}

// 1 = both otadata sectors fully erased (a blank otadata boots the factory
// image, not `app`), 0 = at least one programmed byte, -1 = unreadable.
static int otadata_blank(void)
{
    const esp_partition_t *od = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!od || od->size < 2 * 4096u) {
        return -1;
    }
    uint8_t buf[64];
    for (size_t sector = 0; sector < 2; sector++) {
        for (size_t off = 0; off < 4096u; off += sizeof(buf)) {
            if (esp_partition_read(od, sector * 4096u + off, buf, sizeof(buf)) != ESP_OK) {
                return -1;
            }
            for (size_t i = 0; i < sizeof(buf); i++) {
                if (buf[i] != 0xFF) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static const char *tri(int v)
{
    return v < 0 ? "null" : (v ? "true" : "false");
}

// GET /api/recovery/status -- read-only. Streamed in small
// chunks (send_frag), never one big buffer.
// Per-boot count of failed recovery_http_start() attempts and the last failure
// name. Deliberately NOT reset by a later success: a server that came up only
// on a retry stays visible as "http_start_attempts" > 0 in the status JSON.
static volatile uint32_t s_http_failed_attempts = 0;
static const char *volatile s_http_last_error = NULL;

static esp_err_t recovery_status_get(httpd_req_t *req)
{
    recovery_lcd_poll_relay_fault();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app = find_app_partition();
    boot_guard_info_t bg;
    read_boot_guard(&bg);
    char bgs[128];
    fmt_boot_guard(bgs, sizeof(bgs), &bg);

    // app_desc_present: cheap descriptor/chip/project check. app_valid: full
    // esp_image_verify(), cached (see app_image_verified()).
    bool desc_present = app_has_valid_image(app);
    bool valid = desc_present && app_image_verified(app);
    unsigned nvs_failed = recovery_io_nvs_failed_mask();
    int rr = (int)esp_reset_reason();
    // app_size is the PARTITION size (kept for existing consumers); the real
    // image size is app_image_size, null unless the image verified.
    unsigned app_size = (unsigned)(app ? app->size : 0);
    unsigned img_len = valid ? (unsigned)app_image_len() : 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // auth_mode: the routes are open; the only gate is the SoftAP passphrase
    // shown on the LCD (never reported here).
    esp_err_t e = send_frag(req, "{\"auth_mode\":\"lcd_passphrase\",");
    if (e == ESP_OK) {
        e = send_frag(req, "\"running\":\"%s\",\"uptime_s\":%u,\"reset_reason\":%d,"
                           "\"reset_reason_name\":\"%s\",",
                      running ? running->label : "?",
                      (unsigned)(esp_timer_get_time() / 1000000), rr,
                      recovery_reset_reason_name(rr));
    }
    if (e == ESP_OK) {
        e = send_frag(req, "\"app_present\":%s,\"app_size\":%u,\"app_desc_present\":%s,"
                           "\"app_valid\":%s,\"max_upload\":%u,",
                      app ? "true" : "false", app_size, desc_present ? "true" : "false",
                      valid ? "true" : "false", app_size);
    }
    if (e == ESP_OK) {
        e = img_len ? send_frag(req, "\"app_image_size\":%u,", img_len)
                    : send_frag(req, "\"app_image_size\":null,");
    }
    if (e == ESP_OK) {
        e = send_frag(req, "\"app_ota_state\":\"%s\",\"coredump_present\":%s,"
                           "\"otadata_blank\":%s,",
                      ota_state_name(app), tri(coredump_present()), tri(otadata_blank()));
    }
    if (e == ESP_OK) {
        recovery_wifi_stats_t ws;
        recovery_wifi_get_stats(&ws);
        e = send_frag(req, "\"wifi_up\":%s,\"ap_start_count\":%u,\"ap_stop_count\":%u,"
                           "\"ap_stations\":%u,",
                      recovery_wifi_is_up() ? "true" : "false", (unsigned)ws.ap_start_count,
                      (unsigned)ws.ap_stop_count, (unsigned)ws.ap_sta_now);
        if (e == ESP_OK) {
            e = send_frag(req, "\"ap_connect_total\":%u,\"wifi_last_event\":\"%s\","
                               "\"wifi_last_event_age_s\":%u,",
                          (unsigned)ws.ap_sta_connect_total, ws.last_event_name,
                          (unsigned)ws.last_event_age_s);
        }
    }
    if (e == ESP_OK) {
        // "error": null, or the static string from recovery_wifi_error()
        // ("wifi_storage_fail": the SoftAP was refused, see recovery_wifi.c).
        const char *werr = recovery_wifi_error();
        e = werr ? send_frag(req, "\"error\":\"%s\",", werr) : send_frag(req, "\"error\":null,");
        if (e == ESP_OK) {
            const char *he = s_http_last_error;
            e = send_frag(req, "\"http_start_attempts\":%u,\"http_last_error\":", (unsigned)s_http_failed_attempts);
            if (e == ESP_OK) {
                e = he ? send_frag(req, "\"%s\",", he) : send_frag(req, "null,");
            }
        }
    }
    if (e == ESP_OK) {
        e = send_frag(req, "%s,", bgs);
    }
    if (e == ESP_OK) {
        e = send_frag(req, "\"relay_fault\":%s,\"relays_verified_off\":%s,",
                      recovery_io_relay_fault() ? "true" : "false",
                      recovery_io_relays_verified_off() ? "true" : "false");
    }
    if (e == ESP_OK) {
        // Sticky boot-time record (never cleared by later hold-task recovery); a
        // bring-up failure also means no hold task was started.
        esp_err_t ie = recovery_io_init_error();
        e = ie == ESP_OK ? send_frag(req, "\"relay_io_init_error\":null,")
                         : send_frag(req, "\"relay_io_init_error\":\"%s\",", esp_err_to_name(ie));
    }
    if (e == ESP_OK) {
        recovery_io_hold_status_t hs;
        recovery_io_hold_status(&hs);
        char fs_buf[16], ok_buf[16];
        if (hs.fault_valid) {
            snprintf(fs_buf, sizeof(fs_buf), "%u", (unsigned)hs.fault_s);
        } else {
            snprintf(fs_buf, sizeof(fs_buf), "null");
        }
        if (hs.last_ok_valid) {
            snprintf(ok_buf, sizeof(ok_buf), "%u", (unsigned)hs.last_ok_s);
        } else {
            snprintf(ok_buf, sizeof(ok_buf), "null");
        }
        e = send_frag(req, "\"relay_hold_task\":%s,\"relay_hold_fault\":%s,"
                           "\"relay_hold_fault_s\":%s,\"relay_hold_last_ok_s\":%s,",
                      hs.task_running ? "true" : "false", hs.fault ? "true" : "false", fs_buf,
                      ok_buf);
        if (e == ESP_OK) {
            e = send_frag(req, "\"relay_hold_mismatches\":%u,\"relay_hold_reassert_fails\":%u,"
                               "\"relay_hold_stack_free\":%u,",
                          (unsigned)hs.mismatch_count, (unsigned)hs.reassert_fail_count,
                          (unsigned)hs.task_stack_free_bytes);
        }
    }
    if (e == ESP_OK) {
        recovery_lcd_status_t ls;
        recovery_lcd_get_status(&ls);
        // lcd_ready = driver path OK only: the panel is write-only (no MISO), so a
        // dead panel is undetectable and this does not prove the passphrase is
        // visible.
        e = send_frag(req, "\"lcd_ready\":%s,\"lcd_init_attempts\":%u,\"lcd_draw_failures\":%u,"
                           "\"lcd_task_stack_free_bytes\":%u,",
                      ls.ready ? "true" : "false", (unsigned)ls.init_attempts,
                      (unsigned)ls.draw_failures, (unsigned)ls.task_stack_free_bytes);
    }
    if (e == ESP_OK) {
        e = send_frag(req, "\"nvs_unavailable\":%s,\"nvs_failed_mask\":%u,"
                           "\"heap_internal_min_free\":%u,\"free_heap\":%u}",
                      nvs_failed ? "true" : "false", nvs_failed,
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)esp_get_free_heap_size());
    }
    if (e != ESP_OK) {
        return ESP_FAIL; // truncated stream: close the connection
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Erase one key; ESP_ERR_NVS_NOT_FOUND (key or namespace) counts as success.
static esp_err_t erase_key_in(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// Read back: true only when the key is positively absent (namespace missing
// counts as absent). Any other error reads as "not verified".
static bool key_absent(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    nvs_type_t type;
    err = nvs_find_key(h, key, &type);
    nvs_close(h);
    return err == ESP_ERR_NVS_NOT_FOUND;
}

// Clears the boot_guard record (current and legacy locations), then reads both
// back. Returns true only when every erase succeeded AND the read-back shows
// the keys absent. `msg` always receives a short human-readable outcome
// (never silent success, never silent failure).
static bool clear_boot_guard(char *msg, size_t cap)
{
    esp_err_t e1 = erase_key_in(BOOT_GUARD_NAMESPACE, BOOT_GUARD_KEY);
    esp_err_t e2 = erase_key_in(BOOT_GUARD_NAMESPACE_LEGACY, BOOT_GUARD_KEY_LEGACY);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        snprintf(msg, cap, "boot_guard clear failed (erase: %s / legacy: %s)", esp_err_to_name(e1),
                 esp_err_to_name(e2));
        ESP_LOGW(TAG, "%s", msg);
        return false;
    }
    if (!key_absent(BOOT_GUARD_NAMESPACE, BOOT_GUARD_KEY) ||
        !key_absent(BOOT_GUARD_NAMESPACE_LEGACY, BOOT_GUARD_KEY_LEGACY)) {
        snprintf(msg, cap, "boot_guard clear failed (erased but read-back not verified)");
        ESP_LOGW(TAG, "%s", msg);
        return false;
    }
    snprintf(msg, cap, "boot_guard cleared and verified");
    return true;
}

// POST /api/ota/esp/boot_guard_reset
static esp_err_t boot_guard_reset_post(httpd_req_t *req)
{
    char bg_msg[96];
    if (!clear_boot_guard(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, bg_msg, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_sendstr(req, bg_msg);
}

// POST /api/recovery/exit -- boot the application: refused (409) unless `app`
// holds a valid image. Clears the boot_guard counter so the next boot is not
// itself counted toward recovery, selects `app`, and restarts.
static esp_err_t recovery_exit_post(httpd_req_t *req)
{
    if (recovery_pico_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "Pico update in progress", HTTPD_RESP_USE_STRLEN);
    }
    const esp_partition_t *app = find_app_partition();
    if (!app_has_valid_image(app) || !app_image_verified(app)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "no valid application image in app", HTTPD_RESP_USE_STRLEN);
    }
    // Clear the counter BEFORE touching the boot target: an unverified clear
    // fails the exit (500) with nothing changed, instead of booting the app
    // into a counter that walks the board straight back into recovery.
    char bg_msg[96];
    if (!clear_boot_guard(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, bg_msg, HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t serr = esp_ota_set_boot_partition(app);
    if (serr == ESP_ERR_OTA_VALIDATE_FAILED) {
        app_verify_invalidate();
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "application image failed validation", HTTPD_RESP_USE_STRLEN);
    }
    if (serr != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "esp_ota_set_boot_partition failed", HTTPD_RESP_USE_STRLEN);
    }
    char body[160];
    snprintf(body, sizeof(body), "ok, rebooting into the application; %s", bg_msg);
    esp_err_t sent = httpd_resp_sendstr(req, body);
    restart_soon(500);
    return sent;
}

// POST /api/recovery/wifi_reset -- forget the HOME network credentials
// (WIFI_RESET_KEYS) and restart. The AP name is kept.
static esp_err_t wifi_reset_post(httpd_req_t *req)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, NVS_READWRITE,
                                             &h);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "could not open the Wi-Fi settings store",
                                HTTPD_RESP_USE_STRLEN);
    }
    // Try every key (so as much as possible is cleared), but any erase error
    // makes the route fail: reporting "cleared" while a credential survived
    // would send the operator back to a network they believe is forgotten.
    size_t erase_failed = 0;
    const char *first_failed = NULL;
    for (size_t i = 0; i < sizeof(WIFI_RESET_KEYS) / sizeof(WIFI_RESET_KEYS[0]); i++) {
        esp_err_t e = nvs_erase_key(h, WIFI_RESET_KEYS[i]);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "wifi_reset: erase %s failed: %s", WIFI_RESET_KEYS[i], esp_err_to_name(e));
            if (!first_failed) {
                first_failed = WIFI_RESET_KEYS[i];
            }
            erase_failed++;
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (erase_failed > 0) {
        char fmsg[96];
        snprintf(fmsg, sizeof(fmsg), "Wi-Fi settings erase failed for %u key(s), first: %s",
                 (unsigned)erase_failed, first_failed);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, fmsg, HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Wi-Fi settings commit failed", HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t sent = httpd_resp_sendstr(req, "ok, Wi-Fi settings cleared, restarting");
    restart_soon(500);
    return sent;
}

// POST /api/sw_reset
static esp_err_t sw_reset_post(httpd_req_t *req)
{
    if (recovery_pico_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "Pico update in progress", HTTPD_RESP_USE_STRLEN);
    }

    // Information only. sw_reset deliberately never writes otadata (not even
    // esp_ota_set_boot_partition(factory), which erases it): the bootloader
    // already falls back to the factory (recovery) partition for an invalid
    // app image, and a blank otadata is a hazard for a later JTAG flash.
    const esp_partition_t *app = find_app_partition();
    bool app_ok = app_has_valid_image(app) && app_image_verified(app);
    const char *note = app_ok ? "resetting"
                              : "resetting (no valid application image: the bootloader will "
                                "fall back to recovery)";
    if (!app_ok) {
        ESP_LOGW(TAG, "sw_reset: app image does not verify; boot target left unchanged");
    }

    httpd_resp_sendstr(req, note);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

// Reads exactly `want` body bytes into buf. Returns bytes read (less on a dead
// or stalled connection).
static size_t read_body_exact(httpd_req_t *req, uint8_t *buf, size_t want)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < want) {
        int n = httpd_req_recv(req, (char *)buf + got, want - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 20) {
                break;
            }
            continue;
        }
        if (n <= 0) {
            break;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    return got;
}

// Parses "?crc=<8 hex>[&slot=A|B|auto]". crc is required.
static bool parse_pico_query(httpd_req_t *req, uint32_t *crc, int *operator_slot)
{
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    char v[16];
    if (httpd_query_key_value(q, "crc", v, sizeof(v)) != ESP_OK || strlen(v) != 8) {
        return false;
    }
    uint32_t c = 0;
    for (int i = 0; i < 8; i++) {
        char ch = v[i];
        int d = (ch >= '0' && ch <= '9')   ? ch - '0'
                : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                           : -1;
        if (d < 0) {
            return false;
        }
        c = (c << 4) | (uint32_t)d;
    }
    *crc = c;
    *operator_slot = RPP_SLOT_UNKNOWN;
    if (httpd_query_key_value(q, "slot", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, "A") == 0 || strcmp(v, "a") == 0) {
            *operator_slot = RPP_SLOT_A;
        } else if (strcmp(v, "B") == 0 || strcmp(v, "b") == 0) {
            *operator_slot = RPP_SLOT_B;
        }
    }
    return true;
}

// POST /api/recovery/pico/upload?crc=<hex>[&slot=A|B] -- open. The
// body (a SaftyFW slot image) is received whole into PSRAM, validated (size,
// vectors against the target slot, CRC32 recomputed here) and only then handed
// to the relay task (recovery_pico.c), which does the erase/transfer over the
// UART while GET /api/recovery/pico/status keeps answering. 202 on start.
static esp_err_t pico_upload_post(httpd_req_t *req)
{
    uint32_t claimed_crc = 0;
    int operator_slot = RPP_SLOT_UNKNOWN;
    if (!parse_pico_query(req, &claimed_crc, &operator_slot)) {
        return recovery_upload_send_error(req, 400, "want ?crc=<8 hex digits>[&slot=A|B]");
    }
    size_t len = req->content_len;
    int http_status = 503;
    const char *why = "unavailable";
    uint8_t *buf = recovery_pico_reserve(len, &http_status, &why);
    if (!buf) {
        return recovery_upload_send_error(req, http_status, why);
    }
    if (read_body_exact(req, buf, len) != len) {
        recovery_pico_release();
        return recovery_upload_send_error(req, 400,
                                          "upload interrupted before the whole image arrived");
    }
    int image_slot = RPP_SLOT_UNKNOWN;
    rpp_image_result_t ir = rpp_check_image(buf, len, claimed_crc, &image_slot);
    if (ir != RPP_IMG_OK) {
        recovery_pico_release();
        return recovery_upload_send_error(req, 422, rpp_image_result_str(ir));
    }
    if (!recovery_pico_start(len, claimed_crc, image_slot, operator_slot, &why)) {
        return recovery_upload_send_error(req, 503, why);
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    char body[96];
    int n = snprintf(body, sizeof(body), "{\"started\":true,\"image_slot\":\"%s\"}",
                     image_slot == RPP_SLOT_A ? "A" : "B");
    return httpd_resp_send(req, body, n);
}

// GET /api/recovery/pico/status -- read-only. Polling it is
// also the relay's "the browser is still here" signal.
static esp_err_t pico_status_get(httpd_req_t *req)
{
    int n = 0;
    const char *body = recovery_pico_status_json(&n);
    if (!body || n <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "status unavailable", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

// POST /api/recovery/pico/abort -- open. Asks the relay to stop; it
// sends ABORT to the Pico and ends in phase "aborted". Harmless when idle.
static esp_err_t pico_abort_post(httpd_req_t *req)
{
    recovery_pico_abort();
    return httpd_resp_sendstr(req, "abort requested");
}

esp_err_t recovery_http_start(void)
{
    // Handlers' static buffers (send_frag's frag, s_verify_*) assume every
    // handler runs on this ONE httpd task. Starting a second server would
    // silently break that, so refuse it outright. Set only once a start
    // succeeds, so a failed attempt (which stops its server) can be retried.
    static bool s_started;
    configASSERT(!s_started);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    // 12 routes below (3 are the Pico update relay).
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t start_rc = httpd_start(&server, &config);

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/recovery/status", .method = HTTP_GET, .handler = recovery_status_get},
        {.uri = "/api/recovery/exit", .method = HTTP_POST, .handler = recovery_exit_post},
        {.uri = "/api/recovery/wifi_reset", .method = HTTP_POST, .handler = wifi_reset_post},
        {.uri = "/api/ota/esp", .method = HTTP_POST, .handler = ota_esp_post},
        {.uri = "/api/partitions", .method = HTTP_GET, .handler = partitions_get},
        {.uri = "/api/boot_guard", .method = HTTP_GET, .handler = boot_guard_get},
        {.uri = "/api/ota/esp/boot_guard_reset", .method = HTTP_POST,
         .handler = boot_guard_reset_post},
        {.uri = "/api/sw_reset", .method = HTTP_POST, .handler = sw_reset_post},
        {.uri = "/api/recovery/pico/upload", .method = HTTP_POST, .handler = pico_upload_post},
        {.uri = "/api/recovery/pico/status", .method = HTTP_GET, .handler = pico_status_get},
        {.uri = "/api/recovery/pico/abort", .method = HTTP_POST, .handler = pico_abort_post},
    };
    const unsigned expected = (unsigned)(sizeof(routes) / sizeof(routes[0]));
    _Static_assert(sizeof(routes) / sizeof(routes[0]) <= 16, "routes exceed max_uri_handlers");
    unsigned registered = 0;
    if (start_rc == ESP_OK) {
        for (size_t i = 0; i < expected; i++) {
            esp_err_t rr = httpd_register_uri_handler(server, &routes[i]);
            if (rr == ESP_OK) {
                registered++;
            } else {
                ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(rr));
            }
        }
    }
    if (!rhealth_http_ok((int)start_rc, registered, expected)) {
        s_http_failed_attempts++;
        s_http_last_error = start_rc != ESP_OK ? "http_start_fail" : "route_register_fail";
        ESP_LOGE(TAG, "HTTP FAILED: httpd_start=%s, %u/%u routes registered (%s)",
                 esp_err_to_name(start_rc), registered, expected, s_http_last_error);
        if (start_rc == ESP_OK) {
            (void)httpd_stop(server);
        }
        // No LCD banner here: the caller retries, and only a FINAL failure may show one.
        return start_rc != ESP_OK ? start_rc : ESP_FAIL;
    }
    s_started = true;
    ESP_LOGI(TAG, "recovery httpd up, %u routes", expected);
    return ESP_OK;
}
