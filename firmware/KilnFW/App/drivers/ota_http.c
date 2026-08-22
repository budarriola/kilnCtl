#include "ota_http.h"

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
#include "esp_random.h"
#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- section 4's Pico-image running CRC32, see ota_pico_do_stage() */
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "ota_http";

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

// The hardware pointers main.c hands to ota_http_start(), same pattern (and
// same NULL-tolerant meaning) as dashboard_http.c's s_dash struct. Read-only
// after ota_http_start(), so no lock needed to read them.
static kiln_io_t *s_io;
static MAX31856BusClass *s_thermo_bus;
static SafetyLinkClass *s_safety;

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

// --- POST /api/ota/esp progress (ota_http.h's ota_http_get_esp_progress()) -
// Single writer (ota_esp_post_handler(), one at a time -- s_update_claim
// above already guarantees no second transfer overlaps it), arbitrarily
// many readers -- `volatile` is enough here, no semaphore needed, per
// ota_http.h's doc comment on why a torn read of a phase enum/percentage
// isn't a correctness problem the way the nonce/lockout state would be.
static volatile ota_http_esp_phase_t s_esp_phase = OTA_HTTP_ESP_PHASE_IDLE;
static volatile uint8_t s_esp_progress_pct = 0;

static void esp_progress_set(ota_http_esp_phase_t phase, uint8_t pct)
{
    s_esp_phase = phase;
    s_esp_progress_pct = pct;
}

void ota_http_get_esp_progress(ota_http_esp_phase_t *phase_out, uint8_t *percent_out)
{
    if (phase_out) {
        *phase_out = s_esp_phase;
    }
    if (percent_out) {
        *percent_out = s_esp_progress_pct;
    }
}

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

// Inverse of hex_encode() -- decodes exactly `hex_len` hex characters (must
// be even) into hex_len/2 bytes. Returns false on any non-hex character or
// odd length, leaving `out` in an unspecified state (callers must check the
// return value before trusting `out`, same convention as every other
// parse-and-validate helper in this codebase, e.g. profiles_http.c's field
// parsers).
static bool hex_decode(const char *hex, size_t hex_len, uint8_t *out)
{
    if (hex_len % 2 != 0) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = -1, lo = -1;
        char ch = hex[2 * i];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        ch = hex[2 * i + 1];
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

// TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
// web_client_accepts_gzip() (the per-file duplicated helpers were folded
// into it) -- absent Accept-Encoding is legal and served gzip per RFC 9110
// s12.5.3; a header that explicitly excludes gzip gets an uncompressed 406.
static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "ota_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)ota_page_html_gz_start,
                            (size_t)(ota_page_html_gz_end - ota_page_html_gz_start));
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
    const char *ctx_str;
    ota_auth_lockout_state_t *lockout;
    switch (ctx) {
        case OTA_HTTP_CONTEXT_ESP:          ctx_str = "esp";          lockout = &s_lockout_esp;          break;
        case OTA_HTTP_CONTEXT_ESP_ROLLBACK: ctx_str = "esp-rollback"; lockout = &s_lockout_esp_rollback; break;
        case OTA_HTTP_CONTEXT_PICO:
        default:                            ctx_str = "pico";         lockout = &s_lockout_pico;         break;
    }
    const char *ip = client_ip ? client_ip : "unknown";

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
    uint8_t msg[OTA_AUTH_NONCE_LEN + 12]; // "esp" (3), "pico" (4), or "esp-rollback" (12) -- 12 covers all three
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

bool ota_http_update_try_begin(ota_http_context_t ctx)
{
    ota_update_claim_t want = (ctx == OTA_HTTP_CONTEXT_ESP) ? OTA_UPDATE_ESP : OTA_UPDATE_PICO;

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Cannot safely check/mutate the claim -- refuse rather than risk
        // two callers both believing they won it, same reasoning as
        // ota_http_verify_request()'s lock-timeout path above.
        ESP_LOGW(TAG, "OTA update claim(%s): internal lock timeout, refused",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
        return false;
    }

    bool won = (s_update_claim == OTA_UPDATE_NONE);
    if (won) {
        s_update_claim = want;
    }
    xSemaphoreGive(s_ota_lock);

    if (won) {
        ESP_LOGI(TAG, "OTA update claim(%s): acquired", ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    } else {
        ESP_LOGW(TAG, "OTA update claim(%s): refused, an update is already in progress",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    }
    return won;
}

void ota_http_update_end(void)
{
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_update_claim != OTA_UPDATE_NONE) {
            ESP_LOGI(TAG, "OTA update claim released");
        }
        s_update_claim = OTA_UPDATE_NONE;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Defensive: could not take the lock to release. Logged loudly
        // because an unreleased claim permanently refuses every future
        // update until reboot -- this must never happen silently.
        ESP_LOGE(TAG, "OTA update claim release: lock timeout -- claim may remain held");
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
        ESP_LOGW(TAG, "OTA update claim query before ota_http_start() -- reporting in-progress (fail-safe)");
        return true;
    }

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        claim = s_update_claim;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Cannot confirm the claim is free -- treat contention as "in
        // progress" rather than risk telling a caller it's safe to start a
        // second update when we simply couldn't check.
        ESP_LOGW(TAG, "OTA update claim query: internal lock timeout, reporting in-progress");
        return true;
    }

    in_progress = (claim != OTA_UPDATE_NONE);
    if (in_progress && out_ctx) {
        *out_ctx = (claim == OTA_UPDATE_ESP) ? OTA_HTTP_CONTEXT_ESP : OTA_HTTP_CONTEXT_PICO;
    }
    return in_progress;
}

ota_interlock_result_t ota_http_check_interlocks(char *reason_out, size_t reason_cap)
{
    ota_interlock_snapshot_t snap = { 0 };

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
    if (s_safety) {
        safety_link_status_t link_status;
        snap.safety_link_up = (safety_link_get_status(s_safety, &link_status) == ESP_OK)
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
        // ESP_ROLLBACK collapses onto ESP -- see heat_interlock.h's doc
        // comment on heat_interlock_update_context_t for why.
        snap.update_context =
            (ctx == OTA_HTTP_CONTEXT_PICO) ? HEAT_INTERLOCK_UPDATE_PICO : HEAT_INTERLOCK_UPDATE_ESP;
    }

    return heat_interlock_check(&snap, reason_out, reason_cap) != HEAT_INTERLOCK_OK;
}

// --- POST /api/ota/esp (TODO.md 9.5, ota_http.h's doc comment) ------------

static const char *OTA_MAC_HEADER = "X-Ota-Mac"; // shared by both /api/ota/esp and /api/ota/pico

// Streamed in fixed-size chunks so the ~1.1-2 MB image never sits in RAM
// whole (UPDATE_PROTOCOL.md section 3: "a full image will not fit in RAM").
// 4 KB, matching the doc's own suggested size. This is `static`, NOT a
// stack buffer -- wifi_provision_http.c's httpd config.stack_size is 8192,
// already sized against the largest existing handler's *smaller* buffers
// (zones_post_handler's 2561-byte body, see that file's comment on the hang/
// reset it caused before being bumped); a 4 KB buffer on top of that same
// stack would eat half of it just for this one variable. Safe as a single
// shared buffer because it is only ever touched while s_update_claim (above)
// is held by THIS transfer -- ota_http_update_try_begin() guarantees no
// second ESP or Pico transfer can be in flight at the same time to race it.
#define OTA_ESP_CHUNK_SIZE 4096
static uint8_t s_ota_esp_chunk[OTA_ESP_CHUNK_SIZE];

static const char *verify_result_str(ota_http_verify_result_t r)
{
    switch (r) {
        case OTA_HTTP_VERIFY_OK: return "ok";
        case OTA_HTTP_VERIFY_LOCKED_OUT: return "locked out -- too many recent wrong-password attempts";
        case OTA_HTTP_VERIFY_NO_VALID_NONCE:
            return "no valid challenge -- GET /api/ota/challenge first, then POST within 30 s";
        case OTA_HTTP_VERIFY_BAD_MAC: return "wrong password";
        default: return "authentication failed";
    }
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
static void set_fail_reason(char *dst, size_t dst_cap, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    strncpy(dst, tmp, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

// Everything from "the mutex is held" to "the mutex is released" -- a
// single function so ota_esp_post_handler() below has exactly one call site
// for ota_http_update_end(), per TODO.md 9.5's "use a single cleanup path,
// not duplicated calls at every return" requirement. Every exit -- success,
// a refused/corrupt image, a mid-transfer read/write failure -- sets
// `ok`/`fail_reason` and falls through to the one cleanup block at the
// bottom, which appends the NVS record and updates the progress snapshot
// exactly once regardless of which path got there.
static void ota_esp_do_transfer(httpd_req_t *req, const char *ip)
{
    bool ok = false;
    char fail_reason[OTA_RECORD_REASON_MAX] = "unknown failure";
    char version_after[OTA_RECORD_VERSION_STR_MAX] = "";
    esp_ota_handle_t handle = 0;
    bool ota_began = false;
    const esp_partition_t *target = NULL;

    // Image SHA-256, computed over exactly the bytes esp_ota_write() is
    // given (the 24-byte header first, then every streamed chunk) -- a
    // record of what was actually written, not a gate (see ota_record.h's
    // header comment: nothing compares this against an expected value).
    // Best-effort: a PSA failure here logs and leaves the record's hash
    // field empty rather than failing an otherwise-good transfer over it.
    psa_hash_operation_t sha_op = psa_hash_operation_init();
    bool sha_op_active = false;
    char sha_hex[OTA_RECORD_SHA256_HEX_MAX] = "";

    const esp_app_desc_t *running_desc = esp_app_get_description();
    const char *version_before = (running_desc && running_desc->version[0]) ? running_desc->version : "";

    size_t content_len = req->content_len;
    if (content_len == 0) {
        set_fail_reason(fail_reason, sizeof(fail_reason), "missing Content-Length / empty body");
        ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        set_fail_reason(fail_reason, sizeof(fail_reason), "no free OTA partition");
        ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    if (content_len > target->size) {
        set_fail_reason(fail_reason, sizeof(fail_reason), "image (%u B) larger than the OTA partition (%u B)",
                 (unsigned)content_len, (unsigned)target->size);
        ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    // Per-connection socket timeout, NOT the server-wide default (that
    // stays whatever wifi_provision_http.c's config sets and applies to
    // every other endpoint) -- see ota_http.h's doc comment for why a
    // targeted setsockopt() here is the chosen fix over a global config
    // bump. This is a PER-RECV timeout (how long to wait for the NEXT
    // chunk to arrive), not a whole-transfer deadline -- TCP keeps
    // delivering chunks well inside this window on any link that is
    // actually making progress, so bounding each individual recv() call
    // at 30 s is what "covers the whole transfer" means in practice: the
    // total transfer can take minutes as long as no single gap between
    // chunks exceeds 30 s. A dead connection still times out and is
    // cleaned up; a slow-but-alive one is not punished for its aggregate
    // duration.
    {
        int sockfd = httpd_req_to_sockfd(req);
        if (sockfd >= 0) {
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                ESP_LOGW(TAG, "OTA esp update from %s: could not raise the socket receive timeout -- "
                              "the server-wide default will apply instead",
                         ip);
            }
        }
    }

    // Buffer just the image header (24 B) before esp_ota_begin() --
    // UPDATE_PROTOCOL.md section 3: "ESP-IDF images already carry a magic
    // byte and a chip ID; verify them before calling esp_ota_begin()."
    esp_progress_set(OTA_HTTP_ESP_PHASE_VERIFYING, 0);
    {
        esp_image_header_t hdr;
        size_t hdr_received = 0;
        while (hdr_received < sizeof(hdr)) {
            int ret = httpd_req_recv(req, ((char *)&hdr) + hdr_received, sizeof(hdr) - hdr_received);
            if (ret <= 0) {
                set_fail_reason(fail_reason, sizeof(fail_reason), "body read failed/closed while reading the image header (%d)", ret);
                ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed while reading image header");
                goto cleanup;
            }
            hdr_received += (size_t)ret;
        }

        if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "not an ESP-IDF image (bad magic 0x%02X)", hdr.magic);
            ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a valid ESP-IDF image (bad magic)");
            goto cleanup;
        }
        if (hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "image is for chip id %u, this board is ESP32-S3 (%u)",
                     (unsigned)hdr.chip_id, (unsigned)ESP_CHIP_ID_ESP32S3);
            ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is built for a different chip");
            goto cleanup;
        }

        esp_err_t rc = esp_ota_begin(target, content_len, &handle);
        if (rc != ESP_OK) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_begin failed: %s", esp_err_to_name(rc));
            ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
            goto cleanup;
        }
        ota_began = true;

        psa_status_t hs = psa_hash_setup(&sha_op, PSA_ALG_SHA_256);
        sha_op_active = (hs == PSA_SUCCESS);
        if (!sha_op_active) {
            ESP_LOGW(TAG, "OTA esp update from %s: psa_hash_setup failed (%d) -- record will have no "
                          "image hash, transfer continues",
                     ip, (int)hs);
        }

        rc = esp_ota_write(handle, &hdr, sizeof(hdr));
        if (rc != ESP_OK) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_write (header) failed: %s", esp_err_to_name(rc));
            ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            goto cleanup;
        }
        if (sha_op_active) {
            (void)psa_hash_update(&sha_op, (const uint8_t *)&hdr, sizeof(hdr));
        }
    }

    // Stream the rest. Never read ahead of what esp_ota_write() has
    // consumed -- each loop iteration reads one chunk and writes it before
    // asking for the next, so TCP flow control (not a read-ahead buffer
    // with nowhere to go) paces the transfer, per UPDATE_PROTOCOL.md's
    // "do not read the request body faster than the link drains."
    {
        size_t written = sizeof(esp_image_header_t);
        int last_logged_decile = 0;
        esp_progress_set(OTA_HTTP_ESP_PHASE_WRITING, 0);
        while (written < content_len) {
            size_t want = content_len - written;
            if (want > sizeof(s_ota_esp_chunk)) {
                want = sizeof(s_ota_esp_chunk);
            }
            int ret = httpd_req_recv(req, (char *)s_ota_esp_chunk, want);
            if (ret <= 0) {
                set_fail_reason(fail_reason, sizeof(fail_reason), "body read failed/closed at %u/%u bytes (%d)",
                         (unsigned)written, (unsigned)content_len, ret);
                ESP_LOGW(TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed mid-transfer");
                goto cleanup;
            }

            esp_err_t rc = esp_ota_write(handle, s_ota_esp_chunk, (size_t)ret);
            if (rc != ESP_OK) {
                set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_write failed at %u bytes: %s",
                         (unsigned)written, esp_err_to_name(rc));
                ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
                goto cleanup;
            }
            if (sha_op_active) {
                (void)psa_hash_update(&sha_op, s_ota_esp_chunk, (size_t)ret);
            }
            written += (size_t)ret;

            // Progress every ~10% (TODO.md 9.5: "progress pushed... at
            // least every 2 s" -- decile logging on a multi-second/minute
            // transfer satisfies that cadence without flooding the log on
            // a fast LAN).
            int decile = (int)((written * 10u) / content_len);
            if (decile > last_logged_decile) {
                last_logged_decile = decile;
                uint8_t pct = (uint8_t)((written * 100u) / content_len);
                esp_progress_set(OTA_HTTP_ESP_PHASE_WRITING, pct);
                ESP_LOGI(TAG, "OTA esp update from %s: %u%% (%u/%u bytes)", ip, pct,
                         (unsigned)written, (unsigned)content_len);
            }
        }
    }

    esp_progress_set(OTA_HTTP_ESP_PHASE_FINALIZING, 100);
    {
        esp_err_t rc = esp_ota_end(handle);
        // esp_ota_end() frees the handle regardless of its result (see its
        // own doc comment) -- ota_began must go false here either way so
        // the cleanup block below never calls esp_ota_abort() on a handle
        // that no longer exists.
        ota_began = false;
        if (rc != ESP_OK) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_end failed: %s (image validation failed?)",
                     esp_err_to_name(rc));
            ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "esp_ota_end failed -- image rejected");
            goto cleanup;
        }

        rc = esp_ota_set_boot_partition(target);
        if (rc != ESP_OK) {
            set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_set_boot_partition failed: %s", esp_err_to_name(rc));
            ESP_LOGE(TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not set boot partition -- "
                                                                       "old image is still active");
            goto cleanup;
        }
    }

    // Real, not a placeholder: read back the app description from the
    // partition that was just written, the same way esp_ota_get_partition_
    // description() is documented to be used for an inactive slot's
    // version. Best-effort -- a failure here does not undo a successful
    // update, it just leaves version_after blank in the record.
    {
        esp_app_desc_t written_desc;
        if (esp_ota_get_partition_description(target, &written_desc) == ESP_OK) {
            strncpy(version_after, written_desc.version, sizeof(version_after) - 1);
            version_after[sizeof(version_after) - 1] = '\0';
        }
    }

    ok = true;
    strncpy(fail_reason, "ok", sizeof(fail_reason));
    ESP_LOGI(TAG, "OTA esp update from %s: complete, %u bytes written to '%s', now %s -- "
                  "reboot required to run it (this image stays PENDING_VERIFY until "
                  "ota_rollback_confirm_task() in main.c confirms it)",
             ip, (unsigned)content_len, target->label, version_after[0] ? version_after : "(unknown version)");

cleanup:
    if (ota_began) {
        // Any goto above that happens after esp_ota_begin() succeeded but
        // before esp_ota_end() ran leaves ota_began true -- abort so the
        // partial write can never be selected as a boot target.
        esp_ota_abort(handle);
    }

    // Finish (on success -- the hash covers exactly the bytes that made it
    // into the flash write path) or abort (on failure -- PSA requires every
    // started operation to be finished or aborted, and a failed transfer's
    // partial hash is not meaningful anyway) whatever hash operation was
    // started above. sha_hex stays "" if no operation was ever started, or
    // if psa_hash_finish() itself failed.
    if (sha_op_active) {
        if (ok) {
            uint8_t digest[32];
            size_t digest_len = 0;
            psa_status_t hs = psa_hash_finish(&sha_op, digest, sizeof(digest), &digest_len);
            if (hs == PSA_SUCCESS && digest_len == sizeof(digest)) {
                hex_encode(digest, sizeof(digest), sha_hex);
            } else {
                ESP_LOGW(TAG, "OTA esp update from %s: psa_hash_finish failed (%d) -- record will "
                              "have no image hash",
                         ip, (int)hs);
            }
        } else {
            (void)psa_hash_abort(&sha_op);
        }
    }

    esp_progress_set(ok ? OTA_HTTP_ESP_PHASE_DONE : OTA_HTTP_ESP_PHASE_FAILED,
                      ok ? 100 : s_esp_progress_pct);

    {
        ota_record_t rec;
        ota_record_fill(&rec, (uint32_t)(esp_timer_get_time() / 1000000), "esp", version_before,
                         version_after, ok, fail_reason, sha_hex);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    if (ok) {
        char body[128];
        int n = snprintf(body, sizeof(body), "{\"ok\":true,\"bytes\":%u,\"partition\":\"%s\",\"version\":\"%s\"}",
                          (unsigned)content_len, target->label, version_after);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, body, n);
    }
    // On failure, the specific httpd_resp_send_err() call above (at
    // whichever goto fired) has already sent the response -- nothing left
    // to send here.

    // Released exactly once, regardless of which path got here -- the
    // single-cleanup-path requirement this whole function exists to
    // satisfy. Idempotent even if something above went wrong before the
    // claim was actually held, per ota_http_update_end()'s own doc comment.
    ota_http_update_end();
}

static esp_err_t ota_esp_post_handler(httpd_req_t *req)
{
    char ip[46];
    get_client_ip(req, ip, sizeof(ip));

    // 1. X-Ota-Mac header present and exactly 64 hex chars -- refused
    // before the body is touched at all, per ota_http.h's documented order.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(TAG, "OTA esp update from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
                 ip, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return ESP_OK;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return ESP_OK;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(TAG, "OTA esp update from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    // 2. Auth -- refuse immediately (403) on anything but OK, still before
    // the body is read.
    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    // 3. Interlocks -- run AFTER auth (see ota_http_check_interlocks()'s own
    // doc comment for why: an unauthenticated interlock check would leak
    // live kiln telemetry). esp_http_server's httpd_err_code_t has no 409
    // entry, so the "409 Conflict" status TODO.md asks for ("409 or
    // similar") is set directly via httpd_resp_set_status() rather than
    // httpd_resp_send_err(), which only knows the enum's fixed set.
    char reason[OTA_INTERLOCK_REASON_MAX];
    if (ota_http_check_interlocks(reason, sizeof(reason)) != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "OTA esp update from %s: refused by interlock: %s", ip, reason);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 4. Single update mutex -- claimed before any body byte is read, so a
    // second concurrent attempt (another tab, an agent racing a human) is
    // refused immediately rather than partway through a transfer.
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(TAG, "OTA esp update from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // From here, the mutex is held and ota_esp_do_transfer() owns releasing
    // it exactly once, on every exit path -- see that function's own doc
    // comment.
    ota_esp_do_transfer(req, ip);
    return ESP_OK;
}

// --- POST /api/ota/pico, GET /api/ota/pico/status (TODO.md 9.5) -----------
// See ota_http.h's header comment on this section for the full wire
// contract and the mutex-ownership handoff to ota_pico_relay.c.

// Same static-not-stack reasoning as OTA_ESP_CHUNK_SIZE/s_ota_esp_chunk
// above. A SEPARATE buffer rather than reusing s_ota_esp_chunk: the two
// could technically share one (the cross-processor update mutex guarantees
// only one of the ESP or Pico transfer is ever in flight at a time), but
// keeping them distinct keeps each transfer's code readable on its own
// without a reader having to go verify that cross-file invariant first.
#define OTA_PICO_CHUNK_SIZE 4096
static uint8_t s_ota_pico_chunk[OTA_PICO_CHUNK_SIZE];

// Streams the browser upload into `pico_img`, computing a running CRC32
// alongside it (esp_rom_crc32_le() -- see ota_http.h's header comment for
// why this, not a second hand-rolled CRC32, is used: it is the same
// IEEE 802.3/zlib algorithm SaftyFW's bootloader/crc32.c implements,
// confirmed by reading both this header's own doc comment and that file --
// same poly 0xEDB88320 reflected, same init/final XOR 0xFFFFFFFF, reached
// via esp_rom_crc32_le(0xFFFFFFFF, ...) chained across chunks then a final
// XOR, per esp_rom_crc.h's own "add ~ at the beginning and the end" chaining
// recipe). On success, hands off to ota_pico_relay_start() and returns
// without releasing the update mutex (see ota_http.h's header comment for
// why); on any failure, releases the mutex itself and responds with a
// specific error.
static void ota_pico_do_stage(httpd_req_t *req, const char *ip)
{
    bool started_relay = false;
    char fail_reason[256] = "unknown failure";
    // Declared up here, not at first use, so every goto below (including
    // the very first check) can jump straight to the single cleanup block
    // without skipping past an initializer -- same discipline
    // ota_esp_do_transfer() uses for handle/ota_began/target.
    size_t content_len = 0;
    const esp_partition_t *part = NULL;
    uint32_t crc = 0xFFFFFFFFu; // esp_rom_crc.h's own chaining recipe -- see this function's doc comment
    size_t written = 0;

    // Image SHA-256 over every byte staged into pico_img -- same
    // best-effort, record-not-gate reasoning as ota_esp_do_transfer()'s own
    // copy of this pattern (see ota_record.h's header comment). The 32-byte
    // digest (not the hex string) is handed to ota_pico_relay_start(),
    // which owns turning it into the eventual ota_record_t for the Pico
    // path -- this function's own job ends at "staged successfully, relay
    // started."
    psa_hash_operation_t sha_op = psa_hash_operation_init();
    bool sha_op_active = false;
    uint8_t sha_digest[32];
    bool have_sha_digest = false;

    if (!s_safety) {
        snprintf(fail_reason, sizeof(fail_reason), "no safety link configured this boot -- nothing to relay to");
        ESP_LOGE(TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }

    content_len = req->content_len;
    if (content_len == 0) {
        snprintf(fail_reason, sizeof(fail_reason), "missing Content-Length / empty body");
        ESP_LOGW(TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    part = ota_pico_img_partition();
    if (!part) {
        snprintf(fail_reason, sizeof(fail_reason), "pico_img staging partition not found");
        ESP_LOGE(TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    if (content_len > part->size) {
        snprintf(fail_reason, sizeof(fail_reason), "image (%u B) larger than the pico_img partition (%u B)",
                 (unsigned)content_len, (unsigned)part->size);
        ESP_LOGW(TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    // Same per-connection socket timeout rationale as ota_esp_do_transfer()
    // above -- a per-recv-call bound, not a whole-transfer deadline.
    {
        int sockfd = httpd_req_to_sockfd(req);
        if (sockfd >= 0) {
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                ESP_LOGW(TAG, "OTA pico update from %s: could not raise the socket receive timeout", ip);
            }
        }
    }

    // Erase only what this upload needs, rounded up to the flash sector
    // size esp_partition_write() requires already-erased -- not the whole
    // 896K partition, which would cost real time for no benefit on a
    // typical (much smaller) Pico image.
    {
        uint32_t sector = esp_partition_get_main_flash_sector_size();
        size_t erase_len = ((content_len + sector - 1u) / sector) * sector;
        esp_err_t erc = esp_partition_erase_range(part, 0, erase_len);
        if (erc != ESP_OK) {
            snprintf(fail_reason, sizeof(fail_reason), "pico_img erase failed: %s", esp_err_to_name(erc));
            ESP_LOGE(TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash erase failed");
            goto cleanup;
        }
    }

    {
        psa_status_t hs = psa_hash_setup(&sha_op, PSA_ALG_SHA_256);
        sha_op_active = (hs == PSA_SUCCESS);
        if (!sha_op_active) {
            ESP_LOGW(TAG, "OTA pico update from %s: psa_hash_setup failed (%d) -- record will have no "
                          "image hash, staging continues",
                     ip, (int)hs);
        }
    }

    // Stream the body into pico_img, one httpd_req_recv() per
    // esp_partition_write(), same "never read ahead of what has been
    // consumed" discipline as ota_esp_do_transfer() -- and the same reason
    // it matters here: UPDATE_PROTOCOL.md's "do not read the request body
    // faster than the link drains" is about the SLOW isolated-link relay
    // that happens after this handler returns, but reading the HTTP body
    // no faster than it can be written to flash is the same principle
    // applied to this (fast) staging step.
    int last_logged_decile = 0;
    while (written < content_len) {
        size_t want = content_len - written;
        if (want > sizeof(s_ota_pico_chunk)) {
            want = sizeof(s_ota_pico_chunk);
        }
        int ret = httpd_req_recv(req, (char *)s_ota_pico_chunk, want);
        if (ret <= 0) {
            snprintf(fail_reason, sizeof(fail_reason), "body read failed/closed at %u/%u bytes (%d)",
                     (unsigned)written, (unsigned)content_len, ret);
            ESP_LOGW(TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed mid-transfer");
            goto cleanup;
        }

        esp_err_t werr = esp_partition_write(part, written, s_ota_pico_chunk, (size_t)ret);
        if (werr != ESP_OK) {
            snprintf(fail_reason, sizeof(fail_reason), "pico_img write failed at %u bytes: %s",
                     (unsigned)written, esp_err_to_name(werr));
            ESP_LOGE(TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            goto cleanup;
        }
        crc = esp_rom_crc32_le(crc, s_ota_pico_chunk, (uint32_t)ret);
        if (sha_op_active) {
            (void)psa_hash_update(&sha_op, s_ota_pico_chunk, (size_t)ret);
        }
        written += (size_t)ret;

        int decile = (int)((written * 10u) / content_len);
        if (decile > last_logged_decile) {
            last_logged_decile = decile;
            ESP_LOGI(TAG, "OTA pico update from %s: staged %u%% (%u/%u bytes)", ip,
                     (unsigned)((written * 100u) / content_len), (unsigned)written, (unsigned)content_len);
        }
    }
    crc ^= 0xFFFFFFFFu; // final XOR -- see this function's doc comment

    if (sha_op_active) {
        size_t digest_len = 0;
        psa_status_t hs = psa_hash_finish(&sha_op, sha_digest, sizeof(sha_digest), &digest_len);
        if (hs == PSA_SUCCESS && digest_len == sizeof(sha_digest)) {
            have_sha_digest = true;
        } else {
            ESP_LOGW(TAG, "OTA pico update from %s: psa_hash_finish failed (%d) -- record will have no "
                          "image hash",
                     ip, (int)hs);
        }
        sha_op_active = false; // finished (or failed to finish) -- nothing left to abort in cleanup
    }

    ESP_LOGI(TAG, "OTA pico update from %s: staged %u bytes to pico_img, crc32=0x%08X -- starting relay",
             ip, (unsigned)written, (unsigned)crc);

    if (!ota_pico_relay_start(s_safety, (uint32_t)written, crc, NULL, have_sha_digest ? sha_digest : NULL)) {
        snprintf(fail_reason, sizeof(fail_reason), "image staged, but the relay task could not be started");
        ESP_LOGE(TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    started_relay = true;

    {
        char body[160];
        int n = snprintf(body, sizeof(body),
                          "{\"ok\":true,\"status\":\"relay_started\",\"bytes\":%u,\"crc32\":\"0x%08X\"}",
                          (unsigned)written, (unsigned)crc);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, body, n);
    }

cleanup:
    // Any goto above that fired while sha_op_active was still true left a
    // PSA hash operation started-but-not-finished (a read/write failure
    // mid-stream, staging failing before ota_pico_relay_start() -- the
    // finish-or-fail-fast block above already turned sha_op_active back to
    // false on every path that actually reached it). PSA requires every
    // started operation to be finished or aborted; abort here rather than
    // leak it, same "always release the resource this function borrowed"
    // discipline as the ota_began/esp_ota_abort() cleanup in
    // ota_esp_do_transfer().
    if (sha_op_active) {
        (void)psa_hash_abort(&sha_op);
    }

    // Ownership handoff: if the relay task was successfully started, IT now
    // owns calling ota_http_update_end() (see ota_http.h's header comment
    // and ota_pico_relay.h's own for the full reasoning) -- calling it here
    // too would release a claim the relay task is still actively using.
    // Every OTHER path above (staging never got far enough to start a
    // relay) still owns cleanup itself, exactly like ota_esp_do_transfer()'s
    // single cleanup block.
    if (!started_relay) {
        ota_http_update_end();
    }
}

static esp_err_t ota_pico_post_handler(httpd_req_t *req)
{
    char ip[46];
    get_client_ip(req, ip, sizeof(ip));

    // Same four-step order as ota_esp_post_handler() -- see ota_http.h's
    // documented order and that handler's own comments for why each step
    // precedes the next.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(TAG, "OTA pico update from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
                 ip, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return ESP_OK;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return ESP_OK;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(TAG, "OTA pico update from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    char reason[OTA_INTERLOCK_REASON_MAX];
    if (ota_http_check_interlocks(reason, sizeof(reason)) != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "OTA pico update from %s: refused by interlock: %s", ip, reason);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO)) {
        ESP_LOGW(TAG, "OTA pico update from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // From here, ota_pico_do_stage() owns the mutex -- either it releases
    // it itself (staging failure) or it starts the relay task, which then
    // owns release. See that function's own doc comment.
    ota_pico_do_stage(req, ip);
    return ESP_OK;
}

// String form of ota_http_esp_phase_t, same "each getter's phase enum gets
// exactly one string table, used only by its own status handler" precedent
// ota_pico_relay_phase_str() sets for the Pico side -- no shared enum/string
// mapping exists between the two processors' phases, and there is no reason
// to invent one here.
static const char *esp_phase_str(ota_http_esp_phase_t phase)
{
    switch (phase) {
        case OTA_HTTP_ESP_PHASE_IDLE:       return "idle";
        case OTA_HTTP_ESP_PHASE_VERIFYING:  return "verifying";
        case OTA_HTTP_ESP_PHASE_WRITING:    return "writing";
        case OTA_HTTP_ESP_PHASE_FINALIZING: return "finalizing";
        case OTA_HTTP_ESP_PHASE_DONE:       return "done";
        case OTA_HTTP_ESP_PHASE_FAILED:     return "failed";
        default:                            return "unknown";
    }
}

// GET /api/ota/esp/status -- see ota_http.h's doc comment above
// ota_http_get_esp_progress() for the full field-by-field contract. Closes
// the gap ota_http_client.py's module doc comment and mcp_server.py's
// ota_status() doc comment both flagged: neither the ESP self-update's own
// progress nor the persisted ota_record.h "last update" blob had an HTTP
// route before this handler.
static esp_err_t ota_esp_status_get_handler(httpd_req_t *req)
{
    ota_http_esp_phase_t phase;
    uint8_t percent;
    ota_http_get_esp_progress(&phase, &percent);

    // ota_record_load() is null-tolerant on "no record yet" the same way
    // safety_link_get_status() is null-tolerant on "no link this boot" --
    // ESP_ERR_NVS_NOT_FOUND (or any other non-OK, e.g. NVS partition not
    // yet initialized) means "nothing to report", not an error worth
    // failing this GET over. last_update stays absent (JSON null) in
    // exactly that case.
    ota_record_t rec;
    bool have_record = (ota_record_load(&rec) == ESP_OK);

    // TODO.md 9.6: "running version, build commit, build date, dirty flag,
    // active slot, and the version sitting in the inactive slot" -- none of
    // this was on any existing HTTP route before this pass (dashboard_http.c's
    // /api/status has no such fields; grepped for esp_app_get_description/
    // FW_GIT_COMMIT/esp_ota_get_running_partition there and found nothing).
    // Added directly to this already-existing status route rather than a new
    // one, same "small, contained addition to an existing endpoint" the
    // Pico-status handler below also gets for its own available fields.
    const esp_app_desc_t *running_desc = esp_app_get_description();
    const char *running_version = (running_desc && running_desc->version[0]) ? running_desc->version : "";
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    const char *active_slot = running_part ? running_part->label : "unknown";

    const esp_partition_t *inactive_part = esp_ota_get_next_update_partition(NULL);
    const char *inactive_slot = inactive_part ? inactive_part->label : "unknown";
    char inactive_version[33] = "";
    if (inactive_part) {
        esp_app_desc_t inactive_desc;
        if (esp_ota_get_partition_description(inactive_part, &inactive_desc) == ESP_OK) {
            strncpy(inactive_version, inactive_desc.version, sizeof(inactive_version) - 1);
            inactive_version[sizeof(inactive_version) - 1] = '\0';
        }
        // Left blank (not "unknown") when the inactive slot has no readable
        // app descriptor -- an erased/never-flashed factory or ota_1
        // partition on a fresh board is a real, common state, not an error;
        // the page renders an empty string as "(empty)" itself.
    }

    // rec.reason (ota_record_fill()'s callers, ota_esp_do_transfer() above)
    // is always this codebase's own snprintf() output -- never copied
    // verbatim from an external source -- so, same as
    // ota_pico_status_get_handler()'s last_error field below, it cannot
    // contain a raw '"' or '\' that would need JSON escaping here. version/
    // active_slot/inactive_slot are equally safe: version comes from this
    // firmware's own PROJECT_VER (esp_app_desc_t), the slot labels come from
    // the partition table (esp_partition_t::label), and inactive_version
    // comes from the SAME struct field on a partition this build itself
    // wrote (or its factory-default) -- none of these are attacker-supplied.
    char body[720];
    int n;
    if (have_record) {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,"
                      "\"version\":\"%s\",\"commit\":\"%s\",\"dirty\":%s,\"build_date\":\"%s\","
                      "\"active_slot\":\"%s\",\"inactive_slot\":\"%s\",\"inactive_version\":\"%s\","
                      "\"last_update\":"
                      "{\"processor\":\"%s\",\"version_before\":\"%s\",\"version_after\":\"%s\","
                      "\"success\":%s,\"reason\":\"%s\",\"uptime_s\":%u,\"image_sha256\":\"%s\"}}",
                      esp_phase_str(phase), (unsigned)percent,
                      running_version, FW_GIT_COMMIT, FW_GIT_DIRTY ? "true" : "false",
                      FW_BUILD_DATE " " FW_BUILD_TIME, active_slot, inactive_slot, inactive_version,
                      rec.processor, rec.version_before,
                      rec.version_after, rec.success ? "true" : "false", rec.reason,
                      (unsigned)rec.uptime_s, rec.image_sha256_hex);
    } else {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,"
                      "\"version\":\"%s\",\"commit\":\"%s\",\"dirty\":%s,\"build_date\":\"%s\","
                      "\"active_slot\":\"%s\",\"inactive_slot\":\"%s\",\"inactive_version\":\"%s\","
                      "\"last_update\":null}",
                      esp_phase_str(phase), (unsigned)percent,
                      running_version, FW_GIT_COMMIT, FW_GIT_DIRTY ? "true" : "false",
                      FW_BUILD_DATE " " FW_BUILD_TIME, active_slot, inactive_slot, inactive_version);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

// --- POST /api/ota/esp/rollback -- see ota_http.h's doc comment above this
// section for the full contract. Runs on its own short-lived task (same
// factory_reset.c reboot_task() pattern) so the JSON response already
// queued by the handler has a chance to reach the client before the
// connection is torn down by the reboot.
static void ota_rollback_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    // KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP
    // reboots" line: this call to esp_ota_mark_app_invalid_rollback_and_
    // reboot() below is the one existing path in this file that actually
    // calls esp_restart() (the plain OTA transfer path, ota_esp_do_
    // transfer(), only sets the boot partition and does not itself reboot --
    // see that function's own doc note -- so it has no reboot moment to hook
    // yet; when it grows one, it must send this too). Best-effort: a failed
    // send here does not block or abort the reboot -- worst case SaftyFW's
    // S6b guard behaves exactly as it did before this feature existed, which
    // is the same "no announcement" fallback the grace window itself
    // degrades to on expiry.
    esp_err_t announce_err = safety_link_send_announce_reboot(s_safety);
    if (announce_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA rollback: safety_link_send_announce_reboot failed (%s) -- "
                      "rebooting anyway, S6b may nuisance-trip on the safety processor",
                 esp_err_to_name(announce_err));
    }

    ESP_LOGW(TAG, "OTA rollback: rebooting now into the previous image");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only reached if the call itself failed to even start the reboot --
    // on success this line never runs, the board is already restarting.
    ESP_LOGE(TAG, "esp_ota_mark_app_invalid_rollback_and_reboot failed: %s -- "
                  "board NOT rebooted, still running the current image",
             esp_err_to_name(err));
}

static esp_err_t ota_esp_rollback_post_handler(httpd_req_t *req)
{
    char ip[46];
    get_client_ip(req, ip, sizeof(ip));

    // 1. X-Ota-Mac header present and exactly 64 hex chars -- same order as
    // ota_esp_post_handler(), before anything else is checked.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(TAG, "OTA esp rollback from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
                 ip, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return ESP_OK;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return ESP_OK;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(TAG, "OTA esp rollback from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    // 2. Auth -- its own context (OTA_HTTP_CONTEXT_ESP_ROLLBACK), see
    // ota_http.h's doc comment on that enum value for why a rollback MAC is
    // not interchangeable with a plain-update MAC.
    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP_ROLLBACK, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    // 3. Interlocks -- identical gate to POST /api/ota/esp: a rollback
    // reboots into different code just like an update does, so it is
    // exactly as disruptive and must be refused under the same conditions
    // (kiln not idle/cool, safety link down, another update in progress, ...).
    char reason[OTA_INTERLOCK_REASON_MAX];
    if (ota_http_check_interlocks(reason, sizeof(reason)) != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "OTA esp rollback from %s: refused by interlock: %s", ip, reason);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 4. Single update mutex -- claimed as OTA_HTTP_CONTEXT_ESP (not a
    // separate rollback slot): a rollback is exactly as mutually exclusive
    // with an in-flight ESP or Pico update as a second ESP update would be,
    // there is still only one slot.
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(TAG, "OTA esp rollback from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 5. Is there actually a previous valid image to roll back to? Checked
    // explicitly rather than calling esp_ota_mark_app_invalid_rollback_and_
    // reboot() blind and letting it discover there is nothing -- refuses
    // cleanly, naming the reason, same as every other interlock in this file.
    if (!esp_ota_check_rollback_is_possible()) {
        ESP_LOGW(TAG, "OTA esp rollback from %s: refused, no previous valid image to roll back to", ip);
        ota_http_update_end();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no previous valid image to roll back to", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const esp_app_desc_t *running_desc = esp_app_get_description();
    const char *version_before = (running_desc && running_desc->version[0]) ? running_desc->version : "";

    {
        ota_record_t rec;
        // No image hash for a rollback record -- this action reverts to the
        // PREVIOUS image (already written and hashed, if at all, by whatever
        // update put it there), it does not write new bytes for this record
        // to hash.
        ota_record_fill(&rec, (uint32_t)(esp_timer_get_time() / 1000000), "esp", version_before,
                         "", true, "rollback requested", NULL);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    ESP_LOGW(TAG, "OTA esp rollback from %s: accepted, was running '%s' -- rebooting into the "
                  "previous image", ip, version_before[0] ? version_before : "(unknown version)");

    char body[96];
    int n = snprintf(body, sizeof(body), "{\"ok\":true,\"status\":\"rebooting\",\"version_before\":\"%s\"}",
                      version_before);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);

    // The mutex is intentionally left held across the reboot -- there is no
    // "release it after the transfer" moment here the way ota_esp_do_
    // transfer()'s cleanup path has, because the board is about to reboot
    // out from under this claim entirely. A fresh boot starts with
    // s_update_claim reset to OTA_UPDATE_NONE (ota_http_start()), so there
    // is nothing left to release.
    if (xTaskCreate(ota_rollback_reboot_task, "ota_rollback_reboot", 3072, NULL,
                     tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OTA esp rollback from %s: failed to start the reboot task -- "
                      "board will NOT reboot, still running the current image", ip);
        ota_http_update_end();
    }

    return ESP_OK;
}

// GET /api/ota/interlock -- TODO.md 9.6: "interlock state shown BEFORE the
// file picker, with the blocker named." ota_http_check_interlocks() itself
// is only ever called from inside the authenticated POST /api/ota/{esp,pico}
// handlers (see ota_http.h's doc comment above that function: an
// unauthenticated caller would learn live kiln telemetry, e.g. "zone 2 is at
// 340 C", folded into the refusal reason string). That reasoning is sound in
// isolation, but this codebase's own GET /api/status (dashboard_http.c) is
// ALREADY unauthenticated and already returns every zone's live temperature
// directly -- so gating this endpoint behind the OTA challenge/HMAC dance
// (which would force the web page to ask for the Wi-Fi AP password just to
// show "kiln is running a profile" before the file picker even appears)
// would not close any exposure that isn't already open on this same LAN.
// Unauthenticated here, matching /api/status's existing exposure level, not
// a new one. Returns {"ok":true} or {"ok":false,"reason":"<why>"}.
static esp_err_t ota_interlock_get_handler(httpd_req_t *req)
{
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t r = ota_http_check_interlocks(reason, sizeof(reason));

    char body[OTA_INTERLOCK_REASON_MAX + 32];
    int n;
    if (r == OTA_INTERLOCK_OK) {
        n = snprintf(body, sizeof(body), "{\"ok\":true}");
    } else {
        n = snprintf(body, sizeof(body), "{\"ok\":false,\"reason\":\"%s\"}", reason);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t ota_pico_status_get_handler(httpd_req_t *req)
{
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);

    // TODO.md 9.6: the Pico half of "running version, build commit, build
    // date, dirty flag, active slot, inactive slot" -- and here the honest
    // answer is that most of it does NOT exist over this link. Per
    // safety_link.h's own header comment (SAFETY_CMD_FW_VERSION) the Pico's
    // reply carries protocol/min_compatible/dirty/commit/datetime/boot_id,
    // but safety_apply_fw_version()/safety_parse_fw_version() (safety_link.c)
    // only extract protocol/min_compatible/boot_id -- dirty/commit/datetime
    // are parsed past (to find boot_id's offset) and then discarded, never
    // stored in safety_link_status_t. There is also no concept of an
    // "active/inactive slot" on the Pico side in this protocol at all (no
    // A/B image slots the way the ESP has). What IS actually available is
    // exposed here: the peer's protocol version, whether it's known/
    // compatible with this ESP's build, and its boot_id -- via
    // safety_link_get_peer_version_status(), the same accessor
    // dashboard_http.c's peer_protocol_version fields already use.
    bool version_known = false, version_compatible = false;
    uint16_t peer_protocol = 0, peer_min_compatible = 0;
    if (s_safety) {
        (void)safety_link_get_peer_version_status(s_safety, &version_known, &version_compatible,
                                                    &peer_protocol, &peer_min_compatible);
    }

    // last_error is always built by this codebase's own snprintf() calls
    // (ota_pico_relay.c's relay_set_error()/format_update_error()) -- never
    // copied verbatim from an external source -- so it cannot contain a
    // raw '"' or '\' that would need JSON escaping here.
    char body[384];
    int n;
    if (version_known) {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,\"last_error\":\"%s\","
                      "\"protocol_version_known\":true,\"protocol_version\":%u,"
                      "\"protocol_min_compatible\":%u,\"protocol_compatible\":%s}",
                      ota_pico_relay_phase_str(st.phase), (unsigned)st.percent, st.last_error,
                      (unsigned)peer_protocol, (unsigned)peer_min_compatible,
                      version_compatible ? "true" : "false");
    } else {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,\"last_error\":\"%s\","
                      "\"protocol_version_known\":false}",
                      ota_pico_relay_phase_str(st.phase), (unsigned)st.percent, st.last_error);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

esp_err_t ota_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                          SafetyLinkClass *safety_or_null)
{
    s_io = io_or_null;
    s_thermo_bus = thermo_bus_or_null;
    s_safety = safety_or_null;

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
    memset(&s_lockout_esp_rollback, 0, sizeof(s_lockout_esp_rollback));
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/ota) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t challenge_uri = {
        .uri = "/api/ota/challenge", .method = HTTP_GET, .handler = ota_challenge_get_handler
    };
    err = httpd_register_uri_handler(server, &challenge_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/challenge) failed: %s",
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/esp) failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/pico) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t pico_status_uri = {
        .uri = "/api/ota/pico/status", .method = HTTP_GET, .handler = ota_pico_status_get_handler
    };
    err = httpd_register_uri_handler(server, &pico_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/pico/status) failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/interlock) failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/esp/status) failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/ota/esp/rollback) failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
