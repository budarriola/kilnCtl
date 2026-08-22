#include "readiness_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "nvs_report.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "safety_cfg_store.h"
#include "web_encoding.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "readiness_http";

/* Embedded via EMBED_TXTFILES -- same convention as every other
 * *_page.html in this component. TODO.md 10.6a: embedded pre-gzipped
 * (gzip'd at configure time before idf_component_register runs), hence the
 * "_gz" in both the filename and the generated symbol. */
extern const uint8_t readiness_page_html_gz_start[] asm("_binary_readiness_page_html_gz_start");
extern const uint8_t readiness_page_html_gz_end[] asm("_binary_readiness_page_html_gz_end");

/* readiness_status_t (TODO.md 8.3's four required distinctions: "not_done",
 * "cannot_yet" per bullet 2, "deliberately_off" per bullet 3, and "ok") now
 * lives in readiness_http.h so readiness_commissioning_status() below can be
 * host-tested without esp_http_server. readiness_commissioning_status()
 * likewise lives there as a static inline, so the commissioning item's
 * decision can be tested (test_readiness_commissioning.c) without compiling
 * this file, which pulls in httpd and the whole NVS stack. */

static const char *status_name(readiness_status_t s)
{
    switch (s) {
    case READY_OK: return "ok";
    case READY_NOT_DONE: return "not_done";
    case READY_CANNOT_YET: return "cannot_yet";
    case READY_DELIBERATELY_OFF: return "deliberately_off";
    default: return "unknown";
    }
}

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

/* Appends one checklist item object to *o within cap, returning the new
 * offset (unchanged, i.e. truncated, if it would overflow -- same
 * "stop rather than corrupt" convention as every APPEND macro elsewhere in
 * this codebase, just as a plain function since this file builds the array
 * across many small per-item calls rather than one big format string). */
static size_t append_item(char *json, size_t cap, size_t o, bool first, const char *key, const char *label,
                          readiness_status_t status, const char *detail, const char *fix_url)
{
    char detail_esc[192];
    json_escape(detail, detail_esc, sizeof(detail_esc));
    int n = snprintf(json + o, cap - o,
                     "%s{\"key\":\"%s\",\"label\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\","
                     "\"fix_url\":\"%s\"}",
                     first ? "" : ",", key, label, status_name(status), detail_esc, fix_url);
    if (n < 0 || (size_t)n >= cap - o) {
        return o; /* leave o unchanged -- caller's overflow guard */
    }
    return o + (size_t)n;
}

static esp_err_t api_readiness_get_handler(httpd_req_t *req)
{
    /* thermo_count gates several later items (bullet 2: "order and gate the
     * items rather than presenting a flat list of red crosses") -- read it
     * once up front rather than re-deriving the same gate per item. */
    uint8_t thermo_count = zones_config_get_thermo_count();

    char json[3072];
    size_t o = 0;
    int n = snprintf(json, sizeof(json), "{\"items\":[");
    o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    bool first = true;

    /* 1. Network configured. AP-only is an explicit, recordable choice
     * (wifi_prov.c's mode model -- "AP only, forever, by user choice"), not
     * an unfinished home-network setup, so it is reported as deliberately
     * off rather than nagged. */
    {
        wifi_prov_mode_t mode = wifi_prov_get_mode();
        readiness_status_t st;
        char detail[96];
        if (mode == WIFI_PROV_MODE_AP) {
            st = READY_DELIBERATELY_OFF;
            snprintf(detail, sizeof(detail), "AP-only mode chosen -- no home network join attempted");
        } else {
            wifi_prov_saved_network_t saved[1];
            size_t saved_count = 0;
            wifi_prov_get_saved_networks(saved, 1, &saved_count);
            if (saved_count > 0) {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "home network saved");
            } else {
                st = READY_NOT_DONE;
                snprintf(detail, sizeof(detail), "no network saved yet -- AP-only until one is added");
            }
        }
        o = append_item(json, sizeof(json), o, first, "network", "Network configured", st, detail, "/wifi");
        first = false;
    }

    /* 2. Thermocouple count / zone mapping. Gates almost everything below --
     * relay assignment, control mode, guard limits, calibration, and
     * autotune are all meaningless before the board knows how many zones
     * exist (bullet 2's example is literally this one). */
    {
        readiness_status_t st = thermo_count > 0 ? READY_OK : READY_NOT_DONE;
        char detail[64];
        snprintf(detail, sizeof(detail), "%u of %u channels configured", thermo_count,
                 (unsigned)MAX31856_CHANNEL_COUNT);
        o = append_item(json, sizeof(json), o, first, "thermo_count", "Thermocouple count / zone mapping", st,
                        detail, "/settings/zones");
        first = false;
    }

    /* 3. Relays assigned to zones. */
    {
        readiness_status_t st;
        char detail[80];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t assigned = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                uint8_t mask = 0;
                if (zones_config_get_relay_mask(i, &mask) && mask != 0) {
                    assigned++;
                }
            }
            st = (assigned == thermo_count) ? READY_OK : READY_NOT_DONE;
            snprintf(detail, sizeof(detail), "%u of %u zones have a relay assigned", assigned, thermo_count);
        }
        o = append_item(json, sizeof(json), o, first, "relays_assigned", "Relays assigned to zones", st, detail,
                        "/settings/zones");
        first = false;
    }

    /* 4. Control mode chosen per zone. NOTE: zone_control_mode_t's OFF
     * (0) is BOTH the zero-initialized default of an untouched zone AND a
     * legitimate operator choice ("a zone the board supports but the kiln
     * doesn't use" -- zones_http.h's own doc comment on the enum). There is
     * no separate "chosen" bit for this field the way there is for
     * max_temp_c/cross_zone_max_delta_c, so this item cannot honestly tell
     * "never touched" from "deliberately left off" -- it is reported ok as
     * soon as the zone exists, since every value the field can hold
     * (including 0) is a real, actionable mode. This is a known gap (see
     * TODO.md 8.3's honesty note); closing it would need a persisted
     * "mode explicitly set" flag this pass does not add. */
    {
        readiness_status_t st = (thermo_count > 0) ? READY_OK : READY_CANNOT_YET;
        char detail[80];
        if (thermo_count == 0) {
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            snprintf(detail, sizeof(detail), "every configured zone has a mode (OFF is a valid choice)");
        }
        o = append_item(json, sizeof(json), o, first, "control_mode", "Control mode chosen per zone", st, detail,
                        "/settings/zones");
        first = false;
    }

    /* 5. Guard limits (max_temp_c). 0 == "no ceiling", an explicit,
     * legitimate choice per zones_http.h's own doc comment on the field --
     * bullet 3 names this exact field as the example. Reported
     * deliberately_off (not not_done) whenever every configured zone reads
     * 0, so operators who genuinely want no ceiling are never nagged. */
    {
        readiness_status_t st;
        char detail[96];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t set_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float max_c = 0.0f, min_c = 0.0f;
                if (zones_config_get_temp_limits(i, &max_c, &min_c) && max_c > 0.0f) {
                    set_count++;
                }
            }
            if (set_count == thermo_count) {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "all %u zones have an explicit max_temp_c ceiling",
                         thermo_count);
            } else if (set_count == 0) {
                st = READY_DELIBERATELY_OFF;
                snprintf(detail, sizeof(detail), "max_temp_c is 0 (no ceiling) on all %u zones", thermo_count);
            } else {
                st = READY_OK; /* mixed: every zone has SOME definite choice, 0 or set */
                snprintf(detail, sizeof(detail), "%u of %u zones have a max_temp_c ceiling, rest have none set",
                         set_count, thermo_count);
            }
        }
        o = append_item(json, sizeof(json), o, first, "guard_max_temp", "Guard limits (max_temp_c)", st, detail,
                        "/settings/zones");
        first = false;
    }

    /* 6. Cross-zone plausibility guard (guard 8). Same 0-is-deliberate
     * convention as max_temp_c above, per zones_http.h's doc comment on
     * cross_zone_max_delta_c -- and it needs >=2 zones reporting to mean
     * anything at all (zones_http.h: "stays inert on a single-zone kiln"). */
    {
        readiness_status_t st;
        char detail[96];
        if (thermo_count < 2) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "needs at least 2 zones to mean anything (guard is inert otherwise)");
        } else {
            uint8_t set_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float delta = 0.0f;
                if (zones_config_get_cross_zone_delta(i, &delta) && delta > 0.0f) {
                    set_count++;
                }
            }
            if (set_count == 0) {
                st = READY_DELIBERATELY_OFF;
                snprintf(detail, sizeof(detail), "cross_zone_max_delta_c is 0 (guard disabled) on all zones");
            } else {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "%u of %u zones have the guard configured", set_count,
                         thermo_count);
            }
        }
        o = append_item(json, sizeof(json), o, first, "guard_cross_zone", "Cross-zone plausibility guard", st,
                        detail, "/settings/zones");
        first = false;
    }

    /* 7. Thermocouple calibration offsets. The storage cannot tell "never
     * looked at" from "looked at and correctly left at zero" -- cal_offset_c
     * has no separate reviewed bit -- and a thermocouple that reads true
     * genuinely needs no offset. So an all-zero result is NOT an unfinished
     * task: there is no action the operator must take, and reporting
     * not_done for it (as this item used to) told them to go do something
     * that does not exist.
     *
     * Chosen fix: report it accurately as an informational state and rename
     * the item after what is actually being reported. The alternative --
     * persisting a real "offsets reviewed" acknowledgement in kiln_nvs when
     * the zones page is saved -- would be strictly more informative, but it
     * buys a new NVS key, a migration for every board already in the field
     * (all of which would read back "never reviewed" and start nagging), and
     * a write coupling from zones_http.c into this module, all to add a
     * checkbox to a low-stakes, non-safety item that gates nothing. Not
     * worth it; if a "first-run walkthrough" ever needs that bit, that is
     * the feature to add it with.
     *
     * deliberately_off is the closest of the four existing states: nothing
     * is missing and no red cross is warranted. */
    {
        readiness_status_t st;
        char detail[80];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            /* There is no standalone zones_config_get_cal() getter -- reuse
             * zones_config_apply_cal(), which returns raw_c + cal_offset_c
             * (zones_http.c), against a raw_c of 0 to recover the offset
             * itself without needing a new accessor. */
            uint8_t cal_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float cal = zones_config_apply_cal(i, 0.0f);
                if (cal != 0.0f) {
                    cal_count++;
                }
            }
            if (cal_count > 0) {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "%u of %u zones have a calibration offset applied",
                         cal_count, thermo_count);
            } else {
                st = READY_DELIBERATELY_OFF;
                snprintf(detail, sizeof(detail),
                         "no offsets applied -- correct if your thermocouples read true");
            }
        }
        o = append_item(json, sizeof(json), o, first, "calibration", "Thermocouple calibration offsets", st,
                        detail, "/settings/zones");
        first = false;
    }

    /* 8. At least one profile available to fire. The 28 shipped schedules
     * (profiles_builtin.c) are as runnable as a user's own saved slots, so a
     * board straight out of the box already satisfies this -- counting only
     * user slots, as this item used to, told operators to go create a
     * profile while a full catalogue sat on the /profiles page. Hidden
     * built-ins do not count: hiding is the user's way of removing one from
     * their listings, and something they cannot see is not something they
     * can pick and fire.
     *
     * Renamed from "At least one fire profile saved" for the same reason:
     * the thing satisfying it is shipped, not saved. */
    {
        uint8_t saved = 0;
        for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
            profile_t p;
            if (profiles_http_get(id, &p)) {
                saved++;
            }
        }
        uint16_t builtin_visible = 0;
        for (size_t i = 0; i < g_builtin_profile_count; i++) {
            uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
            if (!profiles_builtin_is_hidden(id)) {
                builtin_visible++;
            }
        }
        bool any = (saved > 0) || (builtin_visible > 0);
        char detail[96];
        if (!any) {
            snprintf(detail, sizeof(detail),
                     "no profiles saved and every shipped schedule has been removed");
        } else {
            snprintf(detail, sizeof(detail), "%u saved, %u shipped schedule%s available", saved,
                     (unsigned)builtin_visible, builtin_visible == 1 ? "" : "s");
        }
        readiness_status_t st = any ? READY_OK : READY_NOT_DONE;
        o = append_item(json, sizeof(json), o, first, "profile_saved",
                        "At least one fire profile available", st, detail, "/profiles");
        first = false;
    }

    /* 9. Autotune run per zone, or gains entered by hand -- either counts,
     * per TODO.md 8.3's own wording ("or gains entered by hand"). */
    {
        readiness_status_t st;
        char detail[96];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t tuned = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float kp = 0, ki = 0, kd = 0;
                float k_dc = 0, tau_s = 0, dead_time_s = 0;
                /* kp > 0 specifically, NOT "any of kp/ki/kd nonzero". A PID
                 * loop with kp == 0 has no proportional term at all: ki alone
                 * integrates its way to setpoint eventually and kd alone does
                 * nothing but fight noise, so neither is a usable set of
                 * gains for a kiln. The looser any-of test previously here
                 * reported a zone as tuned when only kd had been typed in,
                 * which is exactly the "green light on an untuned zone" this
                 * whole page exists to prevent. ki/kd are still allowed to be
                 * zero -- a pure-P zone is crude but genuinely functional. */
                bool has_gains = zones_config_get_pid(i, &kp, &ki, &kd) && kp > 0.0f;
                bool has_model =
                    zones_config_get_model(i, &k_dc, &tau_s, &dead_time_s) && k_dc > 0.0f && tau_s > 0.0f;
                if (has_gains || has_model) {
                    tuned++;
                }
            }
            st = (tuned == thermo_count) ? READY_OK : READY_NOT_DONE;
            snprintf(detail, sizeof(detail), "%u of %u zones have gains or an identified model", tuned,
                     thermo_count);
        }
        o = append_item(json, sizeof(json), o, first, "autotune", "Autotune run per zone (or gains by hand)", st,
                        detail, "/settings/zones");
        first = false;
    }

    /* 10. Hardware present and answering. Mirrors /api/status's io_ready/
     * thermo_ready/safety_ready exactly (dashboard_http_get_hw_ready()). */
    {
        bool io_ready = false, thermo_ready = false, safety_ready = false;
        dashboard_http_get_hw_ready(&io_ready, &thermo_ready, &safety_ready);
        readiness_status_t st = (io_ready && thermo_ready && safety_ready) ? READY_OK : READY_NOT_DONE;
        char detail[96];
        snprintf(detail, sizeof(detail), "io=%s thermo=%s safety=%s", io_ready ? "up" : "down",
                 thermo_ready ? "up" : "down", safety_ready ? "up" : "down");
        /* fix_url used to be "/" -- owner report: "the ready to fire hardware
         * connected item just takes me to the main page." "/" is a real,
         * registered route (wifi_provision_http.c's index_get_handler(), the
         * dashboard), so this never 404'd or looked broken; it just landed
         * somewhere that cannot help diagnose which of io/thermo/safety is
         * down. "/diagnostics" is the actual system-health hub for exactly
         * this (diagnostics_http.c: ESP + board-health info, plus links to
         * /diagnostics/thermo and /safety for the per-subsystem detail this
         * item's own `detail` string already breaks io/thermo/safety out
         * into). */
        o = append_item(json, sizeof(json), o, first, "hardware", "Hardware present and answering", st, detail,
                        "/diagnostics");
        first = false;
    }

    /* 10a. Safety processor commissioned. Added 2026-08-22: the whole
     * commissioning parameter set (trip thresholds, TC type, guard limits --
     * COMMISSIONING.md sec 2.1) could be entirely unset and this page still
     * showed every light green, because nothing here ever asked. That is the
     * worst possible omission on a readiness page: the safety processor is
     * the thing that stops a runaway, and an uncommissioned one has no
     * thresholds to trip on.
     *
     * cached_crc == 0 means "never fetched a config from the Pico" -- 0 is
     * never a real CRC, so it doubles as "never commissioned"
     * (safety_cfg_store.h's own note on the sentinel). A param reading
     * set == false is one the Pico has no value for; the sec-1 fields with
     * no compiled-in default read exactly this way until an operator sets
     * them, so any unset param means commissioning is genuinely incomplete
     * rather than merely un-refetched.
     *
     * Reported cannot_yet (not not_done) when the safety link is down: with
     * no link the ESP cannot tell an uncommissioned Pico from one it simply
     * has not talked to yet, and nagging about a task the operator cannot
     * perform right now is what READY_CANNOT_YET exists for. */
    {
        bool io_ready = false, thermo_ready = false, safety_ready = false;
        dashboard_http_get_hw_ready(&io_ready, &thermo_ready, &safety_ready);

        char detail[112];
        uint16_t cached_crc = safety_cfg_store_cached_crc();
        size_t count = safety_cfg_store_param_count();
        size_t unset = 0;
        for (size_t i = 0; i < count; i++) {
            safety_cfg_param_t p;
            if (safety_cfg_store_get_by_index(i, &p) && !p.set) {
                unset++;
            }
        }

        readiness_status_t st = readiness_commissioning_status(safety_ready, cached_crc, unset, count);

        if (cached_crc == 0) {
            snprintf(detail, sizeof(detail), "%s",
                     safety_ready ? "no commissioning values have ever been read from the safety processor"
                                  : "safety link is down -- cannot tell uncommissioned from unread");
        } else if (unset == 0) {
            snprintf(detail, sizeof(detail), "all %u safety parameters have values (config_crc 0x%04X)",
                     (unsigned)count, (unsigned)cached_crc);
        } else {
            snprintf(detail, sizeof(detail), "%u of %u safety parameters still have no value", (unsigned)unset,
                     (unsigned)count);
        }
        o = append_item(json, sizeof(json), o, first, "safety_commissioned", "Safety processor commissioned", st,
                        detail, "/safety/commissioning");
        first = false;
    }

    /* 11. Storage sections compatible -- the zones_config_valid flag (TODO.md
     * 8.2) plus every NVS partition's present/mounted state (nvs_report.c). */
    {
        bool zones_valid = zones_config_is_valid();
        size_t nvs_count = 0;
        const nvs_report_section_t *sections = nvs_report_get(&nvs_count);
        bool all_mounted = true;
        char bad_name[32] = "";
        for (size_t i = 0; i < nvs_count; i++) {
            if (!sections[i].present || !sections[i].mounted) {
                all_mounted = false;
                strncpy(bad_name, sections[i].name, sizeof(bad_name) - 1);
                break;
            }
        }
        readiness_status_t st = (zones_valid && all_mounted) ? READY_OK : READY_NOT_DONE;
        char detail[96];
        if (!zones_valid) {
            snprintf(detail, sizeof(detail), "zone config failed to load cleanly -- see /settings/zones");
        } else if (!all_mounted) {
            snprintf(detail, sizeof(detail), "NVS partition '%s' is not present/mounted", bad_name);
        } else {
            snprintf(detail, sizeof(detail), "all storage sections mounted, zone config valid");
        }
        o = append_item(json, sizeof(json), o, first, "storage", "Storage sections compatible", st, detail,
                        "/settings/zones");
        first = false;
    }

    n = snprintf(json + o, sizeof(json) - o, "]}");
    o = (n < 0 || (size_t)n >= sizeof(json) - o) ? o : o + (size_t)n;

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding is legal and served
 * gzip (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "readiness_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)readiness_page_html_gz_start,
                           (size_t)(readiness_page_html_gz_end - readiness_page_html_gz_start));
}

esp_err_t readiness_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/readiness", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t api_uri = {
        .uri = "/api/readiness", .method = HTTP_GET, .handler = api_readiness_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/readiness) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/readiness) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "readiness API up");
    return ESP_OK;
}
