// update_install_http.c -- see update_install_http.h.
#include "update_install_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

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
    json_safe(rv, sizeof(rv), run.version);
    json_safe(sv, sizeof(sv), staged_ver);
    json_safe(lv, sizeof(lv), have == 1 ? res.semver : "");

    char json[448];
    int w = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"running_version\":\"%s\",\"stage_busy\":%s,\"staged\":%s,"
                     "\"staged_version\":\"%s\",\"last_install\":{\"state\":\"%s\",\"code\":\"%s\","
                     "\"reason\":\"%s\",\"version\":\"%s\"}}",
                     rv, busy ? "true" : "false", staged ? "true" : "false", sv,
                     have == 1 ? "recorded" : (have == 0 ? "none" : "unreadable"),
                     have == 1 ? update_install_code_name(res.code) : "none",
                     have == 1 ? update_install_reason_name(res.reason) : "none", lv);
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
