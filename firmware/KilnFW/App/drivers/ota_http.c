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

#include <math.h>

#include "autotune_engine.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

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
    s_update_claim = OTA_UPDATE_NONE;

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
