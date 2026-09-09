#include "ota_http.h"
#include "ota_http_internal.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <string.h>

#include "psa/crypto.h"

#include "build_info.h" /* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME -- TODO.md 9.6's
                          * per-processor build-identity fields for the ESP side, same header
                          * safety_link.c already includes for the ANNOUNCE_VERSION payload */
#include "esp_app_desc.h"
#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC -- section 3's pre-esp_ota_begin() check */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "hal_sysinfo.h"
#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- section 4's Pico-image running CRC32, see ota_pico_do_stage() */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "boot_button.h"
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

#include "ota_http.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <string.h>

#include "psa/crypto.h"

#include "build_info.h" /* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME -- TODO.md 9.6's
                          * per-processor build-identity fields for the ESP side, same header
                          * safety_link.c already includes for the ANNOUNCE_VERSION payload */
#include "esp_app_desc.h"
#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC -- section 3's pre-esp_ota_begin() check */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- section 4's Pico-image running CRC32, see ota_pico_do_stage() */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "boot_button.h"
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

const char *OTA_HTTP_TAG = "ota_http";

// GET /ota page (TODO.md 9.6) -- gzipped at configure time by
// App/drivers/CMakeLists.txt's KILNCTL_GZIP_ASSETS list, same
// EMBED_TXTFILES + Content-Encoding: gzip convention every other page in
// this component uses (rules_page.html/profiles_page.html/etc. -- see
// rules_http.c's page_get_handler()/client_accepts_gzip() for the precedent
// this mirrors).
extern const uint8_t ota_page_html_gz_start[] asm("_binary_ota_page_html_gz_start");
extern const uint8_t ota_page_html_gz_end[] asm("_binary_ota_page_html_gz_end");

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
// Its own lockout state, not a reuse of s_lockout_esp -- see ota_http.h's
// doc comment on OTA_HTTP_CONTEXT_ESP_ROLLBACK for why the rollback context
// is kept entirely separate from the plain-esp-update context, including
// its own 3-strikes counter.
static ota_auth_lockout_state_t s_lockout_esp_rollback;
// Same reasoning again for the recovery-exit route -- see ota_http.h's doc
// comment on OTA_HTTP_CONTEXT_RECOVERY_EXIT.
static ota_auth_lockout_state_t s_lockout_recovery_exit;
// Same reasoning again for POST /api/factory_reset -- see ota_http.h's doc
// comment on OTA_HTTP_CONTEXT_FACTORY_RESET.
static ota_auth_lockout_state_t s_lockout_factory_reset;
// Same reasoning again for POST /api/ota/pico/rollback -- see ota_http.h's
// doc comment on OTA_HTTP_CONTEXT_PICO_ROLLBACK.
static ota_auth_lockout_state_t s_lockout_pico_rollback;
// Same reasoning again for POST /api/sw_reset -- see ota_state.h's doc
// comment on OTA_HTTP_CONTEXT_SW_RESET.
static ota_auth_lockout_state_t s_lockout_sw_reset;
// Same reasoning again for POST /api/ota/esp/boot_guard_reset -- see
// ota_state.h's doc comment on OTA_HTTP_CONTEXT_BOOT_GUARD_RESET.
static ota_auth_lockout_state_t s_lockout_boot_guard_reset;

// opus-review finding 3: safety_link_send_rollback_ex() blocks its caller
// for up to ~6.3s (4 sends * 250ms + one reply window + the 5s boot_id
// watch). esp_http_server here runs with exactly ONE worker task
// (wifi_provision_http.c's own config), so a handler that blocks inside it
// for that long queues up EVERY other request behind it -- including the
// dashboard's ~1Hz /api/status poll and this very OTA page's own UI, both
// of which have client-side timeouts well under 6.3s (wifi_provision_http.c
// lines 695-735 document a previously live-tested wedge in exactly this
// area). Rather than shorten the watch (which trades a wedged HTTP worker
// for reporting UNKNOWN_TIMEOUT on a rollback that was still genuinely in
// flight -- see safety_link.h's SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS comment
// for why 5s is already a rough estimate, not a measured floor), the whole
// safety_link_send_rollback_ex() call now runs on its own short-lived task,
// same "task owns the mutex release, handler returns promptly" shape
// ota_pico_do_stage()/the Pico relay task already use for the plain Pico
// update path just above. ota_pico_rollback_post_handler() below starts the
// task and returns a 202 immediately; the OTA page polls GET /api/ota/pico/
// rollback/status (ota_pico_rollback_status_get_handler()) for the outcome,
// the same poll-for-progress shape /api/ota/pico/status already establishes
// for the plain update.
//
// ota_pico_rollback_async_state_t/ota_pico_rollback_async_t moved to
// ota_http_internal.h -- ota_http_pico.c (the reader/writer of the state
// below) needs the type too.

// Guarded by its own small mutex, deliberately separate from s_ota_lock
// (nonce/lockout bookkeeping) and from the update-claim mechanism
// (ota_http_update_try_begin()/_end(), a different file's own mutex) --
// this state is written by a background task while the handler that started
// it may already have returned and moved on to a different request, so it
// cannot share either of those locks' lifetimes.
SemaphoreHandle_t ota_http_pico_rollback_async_lock;
ota_pico_rollback_async_t ota_http_pico_rollback_async;

// The hardware pointers main.c hands to ota_http_start(), same pattern (and
// same NULL-tolerant meaning) as dashboard_http.c's s_dash struct. Read-only
// after ota_http_start(), so no lock needed to read them.
static kiln_io_t *s_io;
static MAX31856BusClass *s_thermo_bus;
SafetyLinkClass *ota_http_safety;

// --- Single cross-processor update mutex (ota_http.h) ---------------------
// Guarded by s_ota_lock, same as the nonce/lockout state above -- this is
// genuinely mutated from whatever future task handles a POST
// /api/ota/{esp,pico} upload, so it needs the same discipline as everything
// else in this file that more than one httpd worker could touch at once.
typedef enum {
    OTA_UPDATE_NONE = 0,
    OTA_UPDATE_ESP,
    OTA_UPDATE_PICO,
} ota_update_claim_t;

static ota_update_claim_t s_update_claim = OTA_UPDATE_NONE;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Client IP for the "log every attempt with the source IP" requirement
// (UPDATE_PROTOCOL.md section 2). Standard ESP-IDF httpd pattern: the
// underlying socket is IPv4-mapped-into-IPv6 by lwip regardless of which
// family the client actually connected over, so this handles both without
// the caller needing to know which.
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
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

// TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
// web_client_accepts_gzip() (the per-file duplicated helpers were folded
// into it) -- absent Accept-Encoding is legal and served gzip per RFC 9110
// s12.5.3; a header that explicitly excludes gzip gets an uncompressed 406.
static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, OTA_HTTP_TAG, "ota_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)ota_page_html_gz_start,
                            (size_t)(ota_page_html_gz_end - ota_page_html_gz_start));
}

/* snprintf() returns the length it WOULD have written, not what it did. Passing
 * that straight to httpd_resp_send() as the body length means that the moment a
 * response truncates, the server reads past the end of the local buffer and
 * sends whatever is next on the stack to the client. Every JSON responder in
 * this file used to do exactly that (TODO.md: "fifteen sites use snprintf's
 * return unclamped at the high end"), and several of them interpolate strings
 * that are not this module's own -- a Pico relay's last_error, an image's
 * version -- so the length is not always something a reader can bound by
 * inspection.
 *
 * Truncating is the right answer rather than 500ing: these are status readouts,
 * a short one is still useful, and the caller has already decided the operation
 * succeeded. The log line is there so an undersized buffer surfaces rather than
 * silently shipping half a document forever. */
esp_err_t ota_http_send_json_clamped(httpd_req_t *req, const char *buf, int n, size_t cap)
{
    if (n < 0) {
        ESP_LOGE(OTA_HTTP_TAG, "response formatting failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response formatting failed");
        return ESP_OK;
    }
    size_t len = (size_t)n;
    if (len >= cap) {
        ESP_LOGE(OTA_HTTP_TAG, "response did not fit in %u bytes -- truncated, raise the buffer", (unsigned)cap);
        len = cap - 1;
    }
    return httpd_resp_send(req, buf, len);
}

static esp_err_t ota_challenge_get_handler(httpd_req_t *req)
{
    uint8_t rand_bytes[OTA_AUTH_NONCE_LEN];
    hal_sysinfo_fill_random(rand_bytes, sizeof(rand_bytes)); // hardware RNG, not a hand-rolled source

    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA challenge from %s: internal lock timeout, refused", ip);
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

    ESP_LOGI(OTA_HTTP_TAG, "OTA challenge issued to %s", ip);

    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, json, n, sizeof(json));
    return ESP_OK;
}

ota_http_verify_result_t ota_http_verify_request(ota_http_context_t ctx, const uint8_t mac[32],
                                                   const char *client_ip)
{
    const char *ctx_str;
    ota_auth_lockout_state_t *lockout;
    switch (ctx) {
        case OTA_HTTP_CONTEXT_ESP:            ctx_str = "esp";          lockout = &s_lockout_esp;          break;
        case OTA_HTTP_CONTEXT_ESP_ROLLBACK:   ctx_str = "esp-rollback"; lockout = &s_lockout_esp_rollback; break;
        case OTA_HTTP_CONTEXT_RECOVERY_EXIT:  ctx_str = "recovery";     lockout = &s_lockout_recovery_exit; break;
        case OTA_HTTP_CONTEXT_FACTORY_RESET:  ctx_str = "factory-reset"; lockout = &s_lockout_factory_reset; break;
        case OTA_HTTP_CONTEXT_PICO_ROLLBACK:  ctx_str = "pico-rollback"; lockout = &s_lockout_pico_rollback; break;
        case OTA_HTTP_CONTEXT_SW_RESET:       ctx_str = "sw-reset";     lockout = &s_lockout_sw_reset;      break;
        case OTA_HTTP_CONTEXT_BOOT_GUARD_RESET: ctx_str = "boot-guard-reset"; lockout = &s_lockout_boot_guard_reset; break;
        case OTA_HTTP_CONTEXT_PICO:
        default:                              ctx_str = "pico";         lockout = &s_lockout_pico;         break;
    }
    const char *ip = client_ip ? client_ip : "unknown";

    // boot_button.h's recovery hatch: a BOOT-button long-press window,
    // checked BEFORE the lockout/nonce work below, for ALL FOUR contexts
    // ("esp", "pico", "esp-rollback", "recovery") -- see boot_button.h's
    // "HOW THIS FITS TOGETHER WITH ota_http.c" comment for why scoping this
    // to only one context would defeat the point (an operator who has lost
    // the AP password has lost it for every context equally). Unmissable on
    // purpose: this is a physical-presence override of a password check, and
    // every attempt made under it must be loud in the log, naming both the
    // context and the source IP, same as every other auth decision in this
    // function.
    if (boot_button_ota_bypass_active()) {
        ESP_LOGE(OTA_HTTP_TAG, "OTA verify(%s) from %s: AUTHENTICATION BYPASSED by the BOOT-button recovery "
                      "window -- a long-press on GPIO0 opened this, %lu ms remain",
                 ctx_str, ip, (unsigned long)boot_button_bypass_remaining_ms());
        return OTA_HTTP_VERIFY_OK;
    }

    uint32_t t = now_ms();

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Cannot safely check or mutate shared auth state -- refuse rather
        // than risk a torn read/write. This is not a real lockout, but the
        // caller-facing effect (refused) is the same, and returning
        // LOCKED_OUT here means the caller never treats an internal
        // contention failure as "the MAC was fine, go ahead."
        ESP_LOGW(OTA_HTTP_TAG, "OTA verify(%s) from %s: internal lock timeout, refused", ctx_str, ip);
        return OTA_HTTP_VERIFY_LOCKED_OUT;
    }

    if (ota_auth_lockout_is_locked(lockout, t)) {
        xSemaphoreGive(s_ota_lock);
        ESP_LOGW(OTA_HTTP_TAG, "OTA verify(%s) from %s: refused, endpoint locked out", ctx_str, ip);
        return OTA_HTTP_VERIFY_LOCKED_OUT;
    }

    // A stale/reused/never-issued nonce is the client's timing, not a
    // wrong-password guess -- do not record a lockout failure for it (see
    // ota_http.h's doc comment on this function for the full reasoning).
    ota_auth_nonce_check_t nonce_check = ota_auth_nonce_check(&s_nonce, t);
    if (nonce_check != OTA_AUTH_NONCE_OK) {
        xSemaphoreGive(s_ota_lock);
        ESP_LOGW(OTA_HTTP_TAG, "OTA verify(%s) from %s: no valid nonce (code %d)", ctx_str, ip,
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

    // An open AP (empty password) means the HMAC key derived below is a
    // zero-length key -- well-defined, but PUBLIC: anyone who can reach this
    // endpoint already knows the AP password is empty, so a MAC computed
    // against it proves nothing. Refuse outright rather than let OTA auth
    // quietly collapse to "any request with a well-formed header succeeds."
    // Checked AFTER the BOOT-button bypass above (that physical-presence
    // override must keep working regardless of AP password state -- it is
    // the recovery path this refusal leans on) and BEFORE any nonce/HMAC math
    // runs, since there is no point computing a MAC against a key everyone
    // already has. Still invalidates the nonce (a stale challenge left lying
    // around after a refused attempt is no better here than after any other
    // refusal) and does NOT record a lockout failure -- like a stale nonce,
    // this is the board's own configuration, not a wrong-password guess.
    if (pw_len == 0) {
        ESP_LOGE(OTA_HTTP_TAG, "OTA verify(%s) from %s: REFUSED -- this board's AP password is empty (open "
                      "AP), so OTA auth would reduce to nothing; set an AP password to update this "
                      "board over HTTP, or use the physical BOOT-button recovery window (hold BOOT "
                      "during boot) which does not depend on the AP password",
                 ctx_str, ip);
        if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            ota_auth_nonce_invalidate(&s_nonce);
            xSemaphoreGive(s_ota_lock);
        }
        return OTA_HTTP_VERIFY_NO_AP_PASSWORD;
    }

    uint8_t key[32];
    int hmac_rc = hmac_sha256((const uint8_t *)ap_password, pw_len,
                               (const uint8_t *)OTA_HTTP_KDF_CONTEXT, strlen(OTA_HTTP_KDF_CONTEXT),
                               key);

    // expected_mac = HMAC-SHA256(key, nonce || context)
    uint8_t msg[OTA_AUTH_NONCE_LEN + 13]; // "esp" (3), "pico" (4), "esp-rollback" (12), "recovery" (8),
                                           // "factory-reset" (13), or "pico-rollback" (13) -- 13 covers
                                           // all six context strings currently in use; if a future
                                           // context string exceeds 13 chars, widen this buffer AND
                                           // update this comment
    size_t ctx_len = strlen(ctx_str);
    memcpy(msg, nonce_copy, sizeof(nonce_copy));
    memcpy(msg + sizeof(nonce_copy), ctx_str, ctx_len);

    uint8_t expected[32];
    if (hmac_rc == 0) {
        hmac_rc = hmac_sha256(key, sizeof(key), msg, sizeof(nonce_copy) + ctx_len, expected);
    }
    if (hmac_rc != 0) {
        memset(key, 0, sizeof(key));
        ESP_LOGE(OTA_HTTP_TAG, "OTA verify(%s) from %s: mbedtls HMAC failed (%d)", ctx_str, ip, hmac_rc);
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
        ESP_LOGE(OTA_HTTP_TAG, "OTA verify(%s) from %s: lock timeout during invalidate/record -- nonce "
                      "may remain reusable until it naturally expires",
                 ctx_str, ip);
    }

    ESP_LOGI(OTA_HTTP_TAG, "OTA verify(%s) from %s: %s", ctx_str, ip, match ? "OK" : "MAC mismatch");
    return match ? OTA_HTTP_VERIFY_OK : OTA_HTTP_VERIFY_BAD_MAC;
}

bool ota_http_update_try_begin(ota_http_context_t ctx)
{
    ota_update_claim_t want = (ctx == OTA_HTTP_CONTEXT_ESP) ? OTA_UPDATE_ESP : OTA_UPDATE_PICO;

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Cannot safely check/mutate the claim -- refuse rather than risk
        // two callers both believing they won it, same reasoning as
        // ota_http_verify_request()'s lock-timeout path above.
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim(%s): internal lock timeout, refused",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
        return false;
    }

    bool won = (s_update_claim == OTA_UPDATE_NONE);
    if (won) {
        s_update_claim = want;
    }
    xSemaphoreGive(s_ota_lock);

    if (won) {
        ESP_LOGI(OTA_HTTP_TAG, "OTA update claim(%s): acquired", ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    } else {
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim(%s): refused, an update is already in progress",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    }
    return won;
}

void ota_http_update_end(void)
{
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_update_claim != OTA_UPDATE_NONE) {
            ESP_LOGI(OTA_HTTP_TAG, "OTA update claim released");
        }
        s_update_claim = OTA_UPDATE_NONE;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Defensive: could not take the lock to release. Logged loudly
        // because an unreleased claim permanently refuses every future
        // update until reboot -- this must never happen silently.
        ESP_LOGE(OTA_HTTP_TAG, "OTA update claim release: lock timeout -- claim may remain held");
    }
}

bool ota_http_update_in_progress(ota_http_context_t *out_ctx)
{
    bool in_progress = false;
    ota_update_claim_t claim = OTA_UPDATE_NONE;

    // s_ota_lock does not exist until ota_http_start() runs. Any caller that
    // asks before then must get a fail-safe answer rather than a crash:
    // xSemaphoreTake() on a NULL handle asserts inside FreeRTOS and panics
    // the whole system. This is not hypothetical -- rules_task called this
    // from its 1 Hz fail-safe gate while ota_http_start() was still ~120
    // lines away in app_main(), and the board boot-looped with
    // "assert_func ... xQueueSemaphoreTake" on every single boot.
    //
    // "In progress" is the safe answer here, not "idle": every caller uses
    // this to decide whether it is safe to start heating or start another
    // update, and before the OTA subsystem is even up, refusing both is
    // correct. The start-order fix in main.c is the real remedy; this guard
    // exists so a future caller that runs early degrades to "refuse" instead
    // of taking the board down.
    if (s_ota_lock == NULL) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim query before ota_http_start() -- reporting in-progress (fail-safe)");
        return true;
    }

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        claim = s_update_claim;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Cannot confirm the claim is free -- treat contention as "in
        // progress" rather than risk telling a caller it's safe to start a
        // second update when we simply couldn't check.
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim query: internal lock timeout, reporting in-progress");
        return true;
    }

    in_progress = (claim != OTA_UPDATE_NONE);
    if (in_progress && out_ctx) {
        *out_ctx = (claim == OTA_UPDATE_ESP) ? OTA_HTTP_CONTEXT_ESP : OTA_HTTP_CONTEXT_PICO;
    }
    return in_progress;
}

/* Per-request acknowledgement token for the one overridable interlock
 * precondition (ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK). A
 * header rather than a query parameter so it survives the binary-body
 * upload routes unchanged, and so it never lands in a server access log or
 * a browser history entry the way a URL would.
 *
 * NOT covered by the request HMAC (ota_http_verify_request()). That is a
 * deliberate, bounded choice: every route that reads this already refuses
 * unauthenticated callers outright, so nobody who cannot produce a valid
 * signature can set this header on a request that gets this far. What the
 * header relaxes is a local operator-policy check ("is a supervisor
 * watching?"), never an authentication or authorisation decision, and it
 * cannot turn a hard refusal (a running profile, a hot zone) into a pass.
 */
#define OTA_ACK_NO_SAFETY_HEADER "X-Ota-Ack-No-Safety"

bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    char val[8];
    if (httpd_req_get_hdr_value_str(req, OTA_ACK_NO_SAFETY_HEADER, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    return val[0] == '1';
}

/* Emits the right refusal for an interlock result, keeping the status code
 * meaningful to the page: 428 Precondition Required means "there is a
 * precondition you can satisfy by acknowledging it, ask the operator and
 * retry with the header"; 409 Conflict keeps its old meaning of "the kiln
 * is busy or hot, there is nothing to acknowledge." A client that does not
 * know about 428 still sees a 4xx and still refuses, which is the safe
 * default. */
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r,
                                          const char *reason)
{
    if (r == OTA_INTERLOCK_REFUSED_NEEDS_ACK) {
        httpd_resp_set_status(req, "428 Precondition Required");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
}

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    /* B2 (opus review, 2026-08-27): a zone current sweep (zones_http.c) is
     * a relay writer too, and mid-update is one of the worse times for a
     * relay to be raced -- checked here, before the snapshot below, same
     * "cheapest and orthogonal to kiln state" reasoning
     * ota_interlock_check() itself documents for its own mutex check. Kept
     * as a standalone early return rather than a new ota_interlock_snapshot_t
     * field so ota_interlock.c -- the pure, host-tested half of this check,
     * with its own precondition-ordering doc comment and test coverage --
     * stays untouched; zones_http.h/.c are the only files this pass is
     * authorized to change. */
    if (zones_current_sweep_is_active()) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "a zone current sweep is running");
        }
        return OTA_INTERLOCK_REFUSED;
    }

    ota_interlock_snapshot_t snap = { 0 };
    snap.operator_ack_no_safety_processor = ack_no_safety_processor;

    // Profile executor state -- one-for-one map onto ota_interlock.h's own
    // enum (see that header's comment on why it can't just reuse
    // profile_exec_state_t directly: this file CAN include profile_executor.h,
    // but ota_interlock.c must stay host-buildable and cannot). Only the
    // .state field is used here -- see the per-zone loop below for why
    // pstat.zones[] itself is the WRONG source for temperature/heater-
    // commanded data.
    profile_exec_status_t pstat;
    profile_executor_get_status(&pstat);
    switch (pstat.state) {
        case PROFILE_EXEC_RUNNING: snap.profile_state = OTA_INTERLOCK_PROFILE_RUNNING; break;
        case PROFILE_EXEC_PAUSED:  snap.profile_state = OTA_INTERLOCK_PROFILE_PAUSED; break;
        case PROFILE_EXEC_DONE:    snap.profile_state = OTA_INTERLOCK_PROFILE_DONE; break;
        case PROFILE_EXEC_FAULTED: snap.profile_state = OTA_INTERLOCK_PROFILE_FAULTED; break;
        case PROFILE_EXEC_IDLE:
        default:                   snap.profile_state = OTA_INTERLOCK_PROFILE_IDLE; break;
    }

    snap.autotune_active = autotune_engine_is_active();

    // Safety link: NULL (no link this boot) is treated as down, same
    // fail-safe default as every other consumer of this pointer.
    if (ota_http_safety) {
        safety_link_status_t link_status;
        snap.safety_link_up = (safety_link_get_status(ota_http_safety, &link_status) == ESP_OK)
                                   ? link_status.link_up
                                   : false;
    } else {
        snap.safety_link_up = false;
    }

    // run_state's boot-record breadcrumb -- catches a firing that survived a
    // reboot without a clean ending, which profile_state alone (always IDLE
    // on a fresh boot) cannot see. Purely informational per run_state.h's
    // own contract, which is exactly the read-only use this is.
    snap.run_state_interrupted = run_state_boot_record_interrupted();

    snap.other_update_in_progress = ota_http_update_in_progress(NULL);

    snap.temp_ceiling_c = OTA_INTERLOCK_TEMP_CEILING_C;

    // Per-zone temperature and relay-commanded state, read the SAME way
    // dashboard_http.c's /api/status does -- directly from the thermo bus
    // and kiln_io, NOT from profile_executor_get_status()'s zones[] array.
    // profile_exec_zone_status_t.active only means "this zone participated
    // in the last/current RUN" (see profile_executor.h's own doc comment);
    // when the executor is IDLE (the exact case this interlock exists to
    // catch -- "a cooling kiln is still a hot kiln" even with nothing
    // running), every zones[i].active there is false and this loop would
    // silently check NOTHING. Reading zones_config_get_thermo_count()/
    // MAX31856_read_all()/kiln_io_read() instead means the ceiling and
    // heater-commanded checks below see the kiln's actual current state
    // regardless of whether a profile happens to be running.
    ota_interlock_zone_snapshot_t zones[MAX31856_CHANNEL_COUNT];
    memset(zones, 0, sizeof(zones));

    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT; /* defensive; zones_http.c already validates this */
    }

    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t reading_count = 0;
    if (sim_backend_enabled()) {
        sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);
    } else if (s_thermo_bus && s_thermo_bus->initialized) {
        MAX31856_read_all(s_thermo_bus, readings, MAX31856_CHANNEL_COUNT, &reading_count);
    }

    kiln_io_state_t io_state;
    memset(&io_state, 0, sizeof(io_state));
    bool io_read_ok = false;
    if (s_io) {
        io_read_ok = (kiln_io_read(s_io, &io_state) == ESP_OK);
    }

    for (uint8_t i = 0; i < thermo_count; i++) {
        zones[i].active = true;

        // Find channel i's reading, if it answered this poll -- readings[]
        // is not guaranteed to be dense/in-order once a channel is absent,
        // same reasoning as dashboard_http.c's own loop.
        const MAX31856Reading *r = NULL;
        for (size_t j = 0; j < reading_count; j++) {
            if (readings[j].channel == i) {
                r = &readings[j];
                break;
            }
        }
        if (r && !r->spi_failed && !isnan(r->tc_temperature_c)) {
            zones[i].actual_valid = true;
            zones[i].actual_c = zones_config_apply_cal(i, r->tc_temperature_c);
        } else {
            zones[i].actual_valid = false;
        }

        // heater_commanded: any relay this zone owns is currently on. NOT
        // io_read_ok (no io this boot / read failed) is treated as
        // "commanded" too -- an unreadable relay state cannot be assumed
        // off, same "no valid data -> refuse" rule the temperature check
        // above follows.
        if (!io_read_ok) {
            zones[i].heater_commanded = true;
            continue;
        }
        uint8_t relay_mask = 0;
        if (zones_config_get_relay_mask(i, &relay_mask)) {
            zones[i].heater_commanded = (io_state.relay_shadow & relay_mask) != 0;
        } else {
            zones[i].heater_commanded = false; /* zone has no relays assigned -- nothing to command */
        }
    }

    return ota_interlock_check(&snap, zones, thermo_count, reason_out, reason_cap);
}

// See ota_http.h's doc comment above this function. The mirror-image glue
// to ota_http_check_interlocks() above: same s_update_claim mutex, opposite
// direction ("may heat proceed" instead of "may an update start").
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    heat_interlock_snapshot_t snap = { 0 };
    ota_http_context_t ctx;

    snap.update_in_progress = ota_http_update_in_progress(&ctx);
    if (snap.update_in_progress) {
        // Explicit per-value mapping, not a two-way ternary -- a ternary
        // testing only "== OTA_HTTP_CONTEXT_PICO" happened to be correct
        // today only because ota_http_update_try_begin() is, in fact, never
        // called with anything but OTA_HTTP_CONTEXT_ESP or _PICO (every
        // other ota_http_context_t member -- ESP_ROLLBACK, RECOVERY_EXIT,
        // FACTORY_RESET, PICO_ROLLBACK -- is an auth context that claims the
        // mutex AS _ESP or _PICO, per each handler's own doc comment; see
        // ota_pico_rollback_post_handler() above claiming OTA_HTTP_CONTEXT_
        // PICO, not _PICO_ROLLBACK). That is an invariant of the CALL SITES,
        // not of this enum, so a future context that claims the mutex under
        // its own value would silently fall through to the ternary's "else"
        // (ESP) with no compiler warning. A switch makes every currently-
        // reachable value's mapping explicit and gives a single place to
        // extend if that invariant ever changes.
        switch (ctx) {
            case OTA_HTTP_CONTEXT_PICO:
                snap.update_context = HEAT_INTERLOCK_UPDATE_PICO;
                break;
            // opus-review finding 4 (latent nit): OTA_HTTP_CONTEXT_PICO_
            // ROLLBACK used to sit in the same case-group as the ESP-mapped
            // values below, correct only because ota_pico_rollback_post_
            // handler() claims the mutex AS OTA_HTTP_CONTEXT_PICO (see the
            // comment above this switch), never as _PICO_ROLLBACK -- so this
            // branch has never actually been reached for it. If a future
            // handler ever DID claim the mutex under its own _PICO_ROLLBACK
            // value, grouping it with ESP would misreport a Pico-side action
            // to the heat interlock as an ESP one. Given its own explicit
            // case rather than left to fall into the ESP group: it names the
            // processor it actually rolls back (the Pico), which is also the
            // semantically correct mapping even in the unreachable-today
            // case, not just the loudest one.
            case OTA_HTTP_CONTEXT_PICO_ROLLBACK:
                snap.update_context = HEAT_INTERLOCK_UPDATE_PICO;
                break;
            case OTA_HTTP_CONTEXT_ESP:
            case OTA_HTTP_CONTEXT_ESP_ROLLBACK:
            case OTA_HTTP_CONTEXT_RECOVERY_EXIT:
            case OTA_HTTP_CONTEXT_FACTORY_RESET:
            default:
                // ESP_ROLLBACK/RECOVERY_EXIT/FACTORY_RESET are listed
                // explicitly even though the claim mutex never actually
                // holds these values (see the comment above) -- collapsing
                // onto ESP here matches heat_interlock.h's own doc comment
                // on heat_interlock_update_context_t (only ESP/PICO exist on
                // that side; ESP_ROLLBACK collapses onto ESP).
                snap.update_context = HEAT_INTERLOCK_UPDATE_ESP;
                break;
        }
    }

    return heat_interlock_check(&snap, reason_out, reason_cap) != HEAT_INTERLOCK_OK;
}

// --- POST /api/ota/esp (TODO.md 9.5, ota_http.h's doc comment) ------------

const char *OTA_MAC_HEADER = "X-Ota-Mac"; // shared by both /api/ota/esp and /api/ota/pico

// See ota_http.h's doc comment above this function's declaration for the
// full contract. Consolidates the "X-Ota-Mac header well-formed -> hex-decode
// -> ota_http_verify_request()" sequence every mutating handler below already
// runs inline, for the first caller OUTSIDE this file (factory_reset.c).
bool ota_http_authenticate_request(httpd_req_t *req, ota_http_context_t ctx, char ip_out[46])
{
    ota_http_get_client_ip(req, ip_out, 46);

    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA authenticate(ctx=%d) from %s: missing or malformed X-Ota-Mac header "
                      "(len %u, want 64)",
                 (int)ctx, ip_out, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return false;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return false;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA authenticate(ctx=%d) from %s: X-Ota-Mac is not valid hex", (int)ctx, ip_out);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return false;
    }

    ota_http_verify_result_t vr = ota_http_verify_request(ctx, mac, ip_out);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return false;
    }
    return true;
}

// See ota_http.h's doc comment above this function's declaration.
bool ota_http_auth_disabled(void)
{
    const char *ap_password = wifi_prov_get_ap_password();
    return !ap_password || ap_password[0] == '\0';
}

// Formats into a comfortably large scratch buffer, then copies (truncating
// if needed, never overflowing) into the caller's smaller `dst`. Used for
// every fail_reason assignment below instead of snprintf() directly into
// fail_reason (OTA_RECORD_REASON_MAX bytes): several of these messages
// interpolate an esp_err_to_name() string or an int whose width the
// compiler cannot bound at a small destination, which -Werror=format-
// truncation (correctly) refuses to build. Formatting into `tmp` first,
// which is sized generously enough that no realistic message here actually
// truncates, sidesteps that without shortening the messages themselves.
void ota_http_set_fail_reason(char *dst, size_t dst_cap, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    strncpy(dst, tmp, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

esp_err_t ota_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                          SafetyLinkClass *safety_or_null)
{
    s_io = io_or_null;
    s_thermo_bus = thermo_bus_or_null;
    ota_http_safety = safety_or_null;

    // Required once before any psa_*() call (hmac_sha256() above) --
    // idempotent per the PSA Crypto API spec, but this is the one place in
    // this codebase that needs it, so it is called here rather than
    // scattered near every HMAC call site.
    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(OTA_HTTP_TAG, "psa_crypto_init failed: %d", (int)psa_status);
        return ESP_FAIL;
    }

    s_ota_lock = xSemaphoreCreateMutex();
    if (!s_ota_lock) {
        return ESP_ERR_NO_MEM;
    }
    ota_http_pico_rollback_async_lock = xSemaphoreCreateMutex();
    if (!ota_http_pico_rollback_async_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
    ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IDLE;
    memset(&s_nonce, 0, sizeof(s_nonce));
    memset(&s_lockout_esp, 0, sizeof(s_lockout_esp));
    memset(&s_lockout_pico, 0, sizeof(s_lockout_pico));
    memset(&s_lockout_esp_rollback, 0, sizeof(s_lockout_esp_rollback));
    /* The fourth context's lockout clears here too. It is a file-scope static
     * so it is already zero at load and nothing is broken today -- but the
     * three lines above exist precisely so that a restart of this layer
     * starts from a clean auth state, and one context quietly keeping its
     * failure count while the others reset is the kind of asymmetry that
     * only shows up the day someone actually calls ota_http_start() twice. */
    memset(&s_lockout_recovery_exit, 0, sizeof(s_lockout_recovery_exit));
    memset(&s_lockout_factory_reset, 0, sizeof(s_lockout_factory_reset));
    s_update_claim = OTA_UPDATE_NONE;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        return ESP_ERR_INVALID_STATE;
    }

    // TODO.md 9.6: the web page itself, GET /ota -- registered first among
    // this file's routes purely because it has no dependency on anything
    // below it; order does not otherwise matter to httpd_register_uri_handler().
    static const httpd_uri_t ota_page_uri = {
        .uri = "/ota", .method = HTTP_GET, .handler = ota_page_get_handler
    };
    esp_err_t err = httpd_register_uri_handler(server, &ota_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/ota) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t challenge_uri = {
        .uri = "/api/ota/challenge", .method = HTTP_GET, .handler = ota_challenge_get_handler
    };
    err = httpd_register_uri_handler(server, &challenge_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/challenge) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // TODO.md 9.5: the ESP's own self-update transfer -- see ota_http.h's
    // doc comment on ota_esp_post_handler() for the wire contract.
    static const httpd_uri_t esp_update_uri = {
        .uri = "/api/ota/esp", .method = HTTP_POST, .handler = ota_esp_post_handler
    };
    err = httpd_register_uri_handler(server, &esp_update_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp) failed: %s", esp_err_to_name(err));
        return err;
    }

    // TODO.md 9.5: the Pico-image relay -- see ota_http.h's doc comment on
    // this pair of handlers for the wire contract and the async response
    // shape.
    static const httpd_uri_t pico_update_uri = {
        .uri = "/api/ota/pico", .method = HTTP_POST, .handler = ota_pico_post_handler
    };
    err = httpd_register_uri_handler(server, &pico_update_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t pico_status_uri = {
        .uri = "/api/ota/pico/status", .method = HTTP_GET, .handler = ota_pico_status_get_handler
    };
    err = httpd_register_uri_handler(server, &pico_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/status) failed: %s", esp_err_to_name(err));
        return err;
    }

    // TODO.md 9.6: interlock state for the web page, before the file picker
    // -- see ota_interlock_get_handler()'s own doc comment for why this is
    // unauthenticated, same exposure level as GET /api/status.
    static const httpd_uri_t interlock_uri = {
        .uri = "/api/ota/interlock", .method = HTTP_GET, .handler = ota_interlock_get_handler
    };
    err = httpd_register_uri_handler(server, &interlock_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/interlock) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Closes the gap flagged in ota_http_client.py's module doc comment and
    // mcp_server.py's ota_status() doc comment -- see ota_esp_status_get_handler()'s
    // own doc comment above for the response shape.
    static const httpd_uri_t esp_status_uri = {
        .uri = "/api/ota/esp/status", .method = HTTP_GET, .handler = ota_esp_status_get_handler
    };
    err = httpd_register_uri_handler(server, &esp_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/status) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Explicit revert to the previous image -- ota_http.h's doc comment
    // above this section for the full contract, and why it needs its own
    // route rather than being folded into POST /api/ota/esp.
    static const httpd_uri_t esp_rollback_uri = {
        .uri = "/api/ota/esp/rollback", .method = HTTP_POST, .handler = ota_esp_rollback_post_handler
    };
    err = httpd_register_uri_handler(server, &esp_rollback_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/rollback) failed: %s", esp_err_to_name(err));
        return err;
    }

    // The Pico half of the same feature -- see ota_pico_rollback_post_
    // handler()'s own doc comment above for the full contract (its own auth
    // context, its own honest accept/refuse/link-down/unknown response
    // shape, built on safety_link_send_rollback_ex()).
    static const httpd_uri_t pico_rollback_uri = {
        .uri = "/api/ota/pico/rollback", .method = HTTP_POST, .handler = ota_pico_rollback_post_handler
    };
    err = httpd_register_uri_handler(server, &pico_rollback_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/rollback) failed: %s", esp_err_to_name(err));
        return err;
    }

    // opus-review finding 3's poll target -- see ota_pico_rollback_status_
    // get_handler()'s own doc comment above.
    static const httpd_uri_t pico_rollback_status_uri = {
        .uri = "/api/ota/pico/rollback/status", .method = HTTP_GET,
        .handler = ota_pico_rollback_status_get_handler
    };
    err = httpd_register_uri_handler(server, &pico_rollback_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/rollback/status) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // boot_guard.h's "a way out of recovery mode" requirement -- see
    // ota_recovery_exit_post_handler()'s own doc comment for the auth
    // (OTA_HTTP_CONTEXT_RECOVERY_EXIT) and recovery-mode-refuses-403 checks
    // it runs, and the ordering between them.
    static const httpd_uri_t recovery_exit_uri = {
        .uri = "/api/ota/esp/recovery_exit", .method = HTTP_POST, .handler = ota_recovery_exit_post_handler
    };
    err = httpd_register_uri_handler(server, &recovery_exit_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/recovery_exit) failed: %s", esp_err_to_name(err));
        return err;
    }

    // docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's
    // follow-up: the TOOL-driven boot_guard_reset_counter() trigger, see
    // ota_boot_guard_reset_post_handler()'s own doc comment
    // (ota_http_recovery.c) for the auth (OTA_HTTP_CONTEXT_BOOT_GUARD_RESET)
    // and why -- unlike recovery_exit -- this one does NOT gate on
    // boot_guard_is_recovery_mode().
    static const httpd_uri_t boot_guard_reset_uri = {
        .uri = "/api/ota/esp/boot_guard_reset", .method = HTTP_POST, .handler = ota_boot_guard_reset_post_handler
    };
    err = httpd_register_uri_handler(server, &boot_guard_reset_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/boot_guard_reset) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Diagnostics follow-up from the same audit -- see
    // ota_boot_guard_status_get_handler()'s own doc comment (unauthenticated,
    // same exposure level as GET /api/status).
    static const httpd_uri_t boot_guard_status_uri = {
        .uri = "/api/boot_guard", .method = HTTP_GET, .handler = ota_boot_guard_status_get_handler
    };
    err = httpd_register_uri_handler(server, &boot_guard_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/boot_guard) failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
