#include "profiles_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "MAX31856.h"
#include "http_form.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "profiles_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_USED "prof_used"
/* "prof0".."prof7" -- see profile_nvs_key() below. */

/* PROFILES_MAX_COUNT / PROFILE_NAME_MAX_LEN / PROFILE_MAX_SEGMENTS and the
 * profile_t/profile_segment_t layout now live in profiles_http.h --
 * profile_executor.c needs them too (via profiles_http_get()). */

/* Firmware sanity bounds, not real kiln-safety limits -- there is no
 * separate safety authority for firing profiles (TODO.md section 6, which
 * would run one, is unbuilt), so these exist only to reject obvious
 * garbage/typos before anything is stored. 1400C is comfortably above any
 * home-kiln cone this board's use case implies; a real ceramics kiln safety
 * limit would come from the kiln's own manufacturer data, not this file. */
#define PROFILE_TARGET_C_MIN 0.0f
#define PROFILE_TARGET_C_MAX 1400.0f
#define PROFILE_RAMP_C_PER_HR_MIN 0.0f
#define PROFILE_RAMP_C_PER_HR_MAX 1000.0f
#define PROFILE_DWELL_MIN_MAX 1440u /* 24h */

/* The 20%-margin warning rule, explicit in TODO.md section 5. */
#define PROFILE_RAMP_WARN_FRACTION 0.8f

extern const uint8_t profiles_page_html_start[] asm("_binary_profiles_page_html_start");
extern const uint8_t profiles_page_html_end[] asm("_binary_profiles_page_html_end");

/* All 8 slots kept resident -- each is well under 200 bytes, so loading all
 * 8 at boot (rather than lazily per-request) is simpler and cheap enough
 * that the "only load what's used" optimization the header docstring
 * mentions as a design choice isn't worth the extra code path. The
 * prof_used bitmap still exists in NVS/RAM so a listing never has to probe
 * 8 keys to find out which exist. */
static struct {
    profile_t profiles[PROFILES_MAX_COUNT];
    uint8_t used_bitmap; /* bit N = slot N in use */
} s_profiles;

/* application/x-www-form-urlencoded whole-profile submit: id, name, zone,
 * seg_count, plus 3 fields per segment across up to 12 segments. Generous
 * headroom over a legitimate 12-segment submission, checked against
 * Content-Length before a single byte is read. */
#define PROFILE_BODY_MAX 2048

static void profile_nvs_key(uint8_t id, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "prof%u", id);
}

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_load_all(void)
{
    memset(&s_profiles, 0, sizeof(s_profiles));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* no kiln_cfg namespace yet -- nothing configured */
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t bitmap = 0;
    err = nvs_get_u8(h, NVS_KEY_USED, &bitmap);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    s_profiles.used_bitmap = (err == ESP_OK) ? bitmap : 0;

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(s_profiles.used_bitmap & (1u << id))) {
            continue;
        }
        char key[8];
        profile_nvs_key(id, key, sizeof(key));
        size_t len = sizeof(s_profiles.profiles[id]);
        esp_err_t slot_err = nvs_get_blob(h, key, &s_profiles.profiles[id], &len);
        if (slot_err != ESP_OK || len != sizeof(s_profiles.profiles[id])) {
            /* The bitmap says used but the blob is missing/wrong-size --
             * trust the blob, not the bitmap: mark it unused rather than
             * hand a client a garbage-decoded profile. */
            ESP_LOGW(TAG, "prof%u load failed or wrong size (%s) -- marking unused", id,
                     esp_err_to_name(slot_err));
            s_profiles.used_bitmap &= ~(1u << id);
            memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
        }
    }

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save_slot(uint8_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    err = nvs_set_blob(h, key, &s_profiles.profiles[id], sizeof(s_profiles.profiles[id]));
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_USED, s_profiles.used_bitmap);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_erase_slot(uint8_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    esp_err_t erase_err = nvs_erase_key(h, key);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return erase_err;
    }
    err = nvs_set_u8(h, NVS_KEY_USED, s_profiles.used_bitmap);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- Public getter (profile_executor.c) ----------------------------------- */

bool profiles_http_get(uint8_t id, profile_t *out)
{
    if (!out || id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        return false;
    }
    *out = s_profiles.profiles[id];
    return true;
}

/* ---- HTML page ------------------------------------------------------------ */

static esp_err_t page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)profiles_page_html_start,
                           (size_t)(profiles_page_html_end - profiles_page_html_start));
}

/* ---- JSON ------------------------------------------------------------------ */

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

static esp_err_t profiles_list_get_handler(httpd_req_t *req)
{
    char json[PROFILES_MAX_COUNT * 96 + 16];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    json[o++] = '[';
    bool first = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(s_profiles.used_bitmap & (1u << id))) {
            continue;
        }
        const profile_t *p = &s_profiles.profiles[id];
        char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
        json_escape(p->name, name_escaped, sizeof(name_escaped));
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u}", first ? "" : ",", id,
               name_escaped, p->zone_mask, p->segment_count);
        first = false;
    }
    APPEND("]");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

static esp_err_t profile_detail_get_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char *end = NULL;
    long id = strtol(id_str, &end, 10);
    if (end == id_str || id < 0 || id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    const profile_t *p = &s_profiles.profiles[id];
    char json[128 + PROFILE_MAX_SEGMENTS * 64];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
    json_escape(p->name, name_escaped, sizeof(name_escaped));
    APPEND("{\"id\":%ld,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u,\"segments\":[", id,
           name_escaped, p->zone_mask, p->segment_count);
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *s = &p->segments[i];
        APPEND("%s{\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu}", i == 0 ? "" : ",",
               (double)s->target_c, (double)s->ramp_c_per_hr, (unsigned long)s->dwell_min);
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* ---- POST /api/profile ----------------------------------------------------
 * Validates into a scratch profile_t before touching s_profiles/NVS. Runs
 * the TODO.md section 5 feasibility check against zones_http.c's
 * user-entered per-zone ramp ceiling (zones_config_get_max_ramp()): a
 * segment whose ramp rate exceeds the ceiling rejects the whole submission;
 * one within 20% of it (PROFILE_RAMP_WARN_FRACTION) is accepted with a
 * warning, per TODO.md's explicit "warn, don't block" rule at that margin.
 * This is the creation-time half of that check -- TODO.md also calls for
 * re-checking at profile-*start* time, which belongs to the profile
 * executor, not this page, and profile_executor.c does exactly that against
 * every participating zone's ceiling as it stands at start. */

static bool parse_profile_fields(const char *body, profile_t *p, char *err_msg, size_t err_cap)
{
    char name[PROFILE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, "name", name, sizeof(name));
    if (name_len == -2) {
        snprintf(err_msg, err_cap, "name too long");
        return false;
    }
    if (name_len <= 0) {
        snprintf(err_msg, err_cap, "name missing");
        return false;
    }
    strncpy(p->name, name, PROFILE_NAME_MAX_LEN);
    p->name[PROFILE_NAME_MAX_LEN] = '\0';

    char zone_val[8];
    int zone_len = http_form_find_field(body, "zone_mask", zone_val, sizeof(zone_val));
    if (zone_len <= 0) {
        snprintf(err_msg, err_cap, "zone_mask missing");
        return false;
    }
    char *end = NULL;
    long zone_mask = strtol(zone_val, &end, 10);
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_bits = thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
    if (end == zone_val || zone_mask <= 0 || zone_mask > 0xFF || ((uint8_t)zone_mask & ~valid_bits) != 0) {
        snprintf(err_msg, err_cap,
                "zone_mask must select at least one configured zone (check Thermocouples & Zones settings)");
        return false;
    }
    p->zone_mask = (uint8_t)zone_mask;

    char seg_count_val[8];
    int seg_count_len = http_form_find_field(body, "seg_count", seg_count_val, sizeof(seg_count_val));
    if (seg_count_len <= 0) {
        snprintf(err_msg, err_cap, "seg_count missing");
        return false;
    }
    end = NULL;
    long seg_count = strtol(seg_count_val, &end, 10);
    if (end == seg_count_val || seg_count < 1 || seg_count > PROFILE_MAX_SEGMENTS) {
        snprintf(err_msg, err_cap, "seg_count out of range (1-12)");
        return false;
    }
    p->segment_count = (uint8_t)seg_count;

    for (uint8_t i = 0; i < p->segment_count; i++) {
        char key[16];
        profile_segment_t *seg = &p->segments[i];

        snprintf(key, sizeof(key), "seg%u_target", i);
        char val[24];
        int len = http_form_find_field(body, key, val, sizeof(val));
        char *fend = NULL;
        float target = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(target) || target < PROFILE_TARGET_C_MIN ||
            target > PROFILE_TARGET_C_MAX) {
            snprintf(err_msg, err_cap, "segment %u: target_c missing or out of range (0-1400)", i + 1);
            return false;
        }
        seg->target_c = target;

        snprintf(key, sizeof(key), "seg%u_ramp", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float ramp = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(ramp) || ramp < PROFILE_RAMP_C_PER_HR_MIN ||
            ramp > PROFILE_RAMP_C_PER_HR_MAX) {
            snprintf(err_msg, err_cap, "segment %u: ramp_c_per_hr missing or out of range (0-1000)", i + 1);
            return false;
        }
        seg->ramp_c_per_hr = ramp;

        snprintf(key, sizeof(key), "seg%u_dwell", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        end = NULL;
        long dwell = len > 0 ? strtol(val, &end, 10) : -1;
        if (len <= 0 || end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX) {
            snprintf(err_msg, err_cap, "segment %u: dwell_min missing or out of range (0-1440)", i + 1);
            return false;
        }
        seg->dwell_min = (uint32_t)dwell;
    }
    return true;
}

/* Appends a JSON string element for warnings[]; returns false (and leaves
 * *o unchanged) if it wouldn't fit, matching every other APPEND-macro
 * handler's "stop rather than overrun" convention. */
static bool append_warning(char *json, size_t cap, size_t *o, bool *first, const char *text)
{
    int n = snprintf(json + *o, cap - *o, "%s\"%s\"", *first ? "" : ",", text);
    if (n < 0 || (size_t)n >= cap - *o) {
        return false;
    }
    *o += (size_t)n;
    *first = false;
    return true;
}

static esp_err_t profile_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PROFILE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[PROFILE_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "profile body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* id: empty or "-1" creates in the first free slot; a valid existing id
     * overwrites that slot. Any other value in 0..7 also targets that exact
     * slot (create-or-overwrite), so a client that already knows its id can
     * address it directly rather than relying on "first free". */
    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long requested_id = (id_len > 0) ? strtol(id_val, NULL, 10) : -1;

    uint8_t target_id;
    if (requested_id >= 0 && requested_id < PROFILES_MAX_COUNT) {
        target_id = (uint8_t)requested_id;
    } else {
        int free_slot = -1;
        for (uint8_t i = 0; i < PROFILES_MAX_COUNT; i++) {
            if (!(s_profiles.used_bitmap & (1u << i))) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile storage full");
            return ESP_OK;
        }
        target_id = (uint8_t)free_slot;
    }

    profile_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    char err_msg[128];
    if (!parse_profile_fields(body, &tmp, err_msg, sizeof(err_msg))) {
        char json[192];
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    /* Feasibility check (TODO.md section 5): a segment with ramp_c_per_hr ==
     * 0 has no ramp-rate constraint at all (dwell/hold segment) and is
     * exempt. For a segment that does specify a rate, no ceiling on record
     * for the zone (zones_config_get_max_ramp returning a ceiling of 0.0,
     * which is also its "never configured" default) makes every nonzero
     * rate infeasible -- correct, since there is nothing to feasibility
     * check against until the zone's max ramp rate is set on the
     * Thermocouples & Zones page. */
    char warn_json[PROFILE_MAX_SEGMENTS * 96 + 16];
    size_t warn_o = 0;
    bool warn_first = true;
    warn_json[warn_o++] = '[';
    /* Multi-zone (TODO.md 6A.5): check every participating zone's ceiling
     * against every ramped segment -- a profile is only feasible if ALL of
     * its zones can sustain the requested rate, since ramp-lock will hold
     * the shared setpoint back to whichever zone is slowest anyway; a
     * profile that's infeasible for even one zone would just always be
     * ramp-locked against that zone forever. */
    for (uint8_t i = 0; i < tmp.segment_count; i++) {
        float rate = tmp.segments[i].ramp_c_per_hr;
        if (rate <= 0.0f) {
            continue;
        }
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(tmp.zone_mask & (1u << zi))) {
                continue;
            }
            float ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ceiling); /* zone already validated < thermo_count */
            if (rate > ceiling) {
                char json[224];
                int n = snprintf(json, sizeof(json),
                                 "{\"ok\":false,\"error\":\"segment %u: ramp rate %.1f C/hr exceeds zone %u's "
                                 "%.1f C/hr ceiling\"}",
                                 i + 1, (double)rate, zi, (double)ceiling);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
            }
            if (rate > PROFILE_RAMP_WARN_FRACTION * ceiling) {
                char text[96];
                snprintf(text, sizeof(text),
                        "segment %u: ramp rate %.1f C/hr is within 20%% of zone %u's %.1f C/hr ceiling",
                        i + 1, (double)rate, zi, (double)ceiling);
                append_warning(warn_json, sizeof(warn_json), &warn_o, &warn_first, text);
            }
        }
    }
    if (warn_o + 1 < sizeof(warn_json)) {
        warn_json[warn_o++] = ']';
    }
    warn_json[warn_o < sizeof(warn_json) ? warn_o : sizeof(warn_json) - 1] = '\0';

    s_profiles.profiles[target_id] = tmp;
    s_profiles.used_bitmap |= (1u << target_id);
    esp_err_t err = nvs_save_slot(target_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
                 target_id, esp_err_to_name(err));
    }

    char json[256 + sizeof(warn_json)];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u,\"warnings\":%s}", target_id, warn_json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

static esp_err_t profile_delete_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[65];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "profile delete body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    char *end = NULL;
    long id = (id_len > 0) ? strtol(id_val, &end, 10) : -1;
    if (id_len <= 0 || end == id_val || id < 0 || id >= PROFILES_MAX_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    if (!(s_profiles.used_bitmap & (1u << id))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    s_profiles.used_bitmap &= ~(1u << id);
    memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
    esp_err_t err = nvs_erase_slot((uint8_t)id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_slot(%ld) failed: %s -- deleted live but may reappear after reboot", id,
                 esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t profiles_http_start(void)
{
    esp_err_t err = nvs_load_all();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "profile NVS load failed: %s -- starting with no saved profiles", esp_err_to_name(err));
        memset(&s_profiles, 0, sizeof(s_profiles));
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/profiles", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t list_uri = {
        .uri = "/api/profiles", .method = HTTP_GET, .handler = profiles_list_get_handler,
    };
    static const httpd_uri_t detail_uri = {
        .uri = "/api/profile", .method = HTTP_GET, .handler = profile_detail_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/profile", .method = HTTP_POST, .handler = profile_post_handler,
    };
    static const httpd_uri_t delete_uri = {
        .uri = "/api/profile/delete", .method = HTTP_POST, .handler = profile_delete_post_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &detail_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &delete_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile/delete) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "profiles API up (storage/validation; execution runs in profile_executor.c)");
    return ESP_OK;
}
