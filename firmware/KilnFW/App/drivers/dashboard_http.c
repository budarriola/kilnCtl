#include "dashboard_http.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "autotune_engine.h"
#include "http_form.h"
#include "nvs_report.h"
#include "profile_executor.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "sim_backend.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "dashboard_http";

/* application/x-www-form-urlencoded, "relay=4&on=1" plus headroom -- same
 * reasoning as wifi_provision_http.c's PROV_BODY_MAX: bounded well above
 * what a legitimate request needs, checked against Content-Length before a
 * single byte is read. */
#define RELAY_BODY_MAX 64

static struct {
    kiln_io_t *io;
    MAX31856BusClass *thermo_bus;
    SafetyLinkClass *safety;
} s_dash;

/* TODO.md 10.1a: the data-gathering half of GET /api/status, pulled out into
 * a plain function so the LCD home page (ui_page_home.c) reads the exact
 * same snapshot this handler serializes, rather than a second reimplementation
 * against kiln_io/MAX31856_read_all/etc. See dashboard_http.h for the struct
 * and the "why not nvs_sections too" note. */
void dashboard_get_status(dashboard_status_t *out)
{
    memset(out, 0, sizeof(*out));

    out->io_ready = s_dash.io != NULL;
    if (out->io_ready) {
        kiln_io_state_t st;
        memset(&st, 0, sizeof(st));
        esp_err_t err = kiln_io_read(s_dash.io, &st);
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            out->relay_on[relay - 1] = (st.relay_shadow & (1u << (relay - 1))) != 0;
        }
        out->io_read_failed = (err != ESP_OK);
    }

    /* Lifetime contact-cycle count per relay (TODO.md 6A.1). Reported even
     * with no expander attached: it is persisted history, not live hardware
     * state, and a relay's accumulated wear is exactly the thing an operator
     * wants to see when deciding whether to replace one. */
    {
        uint32_t cycles[KILN_IO_RELAY_COUNT];
        relay_cycles_get(cycles);
        memcpy(out->relay_cycles, cycles, sizeof(cycles));
    }

    /* thermo_bus->initialized only means the shared SPI bus came up -- it
     * says nothing about whether any MAX31856 actually answered on it (see
     * MAX31856_bus_init). A board-less bus reads back count == 0 from
     * MAX31856_read_all with no error, so "ready" has to mean "at least one
     * channel responded," not "the bus exists." */
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t count = 0;
    if (sim_backend_enabled()) {
        /* A sim build answers here too, so the dashboard shows the same
         * fabricated kiln the executor is controlling rather than
         * "no thermocouple" next to a running firing. */
        sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
    } else if (s_dash.thermo_bus && s_dash.thermo_bus->initialized) {
        MAX31856_read_all(s_dash.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
    }
    out->thermo_ready = count > 0;
    out->channel_count = count;
    for (size_t i = 0; i < count; i++) {
        const MAX31856Reading *r = &readings[i];
        bool valid = !r->spi_failed && !isnan(r->tc_temperature_c);
        /* NaN has no valid JSON literal -- report 0 alongside valid:false
         * rather than emit invalid JSON or a string that breaks a naive
         * client-side parseFloat(). Calibration applied here (zone i <->
         * channel i, per zones_http.h) -- see zones_config_apply_cal()'s
         * doc comment for the UART-bridge-side scope gap this leaves. */
        float temp = isnan(r->tc_temperature_c) ? 0.0f : zones_config_apply_cal(r->channel, r->tc_temperature_c);
        float cj = isnan(r->cj_temperature_c) ? 0.0f : r->cj_temperature_c;
        out->channels[i].channel = r->channel;
        out->channels[i].temp_c = temp;
        out->channels[i].cj_c = cj;
        out->channels[i].valid = valid;
        out->channels[i].fault_status = r->fault_status;
        out->channels[i].spi_failed = r->spi_failed;
        out->channels[i].stale = r->stale;
    }

    out->safety_ready = s_dash.safety != NULL;

    /* ROADMAP.md M6: safety/enclosure temperature straight from the safety
     * link's cache -- see dashboard_http.h's field comment for the wire
     * source and why power_w has no data yet. safety_link_get_status()
     * itself is null-tolerant on a never-initialized link (returns an error,
     * leaving out->safety_temp_c/enclosure_temp_c at the memset(0) above,
     * caught by the isnan() check anyway since a plain 0.0f would otherwise
     * read as a real, very cold reading). */
    if (s_dash.safety) {
        safety_link_status_t sl;
        if (safety_link_get_status(s_dash.safety, &sl) == ESP_OK) {
            out->safety_temp_c = sl.tc_temp_c;
            out->safety_temp_valid = !isnan(sl.tc_temp_c);
            out->enclosure_temp_c = sl.cj_temp_c;
            out->enclosure_temp_valid = !isnan(sl.cj_temp_c);

            /* ROADMAP.md M5/M6, TODO.md 10.10: SAFETY_CMD_POWER (Frame E) now
             * has a real decode path in safety_link.c's safety_apply_power().
             * power_ever_received is false, and power_total_w stays NaN, until
             * a Pico that implements Frame E actually sends one -- same "no
             * hardware yet, honestly null" state as before, just a real path
             * instead of a permanent stub (see this file's history / TODO.md
             * 10.10 for the prior state). */
            if (sl.power_ever_received) {
                out->power_w = sl.power_total_w;
                out->power_valid = !isnan(sl.power_total_w);
            }
        }
    }
    if (!out->safety_temp_valid) {
        out->safety_temp_c = NAN;
    }
    if (!out->enclosure_temp_valid) {
        out->enclosure_temp_c = NAN;
    }
    if (!out->power_valid) {
        out->power_w = NAN;
    }

    /* TODO.md 8.2 "Tie it to the guards, not only the UI": surface the same
     * flag profile_executor.c/autotune_engine.c now refuse on, so the
     * dashboard and pc_tools' Zones panel can say "zone config failed to
     * load" explicitly instead of a kiln that just silently won't fire. */
    out->zones_config_valid = zones_config_is_valid();
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[896];
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

    dashboard_status_t ds;
    dashboard_get_status(&ds);

    APPEND("{\"io_ready\":%s", ds.io_ready ? "true" : "false");

    if (ds.io_ready) {
        APPEND(",\"relays\":[");
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            bool on = ds.relay_on[relay - 1];
            APPEND("%s{\"relay\":%u,\"on\":%s}", relay == 1 ? "" : ",", relay, on ? "true" : "false");
        }
        APPEND("]");
        if (ds.io_read_failed) {
            APPEND(",\"io_read_failed\":true");
        }
    }

    APPEND(",\"relay_cycles\":[");
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        APPEND("%s%lu", r == 0 ? "" : ",", (unsigned long)ds.relay_cycles[r]);
    }
    APPEND("]");

    APPEND(",\"thermo_ready\":%s", ds.thermo_ready ? "true" : "false");

    if (ds.thermo_ready) {
        APPEND(",\"channels\":[");
        for (size_t i = 0; i < ds.channel_count; i++) {
            const dashboard_channel_status_t *r = &ds.channels[i];
            APPEND(
                "%s{\"channel\":%u,\"temp_c\":%.2f,\"cj_c\":%.2f,\"valid\":%s,\"fault_status\":%u,"
                "\"spi_failed\":%s,\"stale\":%s}",
                i == 0 ? "" : ",", r->channel, (double)r->temp_c, (double)r->cj_c, r->valid ? "true" : "false",
                r->fault_status, r->spi_failed ? "true" : "false", r->stale ? "true" : "false");
        }
        APPEND("]");
    }

    APPEND(",\"safety_ready\":%s", ds.safety_ready ? "true" : "false");
    APPEND(",\"zones_config_valid\":%s", ds.zones_config_valid ? "true" : "false");

    /* ROADMAP.md M6 -- null (not 0), same convention board_temps.c's
     * GET /api/board_temps already established (TODO.md 10.7): a JSON null
     * cannot be mistaken for a real 0 C reading or a real 0 W power figure
     * the way a bare 0 could. */
    APPEND(",\"safety_temp_c\":%s", ds.safety_temp_valid ? "" : "null");
    if (ds.safety_temp_valid) {
        APPEND("%.2f", (double)ds.safety_temp_c);
    }
    APPEND(",\"enclosure_temp_c\":%s", ds.enclosure_temp_valid ? "" : "null");
    if (ds.enclosure_temp_valid) {
        APPEND("%.2f", (double)ds.enclosure_temp_c);
    }
    APPEND(",\"power_w\":%s", ds.power_valid ? "" : "null");
    if (ds.power_valid) {
        APPEND("%.1f", (double)ds.power_w);
    }

    /* TODO.md 8.2's "one boot-time report": present/mounted per NVS
     * partition, so the wizard (8.3) can say precisely which storage section
     * is missing rather than an operator discovering it as an unexplained
     * "unconfigured" zone/rule/profile page. */
    {
        size_t nvs_count = 0;
        const nvs_report_section_t *sections = nvs_report_get(&nvs_count);
        APPEND(",\"nvs_sections\":[");
        for (size_t i = 0; i < nvs_count; i++) {
            APPEND("%s{\"name\":\"%s\",\"present\":%s,\"mounted\":%s}", i == 0 ? "" : ",",
                   sections[i].name, sections[i].present ? "true" : "false",
                   sections[i].mounted ? "true" : "false");
        }
        APPEND("]");
    }

    APPEND("}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* See dashboard_http.h -- mirrors status_get_handler()'s io_ready/
 * thermo_ready/safety_ready computation exactly (same order, same
 * sim_backend_enabled() branch) so readiness_http.c can never see a
 * different answer than /api/status does for the same three flags. */
void dashboard_http_get_hw_ready(bool *out_io_ready, bool *out_thermo_ready, bool *out_safety_ready)
{
    if (out_io_ready) {
        *out_io_ready = s_dash.io != NULL;
    }
    if (out_safety_ready) {
        *out_safety_ready = s_dash.safety != NULL;
    }
    if (out_thermo_ready) {
        MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
        size_t count = 0;
        if (sim_backend_enabled()) {
            sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
        } else if (s_dash.thermo_bus && s_dash.thermo_bus->initialized) {
            MAX31856_read_all(s_dash.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
        }
        *out_thermo_ready = count > 0;
    }
}

/* See dashboard_http.h's doc comment -- the plain-C action function
 * extracted from relay_post_handler() so a non-HTTP caller (ui_page_temperature.c)
 * goes through the same ownership/safety gate and the same kiln_io write,
 * not a reimplementation of either. relay_post_handler() below is now a thin
 * wrapper: parse the HTTP body, call this, translate the result to an HTTP
 * status. */
dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)
{
    if (!s_dash.io) {
        return DASHBOARD_RELAY_ERR_NO_BOARD;
    }
    if (relay_index < 1 || relay_index > KILN_IO_RELAY_COUNT) {
        return DASHBOARD_RELAY_ERR_RANGE;
    }

    /* TODO.md section 0's ownership decision: a relay a running (or paused,
     * pre-resume) profile claimed is refused to a manual command in either
     * direction -- not just ON -- since a de-energize mid-window fights the
     * executor's own time-proportioning exactly as much as an unwanted
     * energize does. */
    if (relay_authority_manual_blocked_by_owner(relay_index)) {
        ESP_LOGW(TAG, "dashboard: relay %u refused -- owned by a running profile", (unsigned)relay_index);
        return DASHBOARD_RELAY_ERR_OWNED;
    }

    if (on) {
        uint32_t sources = 0;
        if (relay_authority_on_blocked(s_dash.safety, &sources)) {
            ESP_LOGW(TAG, "dashboard: relay %u ON refused -- safety fault sources 0x%02X",
                     (unsigned)relay_index, (unsigned)sources);
            if (out_safety_sources) {
                *out_safety_sources = sources;
            }
            return DASHBOARD_RELAY_ERR_SAFETY;
        }
    }

    esp_err_t err = kiln_io_set_relay(s_dash.io, relay_index, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "kiln_io_set_relay failed: %s", esp_err_to_name(err));
        return DASHBOARD_RELAY_ERR_IO_FAIL;
    }
    return DASHBOARD_RELAY_OK;
}

static esp_err_t relay_post_handler(httpd_req_t *req)
{
    if (!s_dash.io) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no relay board attached");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > RELAY_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[RELAY_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "relay body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char relay_val[4];
    char on_val[4];
    int relay_len = http_form_find_field(body, "relay", relay_val, sizeof(relay_val));
    int on_len = http_form_find_field(body, "on", on_val, sizeof(on_val));
    if (relay_len <= 0 || on_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay/on missing");
        return ESP_OK;
    }

    long relay = strtol(relay_val, NULL, 10);
    if (relay < 1 || relay > KILN_IO_RELAY_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay out of range");
        return ESP_OK;
    }
    bool want_on = on_val[0] == '1';

    /* dashboard_set_relay() -- see this file's definition above and
     * dashboard_http.h's doc comment -- is now the one place the
     * ownership/safety gate and the kiln_io write happen; this handler only
     * translates its result to an HTTP status. */
    switch (dashboard_set_relay((uint8_t)relay, want_on, NULL)) {
    case DASHBOARD_RELAY_OK:
        return httpd_resp_sendstr(req, "ok");
    case DASHBOARD_RELAY_ERR_NO_BOARD:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no relay board attached");
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_RANGE:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay out of range");
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_OWNED:
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "relay owned by a running profile");
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_SAFETY:
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "blocked by safety fault");
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_IO_FAIL:
    default:
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "relay command failed");
        return ESP_OK;
    }
}

/* ---- Profile executor (TODO.md section 6) --------------------------------- */

static const char *exec_state_name(profile_exec_state_t s)
{
    switch (s) {
    case PROFILE_EXEC_IDLE: return "idle";
    case PROFILE_EXEC_RUNNING: return "running";
    case PROFILE_EXEC_PAUSED: return "paused";
    case PROFILE_EXEC_DONE: return "done";
    case PROFILE_EXEC_FAULTED: return "faulted";
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

/* Shared by both handlers below: one JSON object per active zone. Appends
 * to *o, using the APPEND-into-json-buffer pattern every other handler in
 * this file already uses (the caller owns the buffer/APPEND macro since C
 * has no closures to hand this a local one). control_fields selects
 * between /api/profile_exec's exec-lifecycle shape and /api/control's
 * tuning-focused shape (TODO.md 6A.9 asks for both, as separate endpoints
 * with different focuses, not one bloated one). */
static size_t append_zone_status_json(char *json, size_t cap, size_t o, const profile_exec_status_t *st,
                                      bool control_fields)
{
    int n;
    bool first = true;
    n = snprintf(json + o, cap - o, "\"zones\":[");
    if (n < 0 || (size_t)n >= cap - o) return o;
    o += (size_t)n;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st->zones[zi];
        if (!z->active) continue;
        char reason_escaped[sizeof(z->fault_reason) * 2 + 1];
        reason_escaped[0] = '\0';
        if (z->faulted) {
            json_escape(z->fault_reason, reason_escaped, sizeof(reason_escaped));
        }
        if (control_fields) {
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"control_mode\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,"
                        "\"duty\":%.3f,\"relay_on\":%s,\"pid_p\":%.4f,\"pid_i\":%.4f,\"pid_d\":%.4f,"
                        "\"pid_ff\":%.4f,\"cooling_limited\":%s,\"faulted\":%s,\"fault_guard\":%u}",
                        first ? "" : ",", zi, z->control_mode, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", (double)z->duty,
                        z->relay_commanded_on ? "true" : "false", (double)z->pid_p, (double)z->pid_i,
                        (double)z->pid_d, (double)z->pid_ff, z->cooling_limited ? "true" : "false",
                        z->faulted ? "true" : "false", z->fault_guard);
        } else {
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,\"relay_on\":%s,"
                        "\"duty\":%.3f,\"control_mode\":%u,\"faulted\":%s,\"fault_reason\":\"%s\","
                        "\"fault_guard\":%u}",
                        first ? "" : ",", zi, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", z->relay_commanded_on ? "true" : "false",
                        (double)z->duty, z->control_mode, z->faulted ? "true" : "false", reason_escaped,
                        z->fault_guard);
        }
        if (n < 0 || (size_t)n >= cap - o) return o;
        o += (size_t)n;
        first = false;
    }
    n = snprintf(json + o, cap - o, "]");
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    return o;
}

/* TODO.md 6A.3's "no auto-resume" breadcrumb, appended to /api/profile_exec
 * under its own "last_run" key -- deliberately NOT merged into the live
 * status fields above it. A client that confused the two would show a firing
 * from before the reboot as if it were happening now, which is the one
 * misreading this record must never invite. "present" is false whenever
 * there is nothing to report (first boot, or the operator has acknowledged
 * it), so the banner logic on the page is a single check.
 *
 * uptime_at_write_s, not a timestamp: this board has no RTC and no
 * guaranteed SNTP, so there is no honest absolute time to send. See
 * run_state.h. */
static size_t append_last_run_json(char *json, size_t cap, size_t o)
{
    run_state_record_t rec;
    int n;
    if (!run_state_get_boot_record(&rec)) {
        n = snprintf(json + o, cap - o, "\"last_run\":{\"present\":false},");
        return (n < 0 || (size_t)n >= cap - o) ? o : o + (size_t)n;
    }

    char name_escaped[sizeof(rec.profile_name) * 2 + 1];
    json_escape(rec.profile_name, name_escaped, sizeof(name_escaped));
    char reason_escaped[sizeof(rec.fault_reason) * 2 + 1];
    json_escape(rec.fault_reason, reason_escaped, sizeof(reason_escaped));

    n = snprintf(json + o, cap - o,
        "\"last_run\":{\"present\":true,\"interrupted\":%s,\"phase\":\"%s\",\"profile_id\":%u,"
        "\"profile_name\":\"%s\",\"zone_mask\":%u,\"segment_index\":%u,\"segment_count\":%u,"
        "\"dwelling\":%s,\"target_c\":%.2f,\"segment_elapsed_s\":%lu,\"uptime_at_write_s\":%lu,"
        "\"fault_guard\":%u,\"fault_reason\":\"%s\"},",
        run_state_boot_record_interrupted() ? "true" : "false",
        run_state_phase_name((run_state_phase_t)rec.phase), rec.profile_id, name_escaped, rec.zone_mask,
        rec.segment_index, rec.segment_count, rec.dwelling ? "true" : "false", (double)rec.target_c,
        (unsigned long)rec.segment_elapsed_s, (unsigned long)rec.uptime_s, rec.fault_guard, reason_escaped);
    return (n < 0 || (size_t)n >= cap - o) ? o : o + (size_t)n;
}

static esp_err_t profile_exec_status_get_handler(httpd_req_t *req)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    char name_escaped[sizeof(st.profile_name) * 2 + 1];
    json_escape(st.profile_name, name_escaped, sizeof(name_escaped));
    char reason_escaped[sizeof(st.fault_reason) * 2 + 1];
    json_escape(st.fault_reason, reason_escaped, sizeof(reason_escaped));

    /* Sized against the real worst case rather than the previous estimate.
     * Per zone the exec shape can emit a fully backslash-escaped 95-char
     * fault_reason (190 bytes) on top of ~130 bytes of fixed keys, so 320,
     * not 224 -- 224 was already optimistic before this change and would
     * have truncated mid-object into invalid JSON in a multi-zone fault. The
     * 896-byte fixed part covers the run-level line (its own escaped reason)
     * plus the "last_run" object at ITS worst case. The httpd task runs on
     * an 8192-byte stack (wifi_provision_http.c), so ~2.5 KB here is
     * comfortable. */
    char json[896 + MAX31856_CHANNEL_COUNT * 320];
    int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"profile_id\":%u,\"profile_name\":\"%s\",\"zone_mask\":%u,"
        "\"segment_index\":%u,\"segment_count\":%u,\"dwelling\":%s,\"target_c\":%.2f,"
        "\"segment_elapsed_s\":%lu,\"dwell_remaining_s\":%lu,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,\"fault_reason\":\"%s\",\"fault_guard\":%u,",
        exec_state_name(st.state), st.profile_id, name_escaped, st.zone_mask, st.segment_index,
        st.segment_count, st.dwelling ? "true" : "false", (double)st.target_c,
        (unsigned long)st.segment_elapsed_s, (unsigned long)st.dwell_remaining_s,
        st.ramp_lock_held ? "true" : "false", st.ramp_lock_lagging_mask, reason_escaped, st.fault_guard);
    size_t o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    /* last_run BEFORE the zones array on purpose: both appenders stop rather
     * than overflow, and the zones array is the unbounded-ish one (up to
     * MAX31856_CHANNEL_COUNT escaped fault reasons). Emitting the fixed-size
     * breadcrumb first means an unusually verbose fault can never be what
     * silently drops it from the response. */
    o = append_last_run_json(json, sizeof(json), o);
    o = append_zone_status_json(json, sizeof(json), o, &st, false);
    if (o + 1 < sizeof(json)) json[o++] = '}';
    json[o < sizeof(json) ? o : sizeof(json) - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* TODO.md 6A.9: "a live control-status endpoint... mode, setpoint, actual,
 * duty, PID term breakdown... guard state, and the reason for any ramp-lock
 * hold." Backed by the same profile_executor_get_status() as
 * /api/profile_exec; this is a separate endpoint (not just more fields on
 * that one) because 6A.9 asks for one and a tuning UI wants a stable,
 * control-focused shape independent of the exec-lifecycle one. One entry
 * per active zone now that TODO.md 6A.5 made concurrent multi-zone
 * execution real -- ramp_lock_held/ramp_lock_lagging_mask are shared
 * (there's one ramp per run, TODO.md 6A.5(d)), the rest is per zone. */
static esp_err_t control_status_get_handler(httpd_req_t *req)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    char json[128 + MAX31856_CHANNEL_COUNT * 224];
    int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"zone_mask\":%u,\"target_c\":%.2f,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,",
        exec_state_name(st.state), st.zone_mask, (double)st.target_c, st.ramp_lock_held ? "true" : "false",
        st.ramp_lock_lagging_mask);
    size_t o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    o = append_zone_status_json(json, sizeof(json), o, &st, true);
    if (o + 1 < sizeof(json)) json[o++] = '}';
    json[o < sizeof(json) ? o : sizeof(json) - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* TODO.md section 0 / 6A.9: history ring buffer + CSV export, and the data
 * source for the dashboard graph. One row per sample; guard is
 * thermal_guard_trip_t (0 = none) so the graph can mark trips on the
 * timeline without a second request.
 *
 * Streamed in small batches via httpd_resp_send_chunk() rather than built
 * into one big buffer first: an earlier version allocated a full
 * HISTORY_MAX_SAMPLES-sized entries array (~58KB) *and* a full CSV text
 * buffer (~115KB) at once, and hit ESP_ERR_NO_MEM in practice against the
 * heap this board actually has free at runtime (Wi-Fi/lwIP/httpd already
 * hold a good chunk of it) -- the earlier size-report check only looked at
 * static DIRAM headroom, not the separate runtime heap this comes out of.
 * This version's peak allocation is one HISTORY_CSV_BATCH-sized entries
 * array plus one small text buffer, independent of how many samples exist. */
#define HISTORY_CSV_BATCH 128u

static esp_err_t history_csv_get_handler(httpd_req_t *req)
{
    profile_history_entry_t *batch = malloc(sizeof(profile_history_entry_t) * HISTORY_CSV_BATCH);
    char *line = malloc(96);
    if (!batch || !line) {
        free(batch);
        free(line);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"kiln_history.csv\"");

    int n = snprintf(line, 96, "elapsed_s,actual_c,desired_c,duty,guard\n");
    esp_err_t err = httpd_resp_send_chunk(req, line, n > 0 ? (size_t)n : 0);

    size_t start = 0;
    while (err == ESP_OK) {
        size_t got = profile_executor_get_history(batch, start, HISTORY_CSV_BATCH);
        if (got == 0) break;
        for (size_t i = 0; i < got && err == ESP_OK; i++) {
            n = snprintf(line, 96, "%lu,%.2f,%.2f,%.3f,%u\n", (unsigned long)batch[i].elapsed_s,
                        (double)batch[i].actual_c, (double)batch[i].desired_c, (double)batch[i].duty,
                        batch[i].guard);
            err = httpd_resp_send_chunk(req, line, n > 0 ? (size_t)n : 0);
        }
        start += got;
        if (got < HISTORY_CSV_BATCH) break; /* reached the end */
    }
    free(batch);
    free(line);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); /* terminates the chunked response */
    }
    return ESP_OK;
}

static esp_err_t profile_exec_start_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 32) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[33];
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

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long id = (id_len > 0) ? strtol(id_val, NULL, 10) : -1;
    if (id_len <= 0 || id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or invalid");
        return ESP_OK;
    }

    char err_msg[128] = "";
    if (!profile_executor_run((uint8_t)id, err_msg, sizeof(err_msg))) {
        char json[192];
        char err_escaped[128 * 2 + 1];
        json_escape(err_msg, err_escaped, sizeof(err_escaped));
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", err_escaped);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t profile_exec_stop_post_handler(httpd_req_t *req)
{
    profile_executor_halt();
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t profile_exec_pause_post_handler(httpd_req_t *req)
{
    bool ok = profile_executor_pause();
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing running to pause");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t profile_exec_resume_post_handler(httpd_req_t *req)
{
    bool ok = profile_executor_resume();
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing paused to resume");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* Dismisses the previous-run breadcrumb (TODO.md 6A.3). Note what this does
 * NOT do: it does not touch the executor, the relays, or any live firing.
 * The only thing being acknowledged is that a human has read the record --
 * which is exactly why acknowledging must be an explicit action rather than
 * something the page does for the operator on load. Idempotent; 400 only
 * when there was nothing to acknowledge, so a double-click on a slow link
 * doesn't look like a failure worth investigating. */
static esp_err_t profile_exec_ack_last_run_post_handler(httpd_req_t *req)
{
    if (!run_state_acknowledge()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no previous-run record to acknowledge");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

static const char *autotune_state_name(autotune_engine_state_t s)
{
    switch (s) {
    case AUTOTUNE_ENGINE_IDLE: return "idle";
    case AUTOTUNE_ENGINE_SETTLING: return "settling";
    case AUTOTUNE_ENGINE_STEPPING: return "stepping";
    case AUTOTUNE_ENGINE_RELAY_APPROACH: return "relay_approach";
    case AUTOTUNE_ENGINE_RELAY_CYCLING: return "relay_cycling";
    case AUTOTUNE_ENGINE_DONE: return "done";
    case AUTOTUNE_ENGINE_ABORTED: return "aborted";
    default: return "unknown";
    }
}

/* The rule is reported by name rather than by enum value because it is the
 * one part of a proposal an operator has to be able to judge for themselves:
 * "ziegler-nichols" is a warning label (pid_autotune.h: it is *designed* to
 * leave the loop oscillating), and a bare integer would not be. */
static const char *autotune_rule_name(autotune_rule_t r)
{
    switch (r) {
    case AUTOTUNE_RULE_SIMC: return "simc";
    case AUTOTUNE_RULE_ZIEGLER_NICHOLS: return "ziegler-nichols";
    case AUTOTUNE_RULE_TYREUS_LUYBEN: return "tyreus-luyben";
    default: return "unknown";
    }
}

static esp_err_t autotune_status_get_handler(httpd_req_t *req)
{
    autotune_engine_status_t st;
    autotune_engine_get_status(&st);

    char reason_escaped[sizeof(st.abort_reason) * 2 + 1];
    json_escape(st.abort_reason, reason_escaped, sizeof(reason_escaped));

    /* The relay fit's own rejection reason is reported verbatim alongside the
     * abort reason, for the same reason the RGA's is (see that handler): "not
     * a limit cycle" and "amplitude inside the hysteresis band" are different
     * findings about the kiln and the page must not flatten them. */
    char relay_reason_escaped[sizeof(st.relay.invalid_reason) * 2 + 1];
    json_escape(st.relay.invalid_reason, relay_reason_escaped, sizeof(relay_reason_escaped));

    char json[900];
    int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"method\":\"%s\",\"zone\":%u,\"elapsed_s\":%lu,\"sample_count\":%u,"
        "\"actual_c\":%.2f,\"actual_valid\":%s,\"duty\":%.3f,\"abort_reason\":\"%s\","
        "\"model_valid\":%s,\"k_gain_c_per_duty\":%.3f,\"tau_s\":%.1f,\"dead_time_s\":%.1f,"
        "\"proposed_kp\":%.5f,\"proposed_ki\":%.5f,\"proposed_kd\":%.5f,\"rule\":\"%s\","
        "\"predicted_max_ramp_c_per_hr\":%.1f,"
        "\"relay_setpoint_c\":%.1f,\"relay_d\":%.3f,\"relay_h_c\":%.2f,"
        "\"relay_cycles_seen\":%u,\"relay_cycles_target\":%u,"
        "\"relay_valid\":%s,\"relay_ku\":%.5f,\"relay_tu_s\":%.1f,\"relay_amplitude_c\":%.2f,"
        "\"relay_cycles_used\":%d,\"relay_reason\":\"%s\"}",
        autotune_state_name(st.state), st.method == AUTOTUNE_METHOD_RELAY ? "relay" : "step", st.zone_index,
        (unsigned long)st.elapsed_s, st.sample_count,
        (double)(st.actual_valid ? st.actual_c : 0.0f), st.actual_valid ? "true" : "false", (double)st.duty,
        reason_escaped, st.model.valid ? "true" : "false", (double)st.model.k_gain_c_per_duty,
        (double)st.model.tau_s, (double)st.model.dead_time_s, (double)st.proposed_gains.kp,
        (double)st.proposed_gains.ki, (double)st.proposed_gains.kd,
        autotune_rule_name(st.proposed_gains.rule), (double)st.predicted_max_ramp_c_per_hr,
        (double)st.relay_setpoint_c, (double)st.relay_amplitude_duty, (double)st.relay_hysteresis_c,
        st.relay_cycles_seen, st.relay_cycles_target,
        st.relay.valid ? "true" : "false", (double)st.relay.ku, (double)st.relay.tu_s,
        (double)st.relay.amplitude_c, st.relay.cycles_used, relay_reason_escaped);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* TODO.md 6A.5(b): cross-zone coupling matrix built up across completed
 * autotune runs (one row per zone that's been tested). Cheap enough
 * (MAX31856_CHANNEL_COUNT^2 cells) to send as one JSON object, no pagination
 * needed unlike the trace/history endpoints. */
static esp_err_t autotune_matrix_get_handler(httpd_req_t *req)
{
    autotune_coupling_matrix_t m;
    autotune_engine_get_coupling_matrix(&m);

    /* Second term is the cells array, third is the RGA block appended below
     * (n^2 Lambda values plus the zone map, or a refusal reason). */
    char json[64 + MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT * 96
              + 128 + MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT * 16];
    size_t o = 0;
    o += snprintf(json + o, sizeof(json) - o, "{\"zone_count\":%u,\"cells\":[", (unsigned)MAX31856_CHANNEL_COUNT);
    bool first = true;
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            const autotune_coupling_cell_t *c = &m.cell[i][j];
            if (!first) o += snprintf(json + o, sizeof(json) - o, ",");
            first = false;
            if (c->valid) {
                o += snprintf(json + o, sizeof(json) - o,
                    "{\"i\":%u,\"j\":%u,\"valid\":true,\"k\":%.3f,\"tau_s\":%.1f,\"dead_time_s\":%.1f}", i, j,
                    (double)c->model.k_gain_c_per_duty, (double)c->model.tau_s, (double)c->model.dead_time_s);
            } else {
                o += snprintf(json + o, sizeof(json) - o, "{\"i\":%u,\"j\":%u,\"valid\":false}", i, j);
            }
            if (o >= sizeof(json) - 1) break;
        }
    }
    /* The cells loop above breaks on near-overflow but leaves `o` holding
     * snprintf's would-be length, which can exceed the buffer; every append
     * from here on computes `sizeof(json) - o` and would wrap that into a
     * huge size_t. Clamp once. (Sized so this cannot trigger in practice at
     * MAX31856_CHANNEL_COUNT=3 -- it is a backstop, not a code path.) */
    if (o > sizeof(json) - 1) o = sizeof(json) - 1;

    o += snprintf(json + o, sizeof(json) - o, "]");

    /* TODO.md 6A.5(c): the RGA rides along on the same response as the
     * matrix it is derived from, rather than getting its own endpoint --
     * one fetch, and the page can never render a Lambda computed from a
     * different snapshot of K than the table above it is showing.
     *
     * When it can't be computed the response says so *and why* (empty
     * matrix, hole in it, singular K), because "no RGA" has several
     * distinct causes and only one of them ("nothing tuned yet") is
     * expected: the others are telling the operator something about their
     * kiln. `zones` maps Lambda's rows back to real zone numbers -- the
     * sub-block used is not necessarily zones 0..n-1. */
    autotune_rga_t rga;
    autotune_engine_compute_rga(&m, &rga);
    if (rga.valid) {
        o += snprintf(json + o, sizeof(json) - o, ",\"rga\":{\"available\":true,\"n\":%d,\"det\":%.4g,\"zones\":[",
                      rga.n, (double)rga.determinant);
        for (int a = 0; a < rga.n; a++) {
            o += snprintf(json + o, sizeof(json) - o, "%s%u", a ? "," : "", (unsigned)rga.zone_index[a]);
        }
        o += snprintf(json + o, sizeof(json) - o, "],\"lambda\":[");
        for (int a = 0; a < rga.n; a++) {
            o += snprintf(json + o, sizeof(json) - o, "%s[", a ? "," : "");
            for (int b = 0; b < rga.n; b++) {
                o += snprintf(json + o, sizeof(json) - o, "%s%.4f", b ? "," : "", (double)rga.lambda[a][b]);
            }
            o += snprintf(json + o, sizeof(json) - o, "]");
        }
        o += snprintf(json + o, sizeof(json) - o, "]}");
    } else {
        char rga_reason[sizeof(rga.invalid_reason) * 2 + 1];
        json_escape(rga.invalid_reason, rga_reason, sizeof(rga_reason));
        o += snprintf(json + o, sizeof(json) - o, ",\"rga\":{\"available\":false,\"code\":%d,\"reason\":\"%s\"}",
                      (int)rga.status, rga_reason);
    }

    o += snprintf(json + o, sizeof(json) - o, "}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o < sizeof(json) ? o : sizeof(json) - 1);
}

static esp_err_t autotune_start_post_handler(httpd_req_t *req)
{
    /* Raised from 64 when the relay method arrived: its form carries
     * method/setpoint_c/relay_d/relay_h/rule on top of zone. */
    if (req->content_len <= 0 || req->content_len > 192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[193];
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

    char zone_val[8], duty_val[16], method_val[12], sp_val[16], d_val[16], h_val[16], rule_val[20];
    int zone_len = http_form_find_field(body, "zone", zone_val, sizeof(zone_val));
    int duty_len = http_form_find_field(body, "step_duty", duty_val, sizeof(duty_val));
    int method_len = http_form_find_field(body, "method", method_val, sizeof(method_val));
    long zone = (zone_len > 0) ? strtol(zone_val, NULL, 10) : -1;
    float step_duty = (duty_len > 0) ? strtof(duty_val, NULL) : 0.5f;
    if (zone_len <= 0 || zone < 0 || zone > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone missing or invalid");
        return ESP_OK;
    }

    /* method is optional and defaults to the step test -- TODO.md 6A.4's
     * recommendation is step-test-first, and the relay method oscillates the
     * chamber on purpose. A caller that omits the field, or an older client
     * that has never heard of it, must get the gentler test. Only the exact
     * string "relay" opts in; anything else is refused rather than quietly
     * falling back, so a typo cannot silently change which test runs. */
    /* Parameter rejections take the same {"ok":false,"error":...} route the
     * engine's own refusals do, rather than httpd_resp_send_err()'s HTML: the
     * page parses this response as JSON and shows `error` verbatim, so an
     * error sent the other way would reach the operator as a silent failure. */
    char err_msg[128] = "";
    bool params_ok = true;
    bool started = false;
    bool want_relay = false;
    if (method_len > 0 && strcmp(method_val, "step") != 0) {
        if (strcmp(method_val, "relay") == 0) {
            want_relay = true;
        } else {
            params_ok = false;
            snprintf(err_msg, sizeof(err_msg), "method must be \"step\" or \"relay\"");
        }
    }

    if (params_ok && want_relay) {
        int sp_len = http_form_find_field(body, "setpoint_c", sp_val, sizeof(sp_val));
        int d_len = http_form_find_field(body, "relay_d", d_val, sizeof(d_val));
        int h_len = http_form_find_field(body, "relay_h", h_val, sizeof(h_val));
        int rule_len = http_form_find_field(body, "rule", rule_val, sizeof(rule_val));
        /* 0 for d/h means "engine default" -- see autotune_engine.h. */
        float setpoint_c = (sp_len > 0) ? strtof(sp_val, NULL) : 0.0f;
        float relay_d = (d_len > 0) ? strtof(d_val, NULL) : 0.0f;
        float relay_h = (h_len > 0) ? strtof(h_val, NULL) : 0.0f;
        /* Tyreus-Luyben is the default rule, not Ziegler-Nichols: ZN targets
         * quarter-amplitude decay, i.e. it is designed to leave the loop
         * oscillating (pid_autotune.h). TL is roughly half the gain with a far
         * longer integral time, which is the only one of the two worth having
         * as a default on something that fires ware. */
        autotune_rule_t rule = AUTOTUNE_RULE_TYREUS_LUYBEN;
        if (rule_len > 0 && strcmp(rule_val, "zn") == 0) {
            rule = AUTOTUNE_RULE_ZIEGLER_NICHOLS;
        } else if (rule_len > 0 && strcmp(rule_val, "tl") != 0) {
            params_ok = false;
            snprintf(err_msg, sizeof(err_msg), "rule must be \"tl\" or \"zn\"");
        }
        if (sp_len <= 0) {
            /* No default is possible here and inventing one would be the wrong
             * kind of convenience: the operator is choosing the temperature the
             * kiln will be held oscillating at. */
            params_ok = false;
            snprintf(err_msg, sizeof(err_msg), "relay method requires setpoint_c");
        }
        if (params_ok) {
            started = autotune_engine_run_relay((uint8_t)zone, setpoint_c, relay_d, relay_h, rule, err_msg,
                                                sizeof(err_msg));
        }
    } else if (params_ok) {
        started = autotune_engine_run((uint8_t)zone, step_duty, err_msg, sizeof(err_msg));
    }

    if (!started) {
        char json[192];
        char err_escaped[128 * 2 + 1];
        json_escape(err_msg, err_escaped, sizeof(err_escaped));
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", err_escaped);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t autotune_abort_post_handler(httpd_req_t *req)
{
    autotune_engine_abort("aborted from web UI");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t autotune_accept_post_handler(httpd_req_t *req)
{
    if (!autotune_engine_accept()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no completed autotune result to accept");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* Streamed the same way history_csv_get_handler() is, for the same reason
 * (see that function's comment) -- a single ~69KB one-shot buffer was tried
 * first here too and is exactly the pattern that turned out to risk
 * ESP_ERR_NO_MEM against this board's actual runtime-free heap. */
#define AUTOTUNE_CSV_BATCH 128u

static esp_err_t autotune_trace_csv_get_handler(httpd_req_t *req)
{
    autotune_sample_t *batch = malloc(sizeof(autotune_sample_t) * AUTOTUNE_CSV_BATCH);
    char *line = malloc(64);
    if (!batch || !line) {
        free(batch);
        free(line);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"autotune_trace.csv\"");

    int n = snprintf(line, 64, "elapsed_s,measurement_c\n");
    esp_err_t err = httpd_resp_send_chunk(req, line, n > 0 ? (size_t)n : 0);

    size_t start = 0;
    while (err == ESP_OK) {
        size_t got = autotune_engine_get_trace(batch, start, AUTOTUNE_CSV_BATCH);
        if (got == 0) break;
        for (size_t i = 0; i < got && err == ESP_OK; i++) {
            n = snprintf(line, 64, "%.1f,%.2f\n", (double)batch[i].t_s, (double)batch[i].measurement_c);
            err = httpd_resp_send_chunk(req, line, n > 0 ? (size_t)n : 0);
        }
        start += got;
        if (got < AUTOTUNE_CSV_BATCH) break;
    }
    free(batch);
    free(line);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t dashboard_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                               SafetyLinkClass *safety_or_null)
{
    s_dash.io = io_or_null;
    s_dash.thermo_bus = thermo_bus_or_null;
    s_dash.safety = safety_or_null;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t relay_uri = {
        .uri = "/api/relay", .method = HTTP_POST, .handler = relay_post_handler,
    };
    static const httpd_uri_t exec_status_uri = {
        .uri = "/api/profile_exec", .method = HTTP_GET, .handler = profile_exec_status_get_handler,
    };
    static const httpd_uri_t exec_start_uri = {
        .uri = "/api/profile_exec/start", .method = HTTP_POST, .handler = profile_exec_start_post_handler,
    };
    static const httpd_uri_t exec_stop_uri = {
        .uri = "/api/profile_exec/stop", .method = HTTP_POST, .handler = profile_exec_stop_post_handler,
    };
    static const httpd_uri_t exec_pause_uri = {
        .uri = "/api/profile_exec/pause", .method = HTTP_POST, .handler = profile_exec_pause_post_handler,
    };
    static const httpd_uri_t exec_resume_uri = {
        .uri = "/api/profile_exec/resume", .method = HTTP_POST, .handler = profile_exec_resume_post_handler,
    };
    static const httpd_uri_t exec_ack_last_run_uri = {
        .uri = "/api/profile_exec/ack_last_run", .method = HTTP_POST,
        .handler = profile_exec_ack_last_run_post_handler,
    };
    static const httpd_uri_t control_status_uri = {
        .uri = "/api/control", .method = HTTP_GET, .handler = control_status_get_handler,
    };
    static const httpd_uri_t history_csv_uri = {
        .uri = "/api/history.csv", .method = HTTP_GET, .handler = history_csv_get_handler,
    };
    static const httpd_uri_t autotune_status_uri = {
        .uri = "/api/autotune", .method = HTTP_GET, .handler = autotune_status_get_handler,
    };
    static const httpd_uri_t autotune_matrix_uri = {
        .uri = "/api/autotune/matrix", .method = HTTP_GET, .handler = autotune_matrix_get_handler,
    };
    static const httpd_uri_t autotune_start_uri = {
        .uri = "/api/autotune/start", .method = HTTP_POST, .handler = autotune_start_post_handler,
    };
    static const httpd_uri_t autotune_abort_uri = {
        .uri = "/api/autotune/abort", .method = HTTP_POST, .handler = autotune_abort_post_handler,
    };
    static const httpd_uri_t autotune_accept_uri = {
        .uri = "/api/autotune/accept", .method = HTTP_POST, .handler = autotune_accept_post_handler,
    };
    static const httpd_uri_t autotune_trace_uri = {
        .uri = "/api/autotune/trace.csv", .method = HTTP_GET, .handler = autotune_trace_csv_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &relay_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/relay) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_stop_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec/stop) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_pause_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec/pause) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_resume_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec/resume) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_ack_last_run_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_exec/ack_last_run) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &control_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/control) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &history_csv_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/history.csv) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_matrix_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune/matrix) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_abort_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune/abort) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_accept_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune/accept) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_trace_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/autotune/trace.csv) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "dashboard API up (io_ready=%d, thermo_ready=%d, safety_ready=%d)", s_dash.io != NULL,
             s_dash.thermo_bus != NULL && s_dash.thermo_bus->initialized, s_dash.safety != NULL);
    return ESP_OK;
}
