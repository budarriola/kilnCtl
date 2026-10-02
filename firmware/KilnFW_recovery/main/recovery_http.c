// recovery_http.c -- see recovery_http.h.
//
// Auth mechanism: docs/OTA_SINGLE_SLOT_PLAN.md section 3 item 3 /
// firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 2 --
//   key = HMAC-SHA256(ap_password, "kilnctl-ota-v1")
//   mac = HMAC-SHA256(key, nonce || context)
// where context is "esp" for POST /api/ota/esp, "boot-guard-reset" for POST
// /api/ota/esp/boot_guard_reset, "sw-reset" for POST /api/sw_reset,
// "recovery-exit" for POST /api/recovery/exit and "wifi-reset" for POST
// /api/recovery/wifi_reset -- matching recovery_ota_auth_client.py's
// derive_mac() _VALID_CONTEXTS and the browser page's deriveMac() calls
// exactly (see recovery_authenticate_request()'s own comment for the single
// shared lockout state).
// carried in the request as header "X-Ota-Mac", hex-encoded, checked
// against a single-use, 30s-expiry nonce from GET /api/ota/challenge via
// the same ota_auth.c state machine the main KilnFW image uses (copied in
// verbatim -- see main/ota_auth.c's header for why it is pure/host-testable
// and therefore safe to reuse unmodified rather than re-derive).
//
// Every mutating POST route in this file (POST /api/ota/esp, POST
// /api/ota/esp/boot_guard_reset, POST /api/sw_reset, POST /api/recovery/exit,
// POST /api/recovery/wifi_reset) authenticates via the
// single shared recovery_authenticate_request() below -- 2026-09-19 closed
// docs/audits/web_code_duplication_drift_2026-09-18.md section 2.2's open
// follow-up, which found the latter two routes had no X-Ota-Mac check at
// all. A new mutating route must call it too, or
// check_recovery_ota_auth_mirror.ps1's route-coverage assertion fails.
//
// SCOPE OF THIS PASS: uses mbedtls's classic mbedtls_md_hmac() API rather
// than the main app's PSA Crypto calls (psa_import_key()/psa_mac_compute())
// -- both compute the same standard HMAC-SHA256, but PSA needs its own
// key-slot lifecycle that adds bring-up surface with no correctness benefit
// for a single, short-lived, never-persisted key. Documented here rather
// than silently diverging from the plan's "PSA HMAC-SHA256" phrasing in
// section 1/3: the primitive is identical, only the calling convention
// differs.
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
// recovery_upload.c; a new authenticated upload route authenticates here
// with recovery_authenticate_request() and then calls
// recovery_upload_stream() with its own validator and sink.
#include "recovery_http.h"

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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "ota_auth.h"
#include "recovery_image_check.h"
#include "recovery_upload.h"
#include "recovery_wifi.h"

static const char *TAG = "recovery_http";

// Defined by recovery_io.c (relay-forced-off driver) once that lands; a weak
// undefined reference resolves to NULL until then, which reads as "no fault".
// INTEGRATION TODO: this MUST become a strong declaration (#include
// "recovery_io.h") when recovery_io.c is merged. A weak reference alone never
// pulls an archive member out of a static library: if recovery_io.c ends up in
// libmain.a (or any .a) and nothing else references it, the linker leaves this
// symbol NULL and relay_fault silently reads "false" even with a real fault.
extern bool recovery_io_relay_fault(void) __attribute__((weak));

// The page (recovery_page.html) is linked in via EMBED_FILES.
extern const uint8_t recovery_page_html_start[] asm("_binary_recovery_page_html_start");
extern const uint8_t recovery_page_html_end[] asm("_binary_recovery_page_html_end");

#define APP_PARTITION_LABEL "app"
// Wi-Fi namespace shared with the main app (wifi_prov_nvs.c NVS_NAMESPACE) and
// the keys that describe the HOME network / addressing. ap_ssid/ap_pass are
// deliberately NOT erased: ap_pass is the HMAC key material, so erasing it
// would make this image's own authenticated routes unusable (500).
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

static ota_auth_nonce_state_t s_nonce;
// ONE lockout counter shared by every authenticated route (owner/review
// decision 2026-10-02, reversing the earlier per-route split): all five routes
// guard the same secret (the AP-password-derived key), so a guesser spreading
// attempts across routes must not get N times the attempts. Failed MACs on any
// route count toward the same lockout.
static ota_auth_lockout_state_t s_lockout;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

// Mirrors firmware/KilnFW/App/drivers/http/ota_http_util.c's
// ota_http_hex_decode() nibble-by-nibble: strict [0-9a-fA-F] table, no
// sscanf (which also accepts a leading sign/space/"0x" per byte). Kept
// textually identical to that function's per-nibble logic on purpose --
// see check_recovery_ota_auth_mirror.ps1, which diffs this against it so
// the two cannot silently diverge again. Length here is always out_len*2
// (the caller already enforced the header's exact 64-char length before
// calling this), unlike ota_http_hex_decode() which takes hex_len
// explicitly; the decode logic itself is the same.
static bool hex_decode(const char *in, uint8_t *out, size_t out_len)
{
    if (strlen(in) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = -1, lo = -1;
        char ch = in[2 * i];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        ch = in[2 * i + 1];
        if (ch >= '0' && ch <= '9') lo = ch - '0';
        else if (ch >= 'a' && ch <= 'f') lo = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') lo = ch - 'A' + 10;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Volatile-pointer wipe the compiler cannot elide (explicit_bzero is not
// guaranteed in this libc configuration).
static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32])
{
    // Both mbedtls_md_hmac() (one-shot) and the mbedtls_md_hmac_starts/
    // _update/_finish() family are gated behind
    // MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS in this IDF's mbedtls 4.x headers
    // and are not visible here. Build the standard HMAC construction
    // (RFC 2104) directly from the always-public plain-hash
    // mbedtls_md_starts/update/finish -- same classic HMAC-SHA256, no
    // gated API, no PSA key-slot lifecycle (see the file header comment on
    // that scope decision).
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const size_t block_len = 64; // SHA-256 block size
    uint8_t key_block[64] = {0};
    if (key_len > block_len) {
        mbedtls_md_context_t kctx;
        mbedtls_md_init(&kctx);
        mbedtls_md_setup(&kctx, info, 0);
        mbedtls_md_starts(&kctx);
        mbedtls_md_update(&kctx, key, key_len);
        mbedtls_md_finish(&kctx, key_block); // 32 bytes, rest stays zero
        mbedtls_md_free(&kctx);
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < block_len; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    uint8_t inner[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, ipad, block_len);
    mbedtls_md_update(&ctx, msg, msg_len);
    mbedtls_md_finish(&ctx, inner);
    mbedtls_md_free(&ctx);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, opad, block_len);
    mbedtls_md_update(&ctx, inner, sizeof(inner));
    mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);

    secure_zero(key_block, sizeof(key_block));
    secure_zero(ipad, sizeof(ipad));
    secure_zero(opad, sizeof(opad));
    secure_zero(inner, sizeof(inner));
}

// GET /api/ota/challenge
static esp_err_t challenge_get(httpd_req_t *req)
{
    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));
    ota_auth_nonce_issue(&s_nonce, nonce, now_ms());

    char hex[OTA_AUTH_NONCE_LEN * 2 + 1];
    hex_encode(nonce, sizeof(nonce), hex);

    char body[64];
    int n = snprintf(body, sizeof(body), "{\"nonce\":\"%s\"}", hex);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

// Shared X-Ota-Mac authentication sequence -- every mutating recovery-image
// POST route (POST /api/ota/esp, POST /api/ota/esp/boot_guard_reset,
// POST /api/sw_reset) must call this before acting, rather than hand-
// rolling the header/hex/lockout/nonce/password/HMAC sequence again. This
// used to live inline in ota_esp_post() only -- the other two routes had
// no auth check at all
// (docs/audits/web_code_duplication_drift_2026-09-18.md section 2.2's open
// follow-up, closed here). Factoring it out means a future new mutating
// route gets this for free by calling it, instead of becoming a ninth
// hand-copy.
//
// `context` is the per-route HMAC context string appended after the nonce
// ("esp" / "boot-guard-reset" / "sw-reset" / ... -- must match the caller's
// route exactly, see the file header comment) and `lockout` is the ONE shared
// ota_auth_lockout_state_t (s_lockout): every route passes the same instance,
// so failed MACs against any route count toward a single lockout.
//
// On success: returns true, sends nothing (caller proceeds).
// On failure: returns false, having already sent the appropriate error
// status/body itself via *out_err -- caller must return *out_err
// immediately without sending anything further.
//
// check_recovery_ota_auth_mirror.ps1 diffs this function's body (not
// ota_esp_post()'s, now just a caller) against ota_http_authenticate_
// request()/ota_http_hex_decode() in the main app -- see that check's
// header comment before changing wire strings or ordering here.
static bool recovery_authenticate_request(httpd_req_t *req, esp_err_t *out_err,
                                           const char *context, ota_auth_lockout_state_t *lockout)
{
    uint32_t t = now_ms();

    // Header well-formed BEFORE lockout, matching ota_http_authenticate_
    // request()'s ordering (ota_http.c:935-966): a headerless/malformed
    // request is a client mistake (400), not a wrong-password guess, and
    // must not be answered with the same "locked out" status a genuine
    // failed-MAC attempt gets, nor be allowed to probe lockout state
    // without ever presenting a candidate MAC.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, "X-Ota-Mac");
    if (mac_hex_len != 64) {
        httpd_resp_set_status(req, "400 Bad Request");
        *out_err = httpd_resp_send(req, "missing or malformed X-Ota-Mac header (want 64 hex chars)",
                                    HTTPD_RESP_USE_STRLEN);
        return false;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, "X-Ota-Mac", mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        *out_err = httpd_resp_send(req, "could not read X-Ota-Mac header", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    uint8_t claimed_mac[32];
    if (!hex_decode(mac_hex, claimed_mac, sizeof(claimed_mac))) {
        httpd_resp_set_status(req, "400 Bad Request");
        *out_err = httpd_resp_send(req, "X-Ota-Mac must be 64 hex characters", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    if (ota_auth_lockout_is_locked(lockout, t)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        *out_err = httpd_resp_send(req, "locked out, retry later", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    ota_auth_nonce_check_t check = ota_auth_nonce_check(&s_nonce, t);
    if (check != OTA_AUTH_NONCE_OK) {
        httpd_resp_set_status(req, "403 Forbidden");
        *out_err = httpd_resp_send(req, "no valid challenge outstanding", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    char ap_password[65];
    if (!recovery_wifi_get_ap_password(ap_password, sizeof(ap_password))) {
        ota_auth_nonce_invalidate(&s_nonce);
        secure_zero(ap_password, sizeof(ap_password));
        httpd_resp_set_status(req, "500 Internal Server Error");
        *out_err = httpd_resp_send(req, "no AP password on record", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    uint8_t key[32];
    hmac_sha256((const uint8_t *)ap_password, strlen(ap_password),
                (const uint8_t *)"kilnctl-ota-v1", strlen("kilnctl-ota-v1"), key);

    // Sized for the longest of the three known context literals,
    // "boot-guard-reset" (16 chars, no NUL -- this buffer is never treated
    // as a C string). The _Static_assert()s below pin all three literals'
    // lengths so this comment can't silently go stale if one is renamed.
    // The runtime bounds check right after guards any FUTURE context this
    // helper is called with that the static asserts don't know about --
    // belt and suspenders, since `context` is caller-supplied even though
    // every current caller is a literal in this same file.
    _Static_assert(sizeof("esp") - 1 <= 16, "context literal exceeds msg[] headroom");
    _Static_assert(sizeof("boot-guard-reset") - 1 <= 16, "context literal exceeds msg[] headroom");
    _Static_assert(sizeof("sw-reset") - 1 <= 16, "context literal exceeds msg[] headroom");
    _Static_assert(sizeof("recovery-exit") - 1 <= 16, "context literal exceeds msg[] headroom");
    _Static_assert(sizeof("wifi-reset") - 1 <= 16, "context literal exceeds msg[] headroom");
    size_t context_len = strlen(context);
    uint8_t msg[OTA_AUTH_NONCE_LEN + 16];
    if (context_len > sizeof(msg) - OTA_AUTH_NONCE_LEN) {
        secure_zero(ap_password, sizeof(ap_password));
        secure_zero(key, sizeof(key));
        // Cannot happen with today's three call sites (all compile-time
        // literals covered by the _Static_assert()s above) -- this guards
        // only against a future caller passing a longer context without
        // also growing msg[], which would otherwise overflow it.
        httpd_resp_set_status(req, "500 Internal Server Error");
        *out_err = httpd_resp_send(req, "internal error: auth context too long",
                                    HTTPD_RESP_USE_STRLEN);
        return false;
    }
    memcpy(msg, s_nonce.nonce, OTA_AUTH_NONCE_LEN);
    memcpy(msg + OTA_AUTH_NONCE_LEN, context, context_len);
    uint8_t expected_mac[32];
    hmac_sha256(key, sizeof(key), msg, OTA_AUTH_NONCE_LEN + context_len, expected_mac);
    // Key material is no longer needed; wipe before any return path.
    secure_zero(ap_password, sizeof(ap_password));
    secure_zero(key, sizeof(key));

    // Invalidate the nonce unconditionally before deciding pass/fail --
    // UPDATE_PROTOCOL.md section 2 step 4.
    ota_auth_nonce_invalidate(&s_nonce);

    bool mac_ok = ota_auth_constant_time_equal(claimed_mac, expected_mac, sizeof(expected_mac));
    secure_zero(expected_mac, sizeof(expected_mac));
    if (!mac_ok) {
        ota_auth_lockout_record_failure(lockout, t);
        httpd_resp_set_status(req, "403 Forbidden");
        *out_err = httpd_resp_send(req, "bad MAC", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    ota_auth_lockout_record_success(lockout);
    return true;
}

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
    esp_app_desc_t desc;
    return part && esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

// --- full-image verification (cached) ------------------------------------
// esp_image_verify() on `app` reads the whole image (~2.5 MB of flash) and
// hashes it, so it must never run per status GET. The result is cached and
// invalidated when an upload starts or ends; the next status/exit read refills
// it lazily. httpd is effectively single-task here, so no lock is needed.
static bool s_verify_known;
static bool s_verify_valid;

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
        s_verify_known = true;
    }
    return s_verify_valid;
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

// POST /api/ota/esp -- authenticated, validated, streamed (recovery_upload.c).
// Nothing is written until the first chunk passes ric_validate_first_chunk();
// the boot partition is set only after esp_ota_end() verified the whole image.
static esp_err_t ota_esp_post(httpd_req_t *req)
{
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "esp", &s_lockout)) {
        return auth_err;
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
} boot_guard_info_t;

static void read_boot_guard(boot_guard_info_t *info)
{
    memset(info, 0, sizeof(*info));
    nvs_handle_t h;
    if (nvs_open_from_partition(KILN_NVS_PARTITION, BOOT_GUARD_NAMESPACE, NVS_READONLY, &h) !=
        ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, BOOT_GUARD_KEY, NULL, &info->len) == ESP_OK) {
        info->present = true;
        uint8_t blob[16];
        size_t blen = sizeof(blob);
        if (info->len <= sizeof(blob) &&
            nvs_get_blob(h, BOOT_GUARD_KEY, blob, &blen) == ESP_OK) {
            info->count_valid = ric_boot_guard_decode(blob, blen, &info->count) != 0;
        }
    }
    nvs_close(h);
}

// Formats "record_present":..,"record_len":..[,"boot_count":N]; boot_count
// only when the record decoded cleanly (never guessed).
static int fmt_boot_guard(char *out, size_t cap, const boot_guard_info_t *bg)
{
    int n = snprintf(out, cap, "\"record_present\":%s,\"record_len\":%u",
                      bg->present ? "true" : "false", (unsigned)bg->len);
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

// GET /api/recovery/status -- unauthenticated, read-only.
static esp_err_t recovery_status_get(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app = find_app_partition();
    boot_guard_info_t bg;
    read_boot_guard(&bg);
    char bgs[128];
    fmt_boot_guard(bgs, sizeof(bgs), &bg);

    bool relay_fault = recovery_io_relay_fault ? recovery_io_relay_fault() : false;

    // app_desc_present: cheap descriptor/magic check. app_valid: full
    // esp_image_verify(), cached (see app_image_verified()).
    bool desc_present = app_has_valid_image(app);
    bool valid = desc_present && app_image_verified(app);
    char body[480];
    int n = snprintf(body, sizeof(body),
                      "{\"running\":\"%s\",\"app_present\":%s,\"app_size\":%u,"
                      "\"app_desc_present\":%s,\"app_valid\":%s,\"max_upload\":%u,%s,"
                      "\"relay_fault\":%s,\"free_heap\":%u}",
                      running ? running->label : "?", app ? "true" : "false",
                      (unsigned)(app ? app->size : 0), desc_present ? "true" : "false",
                      valid ? "true" : "false",
                      (unsigned)(app ? app->size : 0), bgs, relay_fault ? "true" : "false",
                      (unsigned)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
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
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "boot-guard-reset", &s_lockout)) {
        return auth_err;
    }
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
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "recovery-exit", &s_lockout)) {
        return auth_err;
    }
    const esp_partition_t *app = find_app_partition();
    if (!app_has_valid_image(app) || !app_image_verified(app)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "no valid application image in app", HTTPD_RESP_USE_STRLEN);
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
    // Exit still proceeds if the clear fails, but the response says so.
    char bg_msg[96];
    clear_boot_guard(bg_msg, sizeof(bg_msg));
    char body[160];
    snprintf(body, sizeof(body), "ok, rebooting into the application; %s", bg_msg);
    esp_err_t sent = httpd_resp_sendstr(req, body);
    restart_soon(500);
    return sent;
}

// POST /api/recovery/wifi_reset -- forget the HOME network credentials
// (WIFI_RESET_KEYS) and restart. The AP name/password are kept: the password
// is this image's HMAC key.
static esp_err_t wifi_reset_post(httpd_req_t *req)
{
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "wifi-reset", &s_lockout)) {
        return auth_err;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, NVS_READWRITE,
                                             &h);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "could not open the Wi-Fi settings store",
                                HTTPD_RESP_USE_STRLEN);
    }
    for (size_t i = 0; i < sizeof(WIFI_RESET_KEYS) / sizeof(WIFI_RESET_KEYS[0]); i++) {
        esp_err_t e = nvs_erase_key(h, WIFI_RESET_KEYS[i]);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "wifi_reset: erase %s failed: %s", WIFI_RESET_KEYS[i], esp_err_to_name(e));
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
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
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "sw-reset", &s_lockout)) {
        return auth_err;
    }

    httpd_resp_sendstr(req, "resetting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

void recovery_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    // 10 routes below; headroom for the later Pico upload route.
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/recovery/status", .method = HTTP_GET, .handler = recovery_status_get},
        {.uri = "/api/recovery/exit", .method = HTTP_POST, .handler = recovery_exit_post},
        {.uri = "/api/recovery/wifi_reset", .method = HTTP_POST, .handler = wifi_reset_post},
        {.uri = "/api/ota/challenge", .method = HTTP_GET, .handler = challenge_get},
        {.uri = "/api/ota/esp", .method = HTTP_POST, .handler = ota_esp_post},
        {.uri = "/api/partitions", .method = HTTP_GET, .handler = partitions_get},
        {.uri = "/api/boot_guard", .method = HTTP_GET, .handler = boot_guard_get},
        {.uri = "/api/ota/esp/boot_guard_reset", .method = HTTP_POST,
         .handler = boot_guard_reset_post},
        {.uri = "/api/sw_reset", .method = HTTP_POST, .handler = sw_reset_post},
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) <= 16, "routes exceed max_uri_handlers");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "recovery httpd up, %u routes", (unsigned)(sizeof(routes) / sizeof(routes[0])));
}
