#include "ota_http.h"

#include <string.h>

#include "psa/crypto.h"

#include "esp_log.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "ota_auth.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"

static const char *TAG = "ota_http";

// mbedtls/md.h's classic mbedtls_md_hmac*() family is entirely gated behind
// MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS in this vendored mbedtls 4.x/TF-PSA-
// Crypto build (confirmed by reading md.h directly: mbedtls_md_hmac_starts()
// through mbedtls_md_hmac() all sit inside that one #if block, lines
// 331-532) -- upstream's own migration docs say applications should not
// define that macro and should move to the PSA Crypto API instead, so this
// uses psa_import_key()/psa_mac_compute() rather than working around the
// guard. Two calls worth of HMAC (the KDF, then the real MAC) share this
// one helper rather than duplicating the import/compute/destroy sequence
// twice. Returns 0 on success, a negative psa_status_t on failure (PSA
// statuses are already negative, so this passes them through directly
// rather than remapping).
static int hmac_sha256(const uint8_t *key_bytes, size_t key_len, const uint8_t *msg,
                        size_t msg_len, uint8_t out[32])
{
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key_bytes, key_len, &key_id);
    if (status != PSA_SUCCESS) {
        return (int)status;
    }

    size_t mac_len = 0;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, 32,
                              &mac_len);

    psa_destroy_key(key_id);

    return (status == PSA_SUCCESS) ? 0 : (int)status;
}

// CommonFW/docs/UPDATE_PROTOCOL.md section 2 step 2's literal fixed context
// string, keyed off the AP password to derive the HMAC key -- never the
// literal password itself, so a bug that leaks the compared bytes leaks a
// derived key rather than the Wi-Fi password.
#define OTA_HTTP_KDF_CONTEXT "kilnctl-ota-v1"

// One shared nonce (see ota_http.h's header comment: the challenge is
// issued once, then either the /esp or /pico context is authenticated
// against it), two independent per-endpoint lockout states ("counted
// per-endpoint" -- UPDATE_PROTOCOL.md section 2). Guarded by s_ota_lock:
// esp_http_server can process more than one connection concurrently
// depending on its configuration, and this state is genuinely mutated on
// every challenge/verify call, unlike dashboard_http.c's s_dash (set once
// at startup, read-only thereafter).
static SemaphoreHandle_t s_ota_lock;
static ota_auth_nonce_state_t s_nonce;
static ota_auth_lockout_state_t s_lockout_esp;
static ota_auth_lockout_state_t s_lockout_pico;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void hex_encode(const uint8_t *in, size_t len, char *out /* 2*len + 1 bytes */)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[(in[i] >> 4) & 0xFu];
        out[2 * i + 1] = digits[in[i] & 0xFu];
    }
    out[2 * len] = '\0';
}

// Client IP for the "log every attempt with the source IP" requirement
// (UPDATE_PROTOCOL.md section 2). Standard ESP-IDF httpd pattern: the
// underlying socket is IPv4-mapped-into-IPv6 by lwip regardless of which
// family the client actually connected over, so this handles both without
// the caller needing to know which.
static void get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        snprintf(out, out_len, "unknown");
        return;
    }

    struct sockaddr_in6 addr;
    socklen_t addr_size = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) != 0) {
        snprintf(out, out_len, "unknown");
        return;
    }

    if (addr.sin6_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &addr4->sin_addr, out, out_len);
    } else {
        inet_ntop(AF_INET6, &addr.sin6_addr, out, out_len);
    }
}

static esp_err_t ota_challenge_get_handler(httpd_req_t *req)
{
    uint8_t rand_bytes[OTA_AUTH_NONCE_LEN];
    esp_fill_random(rand_bytes, sizeof(rand_bytes)); // hardware RNG, not a hand-rolled source

    char ip[46];
    get_client_ip(req, ip, sizeof(ip));

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "OTA challenge from %s: internal lock timeout, refused", ip);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    ota_auth_nonce_issue(&s_nonce, rand_bytes, now_ms());
    xSemaphoreGive(s_ota_lock);

    // {"nonce":"<32 hex chars>"} -- no other JSON producer in this codebase
    // returns a bare hex blob, so wrapping it in a one-field object keeps
    // this endpoint consistent with every other /api/* handler in
    // dashboard_http.c rather than being the one that returns a raw string.
    char hex[OTA_AUTH_NONCE_LEN * 2 + 1];
    hex_encode(rand_bytes, sizeof(rand_bytes), hex);

    char json[64];
    int n = snprintf(json, sizeof(json), "{\"nonce\":\"%s\"}", hex);

    ESP_LOGI(TAG, "OTA challenge issued to %s", ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}

ota_http_verify_result_t ota_http_verify_request(ota_http_context_t ctx, const uint8_t mac[32],
                                                   const char *client_ip)
{
    const char *ctx_str = (ctx == OTA_HTTP_CONTEXT_ESP) ? "esp" : "pico";
    const char *ip = client_ip ? client_ip : "unknown";
    ota_auth_lockout_state_t *lockout = (ctx == OTA_HTTP_CONTEXT_ESP) ? &s_lockout_esp : &s_lockout_pico;

    uint32_t t = now_ms();

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Cannot safely check or mutate shared auth state -- refuse rather
        // than risk a torn read/write. This is not a real lockout, but the
        // caller-facing effect (refused) is the same, and returning
        // LOCKED_OUT here means the caller never treats an internal
        // contention failure as "the MAC was fine, go ahead."
        ESP_LOGW(TAG, "OTA verify(%s) from %s: internal lock timeout, refused", ctx_str, ip);
        return OTA_HTTP_VERIFY_LOCKED_OUT;
    }

    if (ota_auth_lockout_is_locked(lockout, t)) {
        xSemaphoreGive(s_ota_lock);
        ESP_LOGW(TAG, "OTA verify(%s) from %s: refused, endpoint locked out", ctx_str, ip);
        return OTA_HTTP_VERIFY_LOCKED_OUT;
    }

    // A stale/reused/never-issued nonce is the client's timing, not a
    // wrong-password guess -- do not record a lockout failure for it (see
    // ota_http.h's doc comment on this function for the full reasoning).
    ota_auth_nonce_check_t nonce_check = ota_auth_nonce_check(&s_nonce, t);
    if (nonce_check != OTA_AUTH_NONCE_OK) {
        xSemaphoreGive(s_ota_lock);
        ESP_LOGW(TAG, "OTA verify(%s) from %s: no valid nonce (code %d)", ctx_str, ip,
                 (int)nonce_check);
        return OTA_HTTP_VERIFY_NO_VALID_NONCE;
    }

    uint8_t nonce_copy[OTA_AUTH_NONCE_LEN];
    memcpy(nonce_copy, s_nonce.nonce, sizeof(nonce_copy));
    xSemaphoreGive(s_ota_lock);

    // key = HMAC-SHA256(ap_password, "kilnctl-ota-v1") -- UPDATE_PROTOCOL.md
    // section 2 step 2. ap_password is the HMAC KEY here, the context
    // string is the message -- this derivation is what keeps the literal
    // Wi-Fi password out of the comparison path entirely.
    const char *ap_password = wifi_prov_get_ap_password();
    size_t pw_len = ap_password ? strlen(ap_password) : 0;

    uint8_t key[32];
    int hmac_rc = hmac_sha256((const uint8_t *)ap_password, pw_len,
                               (const uint8_t *)OTA_HTTP_KDF_CONTEXT, strlen(OTA_HTTP_KDF_CONTEXT),
                               key);

    // expected_mac = HMAC-SHA256(key, nonce || context)
    uint8_t msg[OTA_AUTH_NONCE_LEN + 4]; // "esp" (3) or "pico" (4) -- 4 covers both
    size_t ctx_len = strlen(ctx_str);
    memcpy(msg, nonce_copy, sizeof(nonce_copy));
    memcpy(msg + sizeof(nonce_copy), ctx_str, ctx_len);

    uint8_t expected[32];
    if (hmac_rc == 0) {
        hmac_rc = hmac_sha256(key, sizeof(key), msg, sizeof(nonce_copy) + ctx_len, expected);
    }
    if (hmac_rc != 0) {
        memset(key, 0, sizeof(key));
        ESP_LOGE(TAG, "OTA verify(%s) from %s: mbedtls HMAC failed (%d)", ctx_str, ip, hmac_rc);
        // Still invalidate the nonce below -- a crypto-library failure must
        // not leave a usable challenge sitting around for a retry to (maybe)
        // succeed against.
        if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            ota_auth_nonce_invalidate(&s_nonce);
            xSemaphoreGive(s_ota_lock);
        }
        return OTA_HTTP_VERIFY_BAD_MAC;
    }

    bool match = ota_auth_constant_time_equal(expected, mac, sizeof(expected));

    // Key material has done its job -- do not leave it sitting in a stack
    // frame any longer than necessary.
    memset(key, 0, sizeof(key));

    // Always invalidate -- "the nonce whether or not it matched"
    // (UPDATE_PROTOCOL.md section 2 step 4) -- and update the lockout
    // counter for a REAL auth attempt (a valid, unexpired, unused nonce was
    // checked against an actual MAC), success or failure.
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        ota_auth_nonce_invalidate(&s_nonce);
        if (match) {
            ota_auth_lockout_record_success(lockout);
        } else {
            ota_auth_lockout_record_failure(lockout, t);
        }
        xSemaphoreGive(s_ota_lock);
    } else {
        // Could not take the lock to invalidate/record -- log loudly rather
        // than silently leave the nonce reusable. This is a defensive edge
        // case (the same short timeout already succeeded twice above in
        // this same call), not an expected path.
        ESP_LOGE(TAG, "OTA verify(%s) from %s: lock timeout during invalidate/record -- nonce "
                      "may remain reusable until it naturally expires",
                 ctx_str, ip);
    }

    ESP_LOGI(TAG, "OTA verify(%s) from %s: %s", ctx_str, ip, match ? "OK" : "MAC mismatch");
    return match ? OTA_HTTP_VERIFY_OK : OTA_HTTP_VERIFY_BAD_MAC;
}

esp_err_t ota_http_start(void)
{
    // Required once before any psa_*() call (hmac_sha256() above) --
    // idempotent per the PSA Crypto API spec, but this is the one place in
    // this codebase that needs it, so it is called here rather than
    // scattered near every HMAC call site.
    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)psa_status);
        return ESP_FAIL;
    }

    s_ota_lock = xSemaphoreCreateMutex();
    if (!s_ota_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_nonce, 0, sizeof(s_nonce));
    memset(&s_lockout_esp, 0, sizeof(s_lockout_esp));
    memset(&s_lockout_pico, 0, sizeof(s_lockout_pico));

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t challenge_uri = {
        .uri = "/api/ota/challenge", .method = HTTP_GET, .handler = ota_challenge_get_handler
    };
    esp_err_t err = httpd_register_uri_handler(server, &challenge_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/challenge) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
