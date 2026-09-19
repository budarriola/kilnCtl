// recovery_http.c -- see recovery_http.h.
//
// Auth mechanism: docs/OTA_SINGLE_SLOT_PLAN.md section 3 item 3 /
// firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 2 --
//   key = HMAC-SHA256(ap_password, "kilnctl-ota-v1")
//   mac = HMAC-SHA256(key, nonce || context)
// where context is "esp" for POST /api/ota/esp, "boot-guard-reset" for POST
// /api/ota/esp/boot_guard_reset, and "sw-reset" for POST /api/sw_reset --
// matching ota_http.c's per-route OTA_HTTP_CONTEXT_* strings and
// ota_http_client.py's derive_mac() calls exactly (see
// recovery_authenticate_request()'s own comment for the per-route lockout
// state that goes with each context).
// carried in the request as header "X-Ota-Mac", hex-encoded, checked
// against a single-use, 30s-expiry nonce from GET /api/ota/challenge via
// the same ota_auth.c state machine the main KilnFW image uses (copied in
// verbatim -- see main/ota_auth.c's header for why it is pure/host-testable
// and therefore safe to reuse unmodified rather than re-derive).
//
// Every mutating POST route in this file (POST /api/ota/esp, POST
// /api/ota/esp/boot_guard_reset, POST /api/sw_reset) authenticates via the
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
// GET /api/boot_guard reports whether the shared `kiln_cfg`/"bootguard"
// NVS record exists and its raw length, but does NOT decode its fields --
// decoding a struct layout owned by firmware/KilnFW/App/drivers/persist/
// boot_guard.c from this independent project risks exactly the kind of
// silent two-copies-of-one-format drift CLAUDE.md's "reset one side of a
// pair" class describes, and a wrong decode here would misreport an
// operator's boot-guard state during exactly the moment they are relying on
// it. POST .../boot_guard_reset only erases the key (nvs_erase_key) rather
// than writing a specific "cleared" struct value for the same reason --
// erasing is format-agnostic and every known reader (boot_guard.c's own
// hal_kv_open/get path) already treats "key not found" as counter-zero.
#include "recovery_http.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "ota_auth.h"
#include "recovery_wifi.h"

static const char *TAG = "recovery_http";

#define KILN_NVS_PARTITION "kiln_nvs"
#define BOOT_GUARD_NAMESPACE "kiln_cfg"
#define BOOT_GUARD_KEY "bootguard"
#define BOOT_GUARD_KEY_LEGACY "count"
#define BOOT_GUARD_NAMESPACE_LEGACY "boot_guard"

static ota_auth_nonce_state_t s_nonce;
// Per-route lockouts, mirroring ota_http.c:408-414's s_lockout_esp /
// s_lockout_sw_reset / s_lockout_boot_guard_reset -- a shared lockout would
// let repeated failures against one route (e.g. boot_guard_reset) lock out
// an unrelated route (e.g. ota_esp_post) that never saw a bad MAC itself.
static ota_auth_lockout_state_t s_lockout_esp;
static ota_auth_lockout_state_t s_lockout_boot_guard_reset;
static ota_auth_lockout_state_t s_lockout_sw_reset;

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
// ("esp" / "boot-guard-reset" / "sw-reset" -- must match the caller's route
// exactly, see the file header comment) and `lockout` is that route's own
// ota_auth_lockout_state_t. Each caller passes its own static lockout
// instance (s_lockout_esp / s_lockout_boot_guard_reset / s_lockout_sw_reset)
// so repeated failures against one route cannot lock out an unrelated one,
// mirroring ota_http.c:408-414's per-context lockouts.
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
        httpd_resp_set_status(req, "500 Internal Server Error");
        *out_err = httpd_resp_send(req, "no AP password on record", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    uint8_t key[32];
    hmac_sha256((const uint8_t *)ap_password, strlen(ap_password),
                (const uint8_t *)"kilnctl-ota-v1", strlen("kilnctl-ota-v1"), key);

    size_t context_len = strlen(context);
    uint8_t msg[OTA_AUTH_NONCE_LEN + 16];
    memcpy(msg, s_nonce.nonce, OTA_AUTH_NONCE_LEN);
    memcpy(msg + OTA_AUTH_NONCE_LEN, context, context_len);
    uint8_t expected_mac[32];
    hmac_sha256(key, sizeof(key), msg, OTA_AUTH_NONCE_LEN + context_len, expected_mac);

    // Invalidate the nonce unconditionally before deciding pass/fail --
    // UPDATE_PROTOCOL.md section 2 step 4.
    ota_auth_nonce_invalidate(&s_nonce);

    if (!ota_auth_constant_time_equal(claimed_mac, expected_mac, sizeof(expected_mac))) {
        ota_auth_lockout_record_failure(lockout, t);
        httpd_resp_set_status(req, "403 Forbidden");
        *out_err = httpd_resp_send(req, "bad MAC", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    ota_auth_lockout_record_success(lockout);
    return true;
}

// POST /api/ota/esp
static esp_err_t ota_esp_post(httpd_req_t *req)
{
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "esp", &s_lockout_esp)) {
        return auth_err;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "no OTA target partition in the flashed table",
                                HTTPD_RESP_USE_STRLEN);
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "esp_ota_begin failed", HTTPD_RESP_USE_STRLEN);
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(handle);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req, "read error mid-image", HTTPD_RESP_USE_STRLEN);
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "esp_ota_write failed", HTTPD_RESP_USE_STRLEN);
        }
        remaining -= n;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "esp_ota_end failed (image invalid?)", HTTPD_RESP_USE_STRLEN);
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "esp_ota_set_boot_partition failed", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "application image accepted and written -- rebooting into it");
    httpd_resp_sendstr(req, "ok, rebooting into new application image");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
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

// GET /api/boot_guard -- see this file's header comment: existence + length
// only, deliberately not a decode of boot_guard.c's private struct.
static esp_err_t boot_guard_get(httpd_req_t *req)
{
    nvs_handle_t h;
    bool present = false;
    size_t len = 0;
    if (nvs_open_from_partition(KILN_NVS_PARTITION, BOOT_GUARD_NAMESPACE, NVS_READONLY, &h) ==
        ESP_OK) {
        if (nvs_get_blob(h, BOOT_GUARD_KEY, NULL, &len) == ESP_OK) {
            present = true;
        }
        nvs_close(h);
    }
    char body[128];
    int n = snprintf(body, sizeof(body), "{\"record_present\":%s,\"record_len\":%u}",
                      present ? "true" : "false", (unsigned)len);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

// POST /api/ota/esp/boot_guard_reset
static esp_err_t boot_guard_reset_post(httpd_req_t *req)
{
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "boot-guard-reset", &s_lockout_boot_guard_reset)) {
        return auth_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, BOOT_GUARD_NAMESPACE,
                                             NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, BOOT_GUARD_KEY); // ESP_ERR_NVS_NOT_FOUND is fine -- already clear
        nvs_commit(h);
        nvs_close(h);
    }
    // Legacy namespace/key, same reasoning as boot_guard.c's own dual-read.
    if (nvs_open_from_partition(KILN_NVS_PARTITION, BOOT_GUARD_NAMESPACE_LEGACY, NVS_READWRITE,
                                 &h) == ESP_OK) {
        nvs_erase_key(h, BOOT_GUARD_KEY_LEGACY);
        nvs_commit(h);
        nvs_close(h);
    }
    return httpd_resp_sendstr(req, "boot_guard counter cleared");
}

// POST /api/sw_reset
static esp_err_t sw_reset_post(httpd_req_t *req)
{
    esp_err_t auth_err = ESP_OK;
    if (!recovery_authenticate_request(req, &auth_err, "sw-reset", &s_lockout_sw_reset)) {
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
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/api/ota/challenge", .method = HTTP_GET, .handler = challenge_get},
        {.uri = "/api/ota/esp", .method = HTTP_POST, .handler = ota_esp_post},
        {.uri = "/api/partitions", .method = HTTP_GET, .handler = partitions_get},
        {.uri = "/api/boot_guard", .method = HTTP_GET, .handler = boot_guard_get},
        {.uri = "/api/ota/esp/boot_guard_reset", .method = HTTP_POST,
         .handler = boot_guard_reset_post},
        {.uri = "/api/sw_reset", .method = HTTP_POST, .handler = sw_reset_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "recovery httpd up, %u routes", (unsigned)(sizeof(routes) / sizeof(routes[0])));
}
