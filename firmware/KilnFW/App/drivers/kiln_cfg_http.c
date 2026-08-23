#include "kiln_cfg_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_form.h"
#include "kiln_cfg_store.h"
#include "ota_http.h"
#include "wifi_provision_http.h"

static const char *TAG = "kiln_cfg_http";

/* Small bodies only -- id=<int>[&name=<str up to KILN_CFG_NAME_MAX_LEN>].
 * Generous headroom over what a legitimate request needs, same discipline
 * as every other small-form handler in this codebase (profiles_http.c's
 * profile_delete_post_handler() etc.), checked against Content-Length before
 * a single byte is read. */
#define KILN_CFG_BODY_MAX 128

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return true;
}

/* Same escaping convention as zones_http.c's/wifi_provision_http.c's
 * json_escape -- a config name came from a POST body at some point. */
static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* Parses a required "id=<int>" form field, 0..INT32_MAX. Returns false (body
 * untouched by the caller) if missing or malformed. */
static bool parse_required_id(const char *body, const char *key, int32_t *out_id)
{
    char val[16];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < 0 || v > INT32_MAX) {
        return false;
    }
    *out_id = (int32_t)v;
    return true;
}

/* Parses a required "name=<str>" form field into out (out_cap >=
 * KILN_CFG_NAME_MAX_LEN + 1). Returns false if missing or too long to decode
 * (http_form_find_field()'s -2). Does NOT itself enforce KILN_CFG_NAME_MAX_LEN
 * against the DECODED length -- kiln_cfg_store.c's name_is_valid() is the one
 * place that bound is checked, so there is exactly one definition of "too
 * long" rather than two that could drift. */
static bool parse_required_name(const char *body, char *out, size_t out_cap)
{
    int len = http_form_find_field(body, "name", out, out_cap);
    return len > 0;
}

/* ---- GET /api/kiln_configs -------------------------------------------------
 * {"active_id":<int|null>,"configs":[{"id":N,"name":"...","is_active":bool},
 * ...],"max_count":N} */
static esp_err_t list_get_handler(httpd_req_t *req)
{
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t active_id = kiln_cfg_store_get_active_id();

    /* KILN_CFG_MAX_COUNT rows * (~60 bytes/row headroom for id+escaped name)
     * plus a small fixed header -- generous over what 8 rows of a <=23-char
     * name could ever need. */
    char json[KILN_CFG_MAX_COUNT * 96 + 96];
    size_t o = 0;
    int written;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        written = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                             \
        if (written < 0 || (size_t)written >= sizeof(json) - o) {                                \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)written;                                                                      \
    } while (0)

    if (active_id == KILN_CFG_NO_ACTIVE_ID) {
        APPEND("{\"active_id\":null,\"configs\":[");
    } else {
        APPEND("{\"active_id\":%ld,\"configs\":[", (long)active_id);
    }
    for (uint8_t i = 0; i < n; i++) {
        char name_escaped[KILN_CFG_NAME_MAX_LEN * 2 + 1];
        json_escape(rows[i].name, name_escaped, sizeof(name_escaped));
        APPEND("%s{\"id\":%ld,\"name\":\"%s\",\"is_active\":%s}", i == 0 ? "" : ",", (long)rows[i].id,
               name_escaped, rows[i].is_active ? "true" : "false");
    }
    APPEND("],\"max_count\":%u}", (unsigned)kiln_cfg_store_max_count());

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* ---- POST /api/kiln_configs/save -- name=<str> [id=<int>] ----------------- */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }
    int32_t id_or_negative = -1;
    {
        char id_val[16];
        int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
        if (id_len > 0) {
            char *end = NULL;
            long v = strtol(id_val, &end, 10);
            if (end == id_val || *end != '\0' || v < 0 || v > INT32_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id out of range");
                return ESP_OK;
            }
            id_or_negative = (int32_t)v;
        }
    }

    int32_t new_id = -1;
    char reason[96];
    reason[0] = '\0';
    if (!kiln_cfg_store_save_current(name, id_or_negative, &new_id, reason, sizeof(reason))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason[0] ? reason : "save failed");
        return ESP_OK;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"id\":%ld}", (long)new_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- POST /api/kiln_configs/clone -- id=<int>, name=<str> ----------------- */
static esp_err_t clone_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    int32_t src_id;
    if (!parse_required_id(body, "id", &src_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }

    int32_t new_id = -1;
    char reason[96];
    reason[0] = '\0';
    if (!kiln_cfg_store_clone(src_id, name, &new_id, reason, sizeof(reason))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason[0] ? reason : "clone failed");
        return ESP_OK;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"id\":%ld}", (long)new_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- POST /api/kiln_configs/apply -- id=<int> ------------------------------
 *
 * The one route in this file that can rewrite a running kiln's relay/
 * thermocouple/PID/guard configuration. kiln_cfg_store_apply() itself now
 * enforces ota_http_check_interlocks() internally (the backstop -- see
 * kiln_cfg_store.h's SAFETY note), so the pre-check here is redundant
 * belt-and-suspenders, kept ONLY because it lets this handler return a
 * clean 409 with the refusal reason before ever calling into the store --
 * removing it would not reopen the hole, it would just make a refused
 * request's error path one function call longer. */
static esp_err_t apply_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }

    /* Existence BEFORE the interlock. An id that does not exist is a 404 no
     * matter what the kiln is doing, and answering "safety link is down"
     * for `id=999` sent the operator to diagnose a link that was not the
     * problem -- the same misdirection the delete route already avoids by
     * checking first. kiln_cfg_store_get_name() is the cheapest existence
     * probe the store exposes (false = no such id). */
    char exists_name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!kiln_cfg_store_get_name(id, exists_name, sizeof(exists_name))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such kiln config");
        return ESP_OK;
    }

    /* Applying a saved configuration writes zone/guard settings; it streams
     * nothing over the safety link. So "the safety link is down" is
     * overridable here with the operator's per-request acknowledgement (see
     * ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK), while a running
     * firing or a hot zone still refuses outright. kiln_cfg_store_apply()'s
     * own internal backstop is passed the same answer, below. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    reason[0] = '\0';
    const bool ack = ota_http_req_ack_no_safety(req);
    ota_interlock_result_t gate = ota_http_check_interlocks(ack, reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "kiln config apply id=%ld refused by interlock: %s", (long)id, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    char apply_reason[96];
    apply_reason[0] = '\0';
    if (!kiln_cfg_store_apply(id, ack, apply_reason, sizeof(apply_reason))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, apply_reason[0] ? apply_reason : "apply failed");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- POST /api/kiln_configs/delete -- id=<int> ---------------------------- */
static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    if (!kiln_cfg_store_delete(id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such kiln config");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- POST /api/kiln_configs/rename -- id=<int>, name=<str> ---------------- */
static esp_err_t rename_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }
    /* Pre-check purely for a specific message -- same "pre-check for a
     * better-worded error, backed by the store's own authoritative check"
     * pattern the apply handler uses for the interlock. `id` is excluded so
     * a no-op rename (or a rename that only changes case/whitespace of the
     * name this entry already has) is not reported as a collision.
     * kiln_cfg_store_rename() below enforces the same rule itself -- this
     * cannot be bypassed by this pre-check being wrong or skipped. */
    if (kiln_cfg_store_name_would_collide(name, id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a saved kiln config already has that name");
        return ESP_OK;
    }
    if (!kiln_cfg_store_rename(id, name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such kiln config, or name invalid");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t kiln_cfg_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t list_uri = {
        .uri = "/api/kiln_configs", .method = HTTP_GET, .handler = list_get_handler,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/api/kiln_configs/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    static const httpd_uri_t clone_uri = {
        .uri = "/api/kiln_configs/clone", .method = HTTP_POST, .handler = clone_post_handler,
    };
    static const httpd_uri_t apply_uri = {
        .uri = "/api/kiln_configs/apply", .method = HTTP_POST, .handler = apply_post_handler,
    };
    static const httpd_uri_t delete_uri = {
        .uri = "/api/kiln_configs/delete", .method = HTTP_POST, .handler = delete_post_handler,
    };
    static const httpd_uri_t rename_uri = {
        .uri = "/api/kiln_configs/rename", .method = HTTP_POST, .handler = rename_post_handler,
    };

    const httpd_uri_t *uris[] = { &list_uri, &save_uri, &clone_uri, &apply_uri, &delete_uri, &rename_uri };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s", uris[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "kiln config API up (max_count=%u)", (unsigned)kiln_cfg_store_max_count());
    return ESP_OK;
}
