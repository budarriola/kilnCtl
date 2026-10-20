// update_install_http.c -- see update_install_http.h.
#include "update_install_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_kv.h"
#include "http_auth_http.h" /* kiln_http_register() */
#include "http_form.h"
#include "ota_http_internal.h"
#include "update_http_internal.h"
#include "update_install_record.h"
#include "update_policy.h"
#include "update_stage.h"
#include "wifi_provision_http.h"

static const char *TAG = "update_install_http";

#define INSTALL_BODY_MAX 64
#define KILN_NVS_PARTITION_NAME "kiln_nvs"

// ---- raw NVS access to the install record (kiln_nvs / upd_inst) ----

// 0 = key gone (read back), nonzero = failure.
static int kv_erase_verified(const char *key)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, UPD_INST_NS, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION_NAME) != HAL_OK) {
        return -1;
    }
    hal_status_t e = hal_kv_erase_key(&h, key);
    int rc = (e == HAL_OK || e == HAL_NOT_FOUND) ? 0 : -1;
    if (rc == 0 && hal_kv_commit(&h) != HAL_OK) {
        rc = -1;
    }
    if (rc == 0 && hal_kv_key_exists(&h, key) != HAL_NOT_FOUND) {
        rc = -1;
    }
    hal_kv_close(&h);
    return rc;
}

static int kv_write_verified(const char *key, const uint8_t *buf, size_t len)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, UPD_INST_NS, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION_NAME) != HAL_OK) {
        return -1;
    }
    int rc = -1;
    if (hal_kv_set_blob(&h, key, buf, len) == HAL_OK && hal_kv_commit(&h) == HAL_OK) {
        uint8_t back[UPD_INST_REQ_SIZE];
        size_t n = sizeof(back);
        if (len <= sizeof(back) && hal_kv_get_blob(&h, key, back, &n) == HAL_OK && n == len &&
            memcmp(back, buf, len) == 0) {
            rc = 0;
        }
    }
    hal_kv_close(&h);
    return rc;
}

// 1 = present and valid, 0 = absent, -1 = unreadable or corrupt.
static int read_result(update_install_res_t *out)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, UPD_INST_NS, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION_NAME) != HAL_OK) {
        return 0; // namespace never created: no install was ever requested
    }
    uint8_t buf[UPD_INST_RES_SIZE];
    size_t n = sizeof(buf);
    hal_status_t e = hal_kv_get_blob(&h, UPD_INST_KEY_RES, buf, &n);
    hal_kv_close(&h);
    if (e == HAL_NOT_FOUND) {
        return 0;
    }
    if (e != HAL_OK) {
        return -1;
    }
    return update_install_res_decode(buf, n, out) ? 1 : -1;
}

// ---- prepare / undo hooks for ota_recovery_boot_run ----

static bool app_kv_safe(void *ctx)
{
    (void)ctx;
    return hal_kv_write_safe_here();
}

static bool app_stage_info(void *ctx, bool *busy, uint32_t *len, uint8_t sha[UPD_INST_SHA_LEN],
                           char semver[UPD_INST_SEMVER_LEN])
{
    (void)ctx;
    size_t cap = 0;
    uint8_t *buf = ota_http_esp_chunk_buf(&cap);
    update_stage_info_t info = { 0 };
    update_stage_err_t e = update_stage_get_status(update_http_stage(), buf, cap, &info);
    *busy = (e == UPDATE_STAGE_ERR_BUSY) || info.busy;
    if (e != UPDATE_STAGE_OK || !info.staged || info.hdr_status != STAGE_HDR_OK ||
        info.state != STAGE_STATE_VERIFIED) {
        return false;
    }
    _Static_assert(STAGE_SHA256_LEN == UPD_INST_SHA_LEN, "sha length");
    *len = info.image_length;
    memcpy(sha, info.sha256, UPD_INST_SHA_LEN);
    memset(semver, 0, UPD_INST_SEMVER_LEN);
    strncpy(semver, info.semver, UPD_INST_SEMVER_LEN - 1);
    return true;
}

static int app_res_erase(void *ctx)
{
    (void)ctx;
    return kv_erase_verified(UPD_INST_KEY_RES);
}

static int app_req_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return kv_write_verified(UPD_INST_KEY_REQ, buf, len);
}

static bool install_prepare(char *msg, size_t cap, int *http_status)
{
    static const update_install_app_ops_t ops = {
        .ctx = NULL,
        .kv_write_safe = app_kv_safe,
        .stage_info = app_stage_info,
        .res_erase = app_res_erase,
        .req_write = app_req_write,
    };
    bool ok = update_install_prepare(&ops, msg, cap, http_status);
    if (!ok && http_status != NULL && *http_status == 500) {
        // ota_recovery_boot_run does not call undo when prepare fails: a half-written
        // request must not survive the refusal.
        (void)kv_erase_verified(UPD_INST_KEY_REQ);
    }
    return ok;
}

static void install_undo(void)
{
    if (kv_erase_verified(UPD_INST_KEY_REQ) != 0) {
        ESP_LOGE(TAG, "could not remove the install request after a failed recovery boot select");
    }
}

// OA-03 glue (only the recovery-side ops the recorded drop uses; the rest stay NULL).
static int stale_req_read(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    hal_kv_handle_t h;
    if (hal_kv_open(&h, UPD_INST_NS, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION_NAME) != HAL_OK) {
        return 1; // namespace never created: no request
    }
    size_t n = cap;
    hal_status_t e = hal_kv_get_blob(&h, UPD_INST_KEY_REQ, buf, &n);
    hal_kv_close(&h);
    if (e == HAL_NOT_FOUND) {
        return 1;
    }
    if (e != HAL_OK) {
        return -1;
    }
    *len = n;
    return 0;
}

static int stale_req_clear(void *ctx)
{
    (void)ctx;
    return kv_erase_verified(UPD_INST_KEY_REQ);
}

static int stale_res_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return kv_write_verified(UPD_INST_KEY_RES, buf, len);
}

static int stale_res_read(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    hal_kv_handle_t h;
    if (hal_kv_open(&h, UPD_INST_NS, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION_NAME) != HAL_OK) {
        return 1;
    }
    size_t n = cap;
    hal_status_t e = hal_kv_get_blob(&h, UPD_INST_KEY_RES, buf, &n);
    hal_kv_close(&h);
    if (e == HAL_NOT_FOUND) {
        return 1;
    }
    if (e != HAL_OK) {
        return -1;
    }
    *len = n;
    return 0;
}

// True only when the first req->image_length bytes of the RUNNING app partition hash to req->sha256
// (the same bytes recovery copied and re-hashed). Called under the update claim, so the shared chunk
// buffer is free. Any read/hash trouble answers false (recorded as outcome_unknown).
static bool stale_running_matches(void *ctx, const update_install_req_t *req)
{
    (void)ctx;
    const esp_partition_t *app = esp_ota_get_running_partition();
    size_t cap = 0;
    uint8_t *buf = ota_http_esp_chunk_buf(&cap);
    if (app == NULL || buf == NULL || cap == 0 || req->image_length > app->size) {
        return false;
    }
    psa_hash_operation_t sha = psa_hash_operation_init();
    if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    for (uint32_t off = 0; off < req->image_length;) {
        size_t n = req->image_length - off < cap ? (size_t)(req->image_length - off) : cap;
        if (esp_partition_read(app, off, buf, n) != ESP_OK || psa_hash_update(&sha, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&sha);
            return false;
        }
        off += (uint32_t)n;
        vTaskDelay(1);
    }
    uint8_t out[UPD_INST_SHA_LEN];
    size_t outn = 0;
    if (psa_hash_finish(&sha, out, sizeof(out), &outn) != PSA_SUCCESS || outn != sizeof(out)) {
        return false;
    }
    return memcmp(out, req->sha256, sizeof(out)) == 0;
}

int update_install_http_drop_stale_request(void)
{
    bool claimed = ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP);
    update_install_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.req_read = stale_req_read;
    ops.req_clear = stale_req_clear;
    ops.res_write = stale_res_write;
    ops.res_read = stale_res_read;
    int rc = update_install_drop_stale_recorded(claimed, &ops, stale_running_matches, NULL);
    if (claimed) {
        ota_http_update_end();
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "stale install request not dropped (rc=%d)", rc);
    }
    return rc;
}

// OH-04: boot confirm completed on an OTA slot: an unconfirmed OK result becomes confirmed. Never fails the boot.
int update_install_http_mark_confirmed(void)
{
    bool claimed = ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP);
    int rc = 1;
    if (claimed) {
        update_install_ops_t ops;
        memset(&ops, 0, sizeof(ops));
        ops.res_write = stale_res_write;
        ops.res_read = stale_res_read;
        rc = update_install_confirm_result(&ops);
        ota_http_update_end();
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "install result not marked confirmed (rc=%d)", rc);
    }
    return rc;
}

static esp_err_t install_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > INSTALL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[INSTALL_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';
    char val[4];
    int n = http_form_find_field(body, "confirm", val, sizeof(val));
    if (n < 0 || strcmp(val, "1") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm=1 required");
        return ESP_OK;
    }
    static const ota_recovery_boot_hooks_t hooks = { .prepare = install_prepare, .undo = install_undo };
    return ota_recovery_boot_run(req, &hooks);
}

// Names and validated semver only; still strip anything that could break the JSON.
static void json_safe(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '?' : (char)c;
    }
    dst[i] = '\0';
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    update_identity_t run;
    memset(&run, 0, sizeof(run));
    update_fetch_running_identity(&run, NULL);

    bool busy = false;
    uint32_t len = 0;
    uint8_t sha[UPD_INST_SHA_LEN];
    char staged_ver[UPD_INST_SEMVER_LEN];
    bool staged = app_stage_info(NULL, &busy, &len, sha, staged_ver);
    if (!staged) {
        staged_ver[0] = '\0';
    }

    update_install_res_t res;
    memset(&res, 0, sizeof(res));
    int have = read_result(&res);

    char rv[UPDATE_VERSION_STR_MAX], sv[UPD_INST_SEMVER_LEN], lv[UPD_INST_SEMVER_LEN];
    char lid[UPD_INST_ID_HEX_LEN];
    update_install_id_hex(res.id, lid); /* res is zeroed when nothing is recorded: "00000000" */
    json_safe(rv, sizeof(rv), run.version);
    json_safe(sv, sizeof(sv), staged_ver);
    json_safe(lv, sizeof(lv), have == 1 ? res.semver : "");

    char json[512];
    int w = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"running_version\":\"%s\",\"stage_busy\":%s,\"staged\":%s,"
                     "\"staged_version\":\"%s\",\"last_install\":{\"state\":\"%s\",\"code\":\"%s\","
                     "\"reason\":\"%s\",\"version\":\"%s\",\"id\":\"%s\","
                     "\"detail\":\"%s\",\"app_modified\":%s}}",
                     rv, busy ? "true" : "false", staged ? "true" : "false", sv,
                     have == 1 ? update_install_res_state_name(&res) : (have == 0 ? "none" : "unreadable"),
                     have == 1 ? update_install_code_name(res.code) : "none",
                     have == 1 ? update_install_reason_name(res.reason) : "none", lv, lid,
                     have == 1 ? update_install_detail_name(res.detail) : "none",
                     (have == 1 && res.app_modified) ? "true" : "false");
    if (w < 0 || (size_t)w >= sizeof(json)) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (ssize_t)w);
}

esp_err_t update_install_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    static const httpd_uri_t post_uri = {
        .uri = "/api/update/install", .method = HTTP_POST, .handler = install_post_handler,
    };
    static const httpd_uri_t get_uri = {
        .uri = "/api/update/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    const httpd_uri_t *routes[] = { &post_uri, &get_uri };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = kiln_http_register(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
