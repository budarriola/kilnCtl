#include "profiles_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "http_form.h"
#include "profiles_builtin.h"
#include "zones_config_accessors.h"


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
        /* 24, not 16. The longest key built here is "seg%u_io_blocking", and
         * at the last segment index that is "seg11_io_blocking" -- 17
         * characters plus the terminator, which does not fit 16. The MSVC
         * host build does not run -Wformat-truncation, so this compiled and
         * passed every host test; only the target build (-Werror=format-
         * truncation) caught it. A truncated key would not have failed
         * loudly either: http_form_find_field() would simply not find
         * "seg11_io_blockin", and the field would silently read as absent,
         * taking its default. 2026-08-28. */
        char key[24];
        profile_segment_t *seg = &p->segments[i];
        memset(seg, 0, sizeof(*seg));

        /* seg%u_kind is OPTIONAL and defaults to PROFILE_SEG_KIND_ZONE_RAMP
         * (0) when absent -- every existing caller of this endpoint (the
         * profiles_page.html editor as it stands today, and any UART/scripted
         * submission written before this pass) never sends it and must keep
         * producing exactly the temperature-ramp segment it always has. */
        snprintf(key, sizeof(key), "seg%u_kind", i);
        char val[24];
        int len = http_form_find_field(body, key, val, sizeof(val));
        char *fend = NULL;
        long kind = len > 0 ? strtol(val, &fend, 10) : PROFILE_SEG_KIND_ZONE_RAMP;
        if (len > 0 && fend == val) {
            kind = PROFILE_SEG_KIND_ZONE_RAMP;
        }
        if (kind != PROFILE_SEG_KIND_ZONE_RAMP && kind != PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(err_msg, err_cap, "segment %u: unknown segment kind %ld", i + 1, kind);
            return false;
        }
        seg->seg_kind = (uint8_t)kind;

        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(key, sizeof(key), "seg%u_io_target", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long io_target = len > 0 ? strtol(val, &end, 10) : -1;
            if (len <= 0 || end == val || io_target < 0 || io_target > 255) {
                snprintf(err_msg, err_cap, "segment %u: io_target missing or out of range", i + 1);
                return false;
            }
            seg->io_target = (uint8_t)io_target;

            snprintf(key, sizeof(key), "seg%u_io_state", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            seg->io_state = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_blocking", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Missing defaults to BLOCKING (1) -- the safer of the two: a
             * segment nobody said was non-blocking should still hold up the
             * schedule and get an explicit force-off at its own end, rather
             * than silently running loose in the background. */
            seg->io_blocking = (len <= 0 || val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_leave_on", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Owner's explicit instruction: "Default must be OFF (force it
             * off)". Missing, empty, or "0" all mean off -- only an explicit
             * nonzero value turns this on. */
            seg->io_leave_on_at_end = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_dwell", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long dwell = len > 0 ? strtol(val, &end, 10) : 0; /* missing = 0, same as "no hold" */
            if (len > 0 && (end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX)) {
                snprintf(err_msg, err_cap, "segment %u: dwell_min out of range (0-1440)", i + 1);
                return false;
            }
            seg->dwell_min = (uint32_t)(dwell < 0 ? 0 : dwell);

            if (!validate_io_segment(seg, (uint8_t)(i + 1), err_msg, err_cap)) {
                return false;
            }
            continue;
        }

        snprintf(key, sizeof(key), "seg%u_target", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float target = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(target) || target < PROFILE_TARGET_C_MIN ||
            target > PROFILE_TARGET_C_MAX) {
            snprintf(err_msg, err_cap, "segment %u: target_c missing or out of range (%.0f-%.0f)", i + 1,
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
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

esp_err_t profile_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PROFILE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack, and freed the moment parse_profile_fields()
     * is done with it, BEFORE warn_json below is even allocated -- this used
     * to be the biggest of three buffers (2049B) that all lived on the
     * stack simultaneously for the whole function (body + warn_json[1168] +
     * the final json[1424] = 4641B in one frame, coordinator review,
     * 2026-08-31 httpd_worker stack-overflow audit). `body` is never
     * referenced again after the parse_profile_fields() call a few lines
     * down (the id_val lookup and that one call are its only two uses), so
     * it does not genuinely need to overlap with warn_json/json at all --
     * sequencing it out drops this function's peak transient allocation
     * from 4641B to ~2592B (warn_json+json, which DO need to coexist since
     * the final response embeds warn_json's text via %s). */
    char *body = heap_caps_malloc(PROFILE_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the request body buffer",
                 (unsigned)(PROFILE_BODY_MAX + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(PROFILES_TAG, "profile body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
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
            free(body);
            return ESP_OK;
        }
        target_id = (uint8_t)free_slot;
    }

    profile_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    char err_msg[128];
    bool parse_ok = parse_profile_fields(body, &tmp, err_msg, sizeof(err_msg));
    /* Last use of `body` in this function either way -- free it here, before
     * warn_json is allocated below, rather than holding it until the
     * function returns. */
    free(body);
    body = NULL;
    if (!parse_ok) {
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
     * Thermocouples & Zones page.
     *
     * HEAP (PSRAM): `body` above is already freed by the time this is
     * allocated, so this and the final `json` below (which embeds this
     * buffer's text) are the only two transient buffers actually coexisting
     * in this function -- see this function's own opening comment for the
     * peak-size accounting. Freed on every return path below (both the
     * feasibility-rejection 400 and the final 200). */
    const size_t warn_json_cap = PROFILE_MAX_SEGMENTS * 96 + 16;
    char *warn_json = heap_caps_malloc(warn_json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (warn_json == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the warnings buffer",
                 (unsigned)warn_json_cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
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
        if (tmp.segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue; /* a relay/IO segment has no ramp rate to check against a zone's ceiling */
        }
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
                esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
                free(warn_json);
                return ret;
            }
            if (rate > PROFILE_RAMP_WARN_FRACTION * ceiling) {
                char text[96];
                snprintf(text, sizeof(text),
                        "segment %u: ramp rate %.1f C/hr is within 20%% of zone %u's %.1f C/hr ceiling",
                        i + 1, (double)rate, zi, (double)ceiling);
                append_warning(warn_json, warn_json_cap, &warn_o, &warn_first, text);
            }
        }
    }
    /* OWNER CORRECTION (2026-09-02): a target exceeding the zone's CURRENT
     * max_temp_c is no longer a save-time refusal (profiles are portable
     * between kilns -- see profile_exceeds_zone_ceiling()'s own comment).
     * Surfaced here as a warning instead, so the web editor's response makes
     * the condition visible immediately rather than leaving the user to
     * discover it only when a run is refused hours later. Enforcement stays
     * profile_executor_run.c's run-start re-check. */
    {
        char ceiling_note[256];
        if (profile_exceeds_zone_ceiling(&tmp, ceiling_note, sizeof(ceiling_note))) {
            append_warning(warn_json, warn_json_cap, &warn_o, &warn_first, ceiling_note);
        }
    }
    if (warn_o + 1 < warn_json_cap) {
        warn_json[warn_o++] = ']';
    }
    warn_json[warn_o < warn_json_cap ? warn_o : warn_json_cap - 1] = '\0';

    s_profiles.profiles[target_id] = tmp;
    s_profiles.used_bitmap |= (1u << target_id);
    esp_err_t err = nvs_save_slot(target_id);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
                 target_id, esp_err_to_name(err));
    }

    /* HEAP (PSRAM), same reasoning as warn_json above -- embeds warn_json's
     * text via %s, so the two DO need to coexist for this one snprintf
     * call; warn_json is freed immediately after, before this buffer is
     * sent, rather than both living until the function returns. */
    const size_t json_cap = 256 + warn_json_cap;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the response buffer", (unsigned)json_cap);
        free(warn_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    int n = snprintf(json, json_cap, "{\"ok\":true,\"id\":%u,\"warnings\":%s}", target_id, warn_json);
    free(warn_json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    free(json);
    return ret;
}

esp_err_t profile_delete_post_handler(httpd_req_t *req)
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
            ESP_LOGW(PROFILES_TAG, "profile delete body read failed/short: %d", ret);
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
    if (id_len > 0 && end != id_val && id >= 0 && id <= 255 && profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "built-in schedules are read-only and cannot be deleted -- "
                            "hide it instead (POST /api/profile/builtin/hide)");
        return ESP_OK;
    }
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
        ESP_LOGE(PROFILES_TAG, "nvs_erase_slot(%ld) failed: %s -- deleted live but may reappear after reboot", id,
                 esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- Builtin hide / unhide / restore ---------------------------------------
 *
 * "Remove this shipped schedule" cannot be a delete -- the catalogue is a
 * const table in flash -- so it is a persisted hide, and unhiding is
 * therefore always possible. See profiles_builtin.h.
 *
 * POST /api/profile/builtin/hide     body: id=<128..>&hidden=0|1
 * POST /api/profile/builtin/restore  body: (none) -- unhides everything
 */

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if ((size_t)req->content_len >= cap) {
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

esp_err_t builtin_hide_post_handler(httpd_req_t *req)
{
    char body[65];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    char *end = NULL;
    long id = (id_len > 0) ? strtol(id_val, &end, 10) : -1;
    if (id_len <= 0 || end == id_val || id < 0 || id > 255 || !profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such built-in schedule");
        return ESP_OK;
    }

    /* Missing "hidden" defaults to 1: the endpoint is named "hide", so the
     * request with no qualifier means hide. Unhiding takes an explicit
     * hidden=0. */
    char hid_val[8];
    int hid_len = http_form_find_field(body, "hidden", hid_val, sizeof(hid_val));
    bool hidden = (hid_len <= 0) || (hid_val[0] != '0');

    esp_err_t err = profiles_builtin_set_hidden((uint8_t)id, hidden);
    char json[128];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"id\":%ld,\"hidden\":%s,\"persisted\":%s}",
                     "true", id, hidden ? "true" : "false", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

esp_err_t builtin_restore_post_handler(httpd_req_t *req)
{
    char body[65];
    if (req->content_len > 0 && !read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or read failed");
        return ESP_OK;
    }
    esp_err_t err = profiles_builtin_restore_all();
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"persisted\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

