#include "dashboard_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp -- unit_pref_post_handler's "fahrenheit"/"celsius" match */

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "autotune_engine.h"
#include "boot_button.h" /* boot_button_ota_bypass_active()/_remaining_ms() -- see the GET /api/status fields below */
#include "heat_interlock.h" /* HEAT_INTERLOCK_REASON_MAX -- see the ERR_UPDATING case below */
#include "http_form.h"
#include "kiln_io_owner.h"
#include "nvs_report.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- see the ERR_UPDATING case below */
#include "profile_executor.h"
#include "profile_feasibility.h" /* profile_feasibility_plan_curve() -- the duration model, see below */
#include "profiles_http.h" /* profiles_http_get() -- /api/profile_plan, see that handler below */
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "sim_backend.h"
#include "uart_task_ids.h"
#include "unit_pref.h"
#include "watchdog_cfg.h"
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

/* Forward declaration: json_escape() is defined further down (near the
 * profile-executor handlers, its original call site) but status_get_handler()
 * above that point now needs it too for the fw_version/fw_build strings
 * appended below -- a plain prototype here is simpler than reordering every
 * function between the two, and this is a static, single-TU helper so a
 * header declaration would be overkill. */
static void json_escape(const char *src, char *out, size_t out_cap);

/* esp_reset_reason_t -> short static string, for the diagnostics page's
 * "why did this boot happen" field (UI_PLAN.md section 5). Verified against
 * this project's installed esp_system.h (ESP-IDF's own enum, unchanged
 * across the S3 targets this board uses) -- every named value in
 * esp_reset_reason_t has a case here, so "unknown" only fires against a
 * future IDF adding a new reason this file hasn't been updated for. */
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

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
        /* The driver's r->stale answers "was there a new conversion this
         * poll" -- honest at that layer, but useless as a UI signal, since
         * polling faster than the part converts sets it on a value that is a
         * fraction of a second old. What a user needs to know is whether the
         * NUMBER is too old to trust, which is the age against one shared
         * threshold. See KILN_TEMP_STALE_AGE_MS in MAX31856.h. */
        out->channels[i].age_ms = r->age_ms;
        out->channels[i].stale = (r->age_ms >= KILN_TEMP_STALE_AGE_MS);
    }

    /* safety_ready must equal dashboard_safety_ready() -- see that function's
     * doc comment (dashboard_http.h) for the owner-reported bench bug this
     * fixes: s_dash.safety != NULL only means the driver object was
     * constructed, not that the Pico is actually answering, and the old code
     * reported the former. Defaults to false/ESP_FAIL here so a never-
     * initialized or currently-failing link reads not-ready, never a stale
     * "true" left over from init. */
    esp_err_t safety_status_err = ESP_FAIL;
    bool safety_link_up = false;

    /* ROADMAP.md M6: safety/enclosure temperature straight from the safety
     * link's cache -- see dashboard_http.h's field comment for the wire
     * source and why power_w has no data yet. safety_link_get_status()
     * itself is null-tolerant on a never-initialized link (returns an error,
     * leaving out->safety_temp_c/enclosure_temp_c at the memset(0) above,
     * caught by the isnan() check anyway since a plain 0.0f would otherwise
     * read as a real, very cold reading). */
    if (s_dash.safety) {
        safety_link_status_t sl;
        safety_status_err = safety_link_get_status(s_dash.safety, &sl);
        if (safety_status_err == ESP_OK) {
            safety_link_up = sl.link_up;
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

            /* ROADMAP.md M5: DIAG (Frame B) / TRIP_EVENT (Frame D) now have a
             * decode path -- see safety_link.h's field comments for what
             * each of these means. Straight passthrough, same convention as
             * the temperature/power fields above. */
            out->diag_ever_received = sl.diag_ever_received;
            out->diag_trip_reason = sl.diag_trip_reason;
            out->diag_warn_mask = sl.diag_warn_mask;
            out->diag_trip_mask = sl.diag_trip_mask;
            out->diag_state = sl.diag_state;
            out->diag_age_ms = sl.age_ms;
            out->diag_context_age_100ms = sl.diag_context_age_100ms;
            out->diag_context_frames_ok = sl.diag_context_frames_ok;
            out->diag_context_frames_bad = sl.diag_context_frames_bad;
            out->diag_tx_frames_dropped = sl.diag_tx_frames_dropped;

            out->trip_event_ever_received = sl.trip_event_ever_received;
            out->trip_reason = sl.trip_reason;
            out->trip_event_age_ms = sl.trip_event_age_ms;
            out->trip_safety_tc_c = sl.trip_safety_tc_c;
            out->trip_deciding_threshold = sl.trip_deciding_threshold;
        }
        out->safety_ready = dashboard_safety_ready(true, safety_status_err, safety_link_up);

        /* TODO.md 9.0's deferred "GUI names both versions" item -- this
         * firmware's own protocol number is always known regardless of link
         * state, the peer's is whatever the last FW_VERSION frame said (or
         * unknown, on this no-Pico bench build). */
        out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;
        (void)safety_link_get_peer_version_status(s_dash.safety, &out->link_version_known,
                                                    &out->link_version_compatible,
                                                    &out->peer_protocol_version,
                                                    &out->peer_min_compatible);

        /* TODO.md owner-report item 5: safety processor's own build identity
         * + config CRC, straight from safety_link.c's FW_VERSION parse.
         * Commit/datetime come back as explicit-length, non-NUL-terminated
         * byte buffers (safety_link.h's own convention) -- copy into this
         * struct's NUL-terminated char[] here, once, so every renderer
         * (JSON below, any future LCD page) gets an ordinary C string
         * instead of re-deriving the length itself. */
        uint8_t commit_buf[64];
        uint8_t commit_len = 0;
        uint8_t datetime_buf[32];
        uint8_t datetime_len = 0;
        (void)safety_link_get_peer_build_status(s_dash.safety, &out->safety_build_known,
                                                 &out->safety_build_dirty, commit_buf, &commit_len,
                                                 datetime_buf, &datetime_len,
                                                 &out->safety_config_version,
                                                 &out->safety_config_crc);
        if (commit_len > sizeof(out->safety_build_commit) - 1u) {
            commit_len = sizeof(out->safety_build_commit) - 1u;
        }
        memcpy(out->safety_build_commit, commit_buf, commit_len);
        out->safety_build_commit[commit_len] = '\0';
        if (datetime_len > sizeof(out->safety_build_datetime) - 1u) {
            datetime_len = sizeof(out->safety_build_datetime) - 1u;
        }
        memcpy(out->safety_build_datetime, datetime_buf, datetime_len);
        out->safety_build_datetime[datetime_len] = '\0';
    } else {
        out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;
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

    /* UI_PLAN.md section 5's missing field set (see dashboard_http.h's
     * struct comment above these fields for the full rationale). Every read
     * here is cheap and side-effect-free -- esp_app_get_description() reads
     * a const struct baked into the app image, esp_reset_reason()/
     * esp_timer_get_time() are simple register/RTC reads, and the
     * heap_caps_get_*() calls are the same O(free-list-length) walk
     * ui_page_diagnostics.c's own refresh_cb() already performs on the same
     * 2-second LCD tick, so doing it again here on a browser's poll cadence
     * is not a new cost profile for this firmware. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    out->fw_version_known = (app_desc != NULL);
    if (app_desc) {
        /* esp_app_desc_t::version/date/time are themselves fixed-size,
         * NUL-terminated char arrays (esp_app_desc.h) -- snprintf still used
         * defensively rather than strcpy, matching this file's json_escape()
         * callers' general "never trust a fixed-size field to already be
         * exactly what its type promises" habit. */
        snprintf(out->fw_version, sizeof(out->fw_version), "%s", app_desc->version);
        snprintf(out->fw_build, sizeof(out->fw_build), "%s %s", app_desc->date, app_desc->time);
    } else {
        out->fw_version[0] = '\0';
        out->fw_build[0] = '\0';
    }

    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->reset_reason = reset_reason_name(esp_reset_reason());

    out->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->heap_internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    /* MALLOC_CAP_SPIRAM reads back as a real 0 (not an error) on a board
     * built without PSRAM enabled -- see dashboard_http.h's field comment
     * and ui_page_diagnostics.c's refresh_cb() for the same call and the
     * same "0 KB is honest, not invented" reasoning. */
    out->heap_spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->heap_spiram_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->heap_spiram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    /* 2026-08-21: the shared display-unit preference (unit_pref.c) -- see
     * dashboard_http.h's field comment. unit_pref_get() is O(1) RAM-only, so
     * this costs nothing extra on either the HTTP or LCD poll path. */
    out->temp_unit = unit_pref_get();
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    /* Bumped from 1400 to make room for the firmware version/build/uptime/
     * reset-reason/heap block appended below (UI_PLAN.md section 5) --
     * worst case for that block is under 300 bytes (two ~32-byte escaped
     * strings plus ~8 numeric fields), so 1400 -> 1700 is comfortably over
     * the new worst case (~1450 bytes with 3 zones/channels and every
     * optional block populated) rather than trimmed to the edge. */
    char json[1700];
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
            /* null, not a sentinel integer: "we have never had a good
             * conversion from this channel" is not an age, and the same
             * null-rather-than-0 convention the safety/board temps already
             * use keeps a client from plotting UINT32_MAX as a number. */
            char age_buf[16];
            if (r->age_ms == MAX31856_READING_AGE_UNKNOWN) {
                snprintf(age_buf, sizeof(age_buf), "null");
            } else {
                snprintf(age_buf, sizeof(age_buf), "%lu", (unsigned long)r->age_ms);
            }
            APPEND(
                "%s{\"channel\":%u,\"temp_c\":%.2f,\"cj_c\":%.2f,\"valid\":%s,\"fault_status\":%u,"
                "\"spi_failed\":%s,\"stale\":%s,\"age_ms\":%s}",
                i == 0 ? "" : ",", r->channel, (double)r->temp_c, (double)r->cj_c, r->valid ? "true" : "false",
                r->fault_status, r->spi_failed ? "true" : "false", r->stale ? "true" : "false",
                age_buf);
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

    /* TODO.md 9.0's deferred "GUI names both versions and which one is
     * older" item. self_protocol_version is always known; peer fields are
     * null until the Pico has announced itself (same convention as
     * safety_temp_c/power_w above). */
    APPEND(",\"self_protocol_version\":%u", (unsigned)ds.self_protocol_version);
    APPEND(",\"link_version_known\":%s", ds.link_version_known ? "true" : "false");
    if (ds.link_version_known) {
        APPEND(",\"link_version_compatible\":%s", ds.link_version_compatible ? "true" : "false");
        APPEND(",\"peer_protocol_version\":%u", (unsigned)ds.peer_protocol_version);
    } else {
        APPEND(",\"link_version_compatible\":null");
        APPEND(",\"peer_protocol_version\":null");
    }

    /* ROADMAP.md M5 -- SAFETY_CMD_DIAG (Frame B) and SAFETY_CMD_TRIP_EVENT
     * (Frame D), same null-until-received convention as everything else on
     * this endpoint. diag_trip_mask/diag_warn_mask are bitmasks (one bit per
     * guard, SaftyFW's safety_guards.h) -- left as raw integers rather than
     * decoded here, same as diag_trip_reason/trip_reason, since this
     * firmware has no guard-name table of its own to decode them against. */
    APPEND(",\"diag_ever_received\":%s", ds.diag_ever_received ? "true" : "false");
    if (ds.diag_ever_received) {
        APPEND(",\"diag_trip_reason\":%u", (unsigned)ds.diag_trip_reason);
        APPEND(",\"diag_warn_mask\":%u", (unsigned)ds.diag_warn_mask);
        APPEND(",\"diag_trip_mask\":%u", (unsigned)ds.diag_trip_mask);
        APPEND(",\"diag_state\":%u", (unsigned)ds.diag_state);
        APPEND(",\"diag_age_ms\":%u", (unsigned)ds.diag_age_ms);
        APPEND(",\"diag_context_age_100ms\":%u", (unsigned)ds.diag_context_age_100ms);
        APPEND(",\"diag_context_frames_ok\":%lu", (unsigned long)ds.diag_context_frames_ok);
        APPEND(",\"diag_context_frames_bad\":%lu", (unsigned long)ds.diag_context_frames_bad);
        APPEND(",\"diag_tx_frames_dropped\":%lu", (unsigned long)ds.diag_tx_frames_dropped);
    }

    APPEND(",\"trip_event_ever_received\":%s", ds.trip_event_ever_received ? "true" : "false");
    if (ds.trip_event_ever_received) {
        APPEND(",\"trip_reason\":%u", (unsigned)ds.trip_reason);
        APPEND(",\"trip_event_age_ms\":%lu", (unsigned long)ds.trip_event_age_ms);
        APPEND(",\"trip_safety_tc_c\":%.1f", (double)ds.trip_safety_tc_c);
        APPEND(",\"trip_deciding_threshold\":%.1f", (double)ds.trip_deciding_threshold);
    }

    /* TODO.md owner-report item 5: the safety processor's own build identity
     * + config CRC (CommonFW/docs/LINK_PROTOCOL.md sec 7: "Show the safety
     * processor's own build identity, not just the ESP's"), null until a
     * FW_VERSION frame has parsed far enough to report it -- see
     * dashboard_http.h's safety_build_known comment. Escaped for the same
     * "operator/build-time string, still worth escaping" reason
     * fw_version/fw_build are above. */
    APPEND(",\"safety_build_known\":%s", ds.safety_build_known ? "true" : "false");
    if (ds.safety_build_known) {
        char commit_esc[sizeof(ds.safety_build_commit) * 2 + 1];
        char datetime_esc[sizeof(ds.safety_build_datetime) * 2 + 1];
        json_escape(ds.safety_build_commit, commit_esc, sizeof(commit_esc));
        json_escape(ds.safety_build_datetime, datetime_esc, sizeof(datetime_esc));
        APPEND(",\"safety_build_dirty\":%s", ds.safety_build_dirty ? "true" : "false");
        APPEND(",\"safety_build_commit\":\"%s\"", commit_esc);
        APPEND(",\"safety_build_datetime\":\"%s\"", datetime_esc);
        APPEND(",\"safety_config_version\":%u", (unsigned)ds.safety_config_version);
        APPEND(",\"safety_config_crc\":%u", (unsigned)ds.safety_config_crc);
    } else {
        APPEND(",\"safety_build_dirty\":null");
        APPEND(",\"safety_build_commit\":null");
        APPEND(",\"safety_build_datetime\":null");
        APPEND(",\"safety_config_version\":null");
        APPEND(",\"safety_config_crc\":null");
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

    /* UI_PLAN.md section 5's genuinely-new field set for the web diagnostics
     * page -- firmware version/build/uptime/reset reason/heap, none of which
     * were on this endpoint before this pass (see dashboard_http.h's struct
     * comment for the full "why" and the largest-free-block margin note).
     * fw_version/fw_build are escaped even though they come from this same
     * firmware's own embedded esp_app_desc_t (not untrusted network input):
     * a version string is still operator-supplied at build time (git tag/
     * describe output can contain arbitrary characters), and json_escape()
     * is cheap enough that "trust the build" is not a saving worth the risk
     * of ever emitting invalid JSON from a stray quote in a tag name. */
    {
        char fw_version_esc[sizeof(ds.fw_version) * 2 + 1];
        char fw_build_esc[sizeof(ds.fw_build) * 2 + 1];
        json_escape(ds.fw_version, fw_version_esc, sizeof(fw_version_esc));
        json_escape(ds.fw_build, fw_build_esc, sizeof(fw_build_esc));
        APPEND(",\"fw_version_known\":%s", ds.fw_version_known ? "true" : "false");
        APPEND(",\"fw_version\":\"%s\"", fw_version_esc);
        APPEND(",\"fw_build\":\"%s\"", fw_build_esc);
    }
    APPEND(",\"uptime_s\":%lu", (unsigned long)ds.uptime_s);
    APPEND(",\"reset_reason\":\"%s\"", ds.reset_reason);
    APPEND(",\"heap_internal\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu}",
           (unsigned long)ds.heap_internal_free, (unsigned long)ds.heap_internal_largest_free_block,
           (unsigned long)ds.heap_internal_min_free);
    APPEND(",\"heap_spiram\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu}",
           (unsigned long)ds.heap_spiram_free, (unsigned long)ds.heap_spiram_largest_free_block,
           (unsigned long)ds.heap_spiram_min_free);

    /* 2026-08-21, ROADMAP.md "a real shared temperature-unit setting":
     * ADDITIVE field -- every field above this line is unchanged, so an
     * older main_page.html/app.js that has never heard of "temp_unit" keeps
     * working exactly as before. "C" or "F", matching unit_pref_suffix() --
     * a short string rather than a bare 0/1 so a client reading this JSON by
     * hand (or a future integration) does not have to know this firmware's
     * internal enum encoding. DISPLAY-ONLY: nothing above this line (every
     * temp_c/cj_c/safety_temp_c/etc.) is itself converted -- those stay
     * Celsius; a client that wants to *show* Fahrenheit converts using this
     * field, the same boundary point unit_pref_convert() enforces on the LCD
     * side. */
    APPEND(",\"temp_unit\":\"%s\"", unit_pref_suffix(ds.temp_unit));

    /* watchdog_cfg.h -- a board running with this safety default disabled
     * must say so somewhere always visible, not only at the moment of
     * starting a firing (see the extra confirmation in
     * profile_exec_start_post_handler() below and main_page.html/
     * ui_page_home.c's own dialogs). */
    APPEND(",\"watchdog_panic_disabled\":%s", watchdog_cfg_panic_disabled() ? "true" : "false");

    /* boot_button.h -- the BOOT-button OTA-auth bypass window. Same
     * always-visible-in-status reasoning as watchdog_panic_disabled just
     * above: a board currently reachable by anyone on the network with NO
     * OTA password check must say so everywhere this status is read, not
     * only at the moment a request happens to hit the bypassed check. */
    APPEND(",\"boot_button_bypass_active\":%s", boot_button_ota_bypass_active() ? "true" : "false");
    APPEND(",\"boot_button_bypass_remaining_s\":%lu",
           (unsigned long)(boot_button_bypass_remaining_ms() / 1000u));

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
        /* Same dashboard_safety_ready() contract as dashboard_get_status()
         * above -- see that function's call site, or dashboard_http.h's doc
         * comment, for the owner-reported false-positive this fixes. */
        esp_err_t safety_status_err = ESP_FAIL;
        bool safety_link_up = false;
        if (s_dash.safety) {
            safety_link_status_t sl;
            safety_status_err = safety_link_get_status(s_dash.safety, &sl);
            if (safety_status_err == ESP_OK) {
                safety_link_up = sl.link_up;
            }
        }
        *out_safety_ready = dashboard_safety_ready(s_dash.safety != NULL, safety_status_err, safety_link_up);
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
 * status.
 *
 * 2026-08-19 (TODO.md 10.14 Phase 1): the ownership/safety-fault check and
 * the actual write both moved into kiln_io_owner.c -- this used to
 * reimplement relay_authority_manual_blocked_by_owner()/
 * relay_authority_on_blocked() independently of uart_bridge.c's identical
 * copy, which is exactly the "two copies that can drift" problem that pass
 * closed. This function is now a thin translation from
 * kiln_io_owner_relay_result_t to dashboard_relay_result_t. Also closes a
 * real race: this used to call kiln_io_set_relay() directly, with no
 * coordination against uart_bridge.c's io_bridge_task or
 * profile_executor.c's control loop doing the same. */
dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)
{
    if (!s_dash.io) {
        return DASHBOARD_RELAY_ERR_NO_BOARD;
    }

    kiln_io_owner_relay_result_t rr = kiln_io_owner_command_set_relay(relay_index, on, out_safety_sources);
    switch (rr) {
    case KILN_IO_OWNER_RELAY_OK:
        return DASHBOARD_RELAY_OK;
    case KILN_IO_OWNER_RELAY_ERR_RANGE:
        return DASHBOARD_RELAY_ERR_RANGE;
    case KILN_IO_OWNER_RELAY_ERR_OWNED:
        ESP_LOGW(TAG, "dashboard: relay %u refused -- owned by a running profile", (unsigned)relay_index);
        return DASHBOARD_RELAY_ERR_OWNED;
    case KILN_IO_OWNER_RELAY_ERR_SAFETY:
        ESP_LOGW(TAG, "dashboard: relay %u ON refused -- safety fault sources 0x%02X", (unsigned)relay_index,
                 out_safety_sources ? (unsigned)*out_safety_sources : 0u);
        return DASHBOARD_RELAY_ERR_SAFETY;
    case KILN_IO_OWNER_RELAY_ERR_UPDATING:
        /* 2026-08-21: distinct from ERR_SAFETY above -- see
         * dashboard_http.h's DASHBOARD_RELAY_ERR_UPDATING comment and
         * kiln_io_owner.c's relay_on_blocked() for why this is not a fault.
         * Re-derive the exact reason string relay_on_blocked() already
         * logged once (kiln_io_owner.c does not hand it back through
         * kiln_io_owner_relay_result_t, only the enum value) rather than
         * inventing a second, possibly-drifting message here. */
        {
            char reason[HEAT_INTERLOCK_REASON_MAX];
            if (ota_http_heat_blocked_by_update(reason, sizeof(reason))) {
                ESP_LOGW(TAG, "dashboard: relay %u ON refused -- %s", (unsigned)relay_index, reason);
            } else {
                /* Should not happen -- kiln_io_owner.c only returns this
                 * result when that same check just returned true -- but the
                 * update could in principle finish between that check and
                 * this one, so fall back to a still-accurate generic reason
                 * rather than printing an empty/stale string. */
                ESP_LOGW(TAG, "dashboard: relay %u ON refused -- firmware update in progress",
                         (unsigned)relay_index);
            }
        }
        return DASHBOARD_RELAY_ERR_UPDATING;
    case KILN_IO_OWNER_RELAY_ERR_IO_FAIL:
    case KILN_IO_OWNER_RELAY_ERR_TIMEOUT:
    default:
        ESP_LOGW(TAG, "dashboard: kiln_io_owner_command_set_relay(%u) failed (result %d)",
                 (unsigned)relay_index, (int)rr);
        return DASHBOARD_RELAY_ERR_IO_FAIL;
    }
}

/* Turns a DASHBOARD_RELAY_ERR_SAFETY refusal's raw SAFETY_FAULT_SRC_* bitmask
 * (safety_link.h) into the specific, actionable sentence relay_post_handler()
 * below hands back in the HTTP body -- the owner's report (TODO.md/ROADMAP.md
 * 2026-08-21 "manual relays refuse silently") was that "blocked by safety
 * fault" alone reads as a dead button, not a safety refusal, and that this
 * board's actual bench condition (safety link never comes up -- the
 * optocouplers are non-functional, so safety_link.c's link_up latches 0
 * forever and SAFETY_FAULT_SRC_SAFETY_LINK is asserted permanently) needs to
 * be named, not left as a bit an operator has no way to decode. Checked in
 * the same priority relay_authority_on_blocked()'s bit values imply (any bit
 * set blocks -- see that function) but SAFETY_LINK is called out first and by
 * name because it is the chronic, expected-on-this-bench condition every
 * other bit is a rarer, acute one; a caller with more than one bit set still
 * gets a truthful single sentence rather than every bit spelled out, matching
 * this file's other one-reason-at-a-time refusal messages (ERR_UPDATING
 * above).
 *
 * Deliberately does NOT suggest disabling CONFIG_KILNCTL_SX1509_RELAYS_OFF_
 * ON_LINK_LOSS -- that Kconfig default (Kconfig: "Drop all relays when the
 * PC link goes away") exists specifically so a crashed/unplugged controller
 * can never leave a kiln element energised, and turning it off is a
 * deliberate bench-only escape hatch the owner has not asked for here. This
 * message names it only so the operator understands WHY relays keep bouncing
 * back off if they were ever momentarily forced on some other way, not as an
 * invitation to flip it. */
static void relay_safety_reason(uint32_t sources, char *buf, size_t buf_len)
{
    if (sources & SAFETY_FAULT_SRC_SAFETY_LINK) {
        snprintf(buf, buf_len,
                 "safety link is down (no safety processor has ever answered on this board) -- "
                 "manual relay-ON is refused while the link is down, and "
                 "CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS additionally forces every relay "
                 "off on this same condition so a crashed controller can never leave an element on");
    } else if (sources & SAFETY_FAULT_SRC_PC_LINK) {
        snprintf(buf, buf_len, "PC control link is down");
    } else if (sources & SAFETY_FAULT_SRC_THERMO) {
        snprintf(buf, buf_len, "a thermocouple has faulted");
    } else if (sources & SAFETY_FAULT_SRC_THERMAL_SANITY) {
        snprintf(buf, buf_len, "a zone failed its thermal sanity check");
    } else if (sources & SAFETY_FAULT_SRC_MANUAL) {
        snprintf(buf, buf_len, "a fault was manually asserted (SAFETY_CMD_SET_FAULT_OUT)");
    } else if (sources & SAFETY_FAULT_SRC_APP) {
        snprintf(buf, buf_len, "the application asserted a safety fault");
    } else {
        /* Should not happen -- ERR_SAFETY is only returned when
         * relay_authority_on_blocked() found at least one bit set -- but
         * still an honest, non-empty sentence rather than a blank body if it
         * somehow does. */
        snprintf(buf, buf_len, "blocked by an unspecified safety fault (sources=0x%02X)", (unsigned)sources);
    }
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
     * translates its result to an HTTP status. out_safety_sources is a real
     * pointer (not NULL, as this used to pass) so a DASHBOARD_RELAY_ERR_SAFETY
     * result below can be decoded into the specific reason relay_safety_reason()
     * builds, instead of the flat "blocked by safety fault" text that read as
     * a dead button to an operator who has no way to tell a refusal from a
     * silently-ignored click (2026-08-21 owner report). */
    uint32_t safety_sources = 0;
    switch (dashboard_set_relay((uint8_t)relay, want_on, &safety_sources)) {
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
        {
            char reason[320];
            relay_safety_reason(safety_sources, reason, sizeof(reason));
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, reason);
        }
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_UPDATING:
        /* 2026-08-21: distinct wording from ERR_SAFETY above -- see
         * dashboard_http.h's DASHBOARD_RELAY_ERR_UPDATING comment. Re-derive
         * the specific reason (which processor is updating) the same way
         * dashboard_set_relay()'s own ERR_UPDATING case does, rather than a
         * flat string, so a dashboard operator sees the same detail the
         * server log already recorded. */
        {
            char reason[HEAT_INTERLOCK_REASON_MAX];
            if (ota_http_heat_blocked_by_update(reason, sizeof(reason))) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, reason);
            } else {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "firmware update in progress");
            }
        }
        return ESP_OK;
    case DASHBOARD_RELAY_ERR_IO_FAIL:
    default:
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "relay command failed");
        return ESP_OK;
    }
}

/* ---- Unit preference (ROADMAP.md, 2026-08-21) -----------------------------
 * POST /api/unit_pref -- the write side of the shared display-unit setting
 * (unit_pref.c). Same application/x-www-form-urlencoded, bounded-body-then-
 * validate-then-commit shape every other settings POST in this codebase uses
 * (relay_post_handler above, zones_http.c's zones_post_handler). DISPLAY-ONLY:
 * this never touches a stored/transmitted temperature anywhere else -- see
 * unit_pref.h's header comment. */
#define UNIT_PREF_BODY_MAX 16 /* "unit=fahrenheit" plus headroom -- generous over the ~14-byte worst case */

static esp_err_t unit_pref_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > UNIT_PREF_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[UNIT_PREF_BODY_MAX + 1];
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

    char unit_val[12];
    int unit_len = http_form_find_field(body, "unit", unit_val, sizeof(unit_val));
    unit_pref_t pref;
    /* Accepts either the short suffix ("C"/"F", matching unit_pref_suffix())
     * or the full word ("celsius"/"fahrenheit", matching what a browser
     * <select> naturally submits) -- case-insensitive on the full word since
     * that one is more likely to be hand-typed by a future integration.
     * Anything else is refused rather than defaulted, same "reject
     * outright, never guess" discipline as zones_http.c's field parsers. */
    if (unit_len > 0 && (strcmp(unit_val, "F") == 0 || strcasecmp(unit_val, "fahrenheit") == 0)) {
        pref = UNIT_PREF_FAHRENHEIT;
    } else if (unit_len > 0 && (strcmp(unit_val, "C") == 0 || strcasecmp(unit_val, "celsius") == 0)) {
        pref = UNIT_PREF_CELSIUS;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unit must be \"C\" or \"F\"");
        return ESP_OK;
    }

    if (unit_pref_set(pref) != ESP_OK) {
        /* Live value still took effect (unit_pref_set() updates RAM before
         * attempting the NVS write) -- only persistence failed, so this is
         * reported but not treated as a request failure the client needs to
         * retry differently. */
        ESP_LOGW(TAG, "unit preference applied but not persisted -- will not survive a reboot");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* "level=N" plus headroom, same "generous over the actual worst case,
 * checked against Content-Length before a single byte is read" reasoning as
 * UNIT_PREF_BODY_MAX above. */
#define SAFETY_LOG_LEVEL_BODY_MAX 16

/* POST /api/safety/log_level -- ROADMAP.md "SET_LOG_LEVEL (0x1B) has a codec
 * and a Pico consumer but no ESP caller" loose end. Deliberately API-only,
 * no LCD or web page control: this is a developer/bench knob (how chatty
 * the safety processor's own log_task is), not an operator-facing setting
 * -- there is no kiln-operation reason to ever change it during a firing,
 * unlike safety_get_status()'s clear_trip or the commissioning page's
 * config fields, which the operator or installer routinely needs. A
 * developer with `curl` or tools/PcTools has this endpoint; that is judged
 * sufficient exposure. Body is "level=N" (0-4, UART_LOG_LEVEL_* --
 * ERROR/WARN/INFO/DEBUG/VERBOSE), same query-string-in-POST-body shape as
 * unit_pref_post_handler() above. */
static esp_err_t safety_log_level_post_handler(httpd_req_t *req)
{
    if (!s_dash.safety) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "safety link not wired up");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > SAFETY_LOG_LEVEL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[SAFETY_LOG_LEVEL_BODY_MAX + 1];
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

    char level_val[8];
    int level_len = http_form_find_field(body, "level", level_val, sizeof(level_val));
    if (level_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"level\" field");
        return ESP_OK;
    }
    char *endptr = NULL;
    long level = strtol(level_val, &endptr, 10);
    if (endptr == level_val || *endptr != '\0' || level < 0 || level > UART_LOG_LEVEL_VERBOSE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "level must be 0-4 (ERROR..VERBOSE)");
        return ESP_OK;
    }

    esp_err_t err = safety_link_send_set_log_level(s_dash.safety, (uint8_t)level);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"send failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

/* ---- Planned-curve duration model -----------------------------------------
 *
 * Backs /api/profile_exec's total_planned_s/elapsed_s/remaining_s/
 * remaining_is_estimate and GET /api/profile_plan's polyline -- the actual
 * math and its one honesty rule (a zero/negative ramp_c_per_hr segment has
 * an UNKNOWN duration, not a zero one) live in
 * profile_feasibility_plan_curve() (profile_feasibility.h/.c), where they
 * are host-tested; this file only decides WHICH start_c to hand it and
 * shapes the JSON. */
#define PLAN_MAX_POINTS (PROFILE_MAX_SEGMENTS * 2 + 1)

/* start_c fallback for an idle/preview /api/profile_plan call (the profile
 * named isn't the one actually running, so there is no real starting
 * temperature to know yet) -- matches profile_feasibility.c's
 * FEASIBILITY_AMBIENT_C and profile_executor.c's FALLBACK_AMBIENT_C. Kept as
 * its own constant here (not a shared #include) for the same reason those
 * two don't share one: each caller asks a different question, at a
 * different time, and 20 C is coincidentally the right idle-room answer to
 * all of them, not a value one owns and the others borrow. */
#define PROFILE_PLAN_PREVIEW_AMBIENT_C 20.0f

/* Fills in the four exec-status timing fields from a status snapshot. IDLE
 * reports elapsed 0 and everything else unknown/absent -- there is no run to
 * time. DONE reports remaining 0 exactly (a real, not estimated, answer: the
 * run finished). FAULTED reports remaining unknown -- the run stopped short
 * of the plan with no path back to RUNNING except halt() then a fresh
 * run(), and "time left on a schedule nothing is following anymore" has no
 * honest number. RUNNING/PAUSED report remaining_is_estimate true
 * unconditionally whenever the total is known at all: ramp-lock can always
 * overrun the plan if a zone lags, and this module does not attempt to
 * correct for observed lag (see the report this task asked for) -- it is a
 * plan-only estimate, every time, not just when a lag is currently visible. */
static void plan_exec_fields(const profile_exec_status_t *st, int64_t *out_total_planned_s,
                             uint32_t *out_elapsed_s, int64_t *out_remaining_s, bool *out_remaining_is_estimate)
{
    *out_elapsed_s = 0;
    *out_total_planned_s = -1;
    *out_remaining_s = -1;
    *out_remaining_is_estimate = false;

    if (st->state == PROFILE_EXEC_IDLE) {
        return;
    }

    *out_elapsed_s = st->total_elapsed_s;
    int64_t total = profile_feasibility_plan_curve(st->segments, st->segment_count, st->run_start_c,
                                                   NULL, 0, NULL);
    *out_total_planned_s = total;

    switch (st->state) {
    case PROFILE_EXEC_DONE:
        *out_remaining_s = 0;
        *out_remaining_is_estimate = false;
        break;
    case PROFILE_EXEC_FAULTED:
        *out_remaining_s = -1;
        *out_remaining_is_estimate = false;
        break;
    case PROFILE_EXEC_RUNNING:
    case PROFILE_EXEC_PAUSED:
    default:
        if (total >= 0) {
            int64_t rem = total - (int64_t)st->total_elapsed_s;
            *out_remaining_s = rem > 0 ? rem : 0;
            *out_remaining_is_estimate = true;
        }
        break;
    }
}

static esp_err_t profile_exec_status_get_handler(httpd_req_t *req)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    char name_escaped[sizeof(st.profile_name) * 2 + 1];
    json_escape(st.profile_name, name_escaped, sizeof(name_escaped));
    char reason_escaped[sizeof(st.fault_reason) * 2 + 1];
    json_escape(st.fault_reason, reason_escaped, sizeof(reason_escaped));

    int64_t total_planned_s, remaining_s;
    uint32_t elapsed_s;
    bool remaining_is_estimate;
    plan_exec_fields(&st, &total_planned_s, &elapsed_s, &remaining_s, &remaining_is_estimate);
    char total_planned_buf[24], remaining_buf[24];
    if (total_planned_s < 0) {
        snprintf(total_planned_buf, sizeof(total_planned_buf), "null");
    } else {
        snprintf(total_planned_buf, sizeof(total_planned_buf), "%lld", (long long)total_planned_s);
    }
    if (remaining_s < 0) {
        snprintf(remaining_buf, sizeof(remaining_buf), "null");
    } else {
        snprintf(remaining_buf, sizeof(remaining_buf), "%lld", (long long)remaining_s);
    }

    /* Sized against the real worst case rather than the previous estimate.
     * Per zone the exec shape can emit a fully backslash-escaped 95-char
     * fault_reason (190 bytes) on top of ~130 bytes of fixed keys, so 320,
     * not 224 -- 224 was already optimistic before this change and would
     * have truncated mid-object into invalid JSON in a multi-zone fault. The
     * 960-byte fixed part covers the run-level line (its own escaped reason,
     * plus the four duration-model fields added for the profile-plan
     * contract -- at most ~48 bytes more) plus the "last_run" object at ITS
     * worst case. The httpd task runs on an 8192-byte stack
     * (wifi_provision_http.c), so ~2.5 KB here is comfortable. */
    char json[960 + MAX31856_CHANNEL_COUNT * 320];
    int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"profile_id\":%u,\"profile_name\":\"%s\",\"zone_mask\":%u,"
        "\"segment_index\":%u,\"segment_count\":%u,\"dwelling\":%s,\"target_c\":%.2f,"
        "\"segment_elapsed_s\":%lu,\"dwell_remaining_s\":%lu,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,\"fault_reason\":\"%s\",\"fault_guard\":%u,"
        "\"total_planned_s\":%s,\"elapsed_s\":%lu,\"remaining_s\":%s,\"remaining_is_estimate\":%s,",
        exec_state_name(st.state), st.profile_id, name_escaped, st.zone_mask, st.segment_index,
        st.segment_count, st.dwelling ? "true" : "false", (double)st.target_c,
        (unsigned long)st.segment_elapsed_s, (unsigned long)st.dwell_remaining_s,
        st.ramp_lock_held ? "true" : "false", st.ramp_lock_lagging_mask, reason_escaped, st.fault_guard,
        total_planned_buf, (unsigned long)elapsed_s, remaining_buf, remaining_is_estimate ? "true" : "false");
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

/* GET /api/profile_plan?id=<profile_id> -- the PLANNED curve as a polyline
 * ready to draw: one {"t","c"} point per ramp start/end and dwell
 * start/end (see plan_curve() above), CELSIUS always -- the web/LCD front
 * ends already own converting for unit_pref, this endpoint has no display
 * concerns.
 *
 * Starting temperature: if `id` names the profile the executor is actually
 * RUNNING/PAUSED/DONE/FAULTED on right now, this uses that run's captured
 * run_start_c -- the real reading segment 0's ramp started from -- so the
 * curve lines up with the live /api/profile_exec numbers for that firing.
 * Otherwise (idle preview, or a different profile than whatever is
 * running) there is no real starting temperature to know yet, so this
 * falls back to PROFILE_PLAN_PREVIEW_AMBIENT_C, same as
 * profile_feasibility.c's edit-time check -- see that module's doc comment
 * for why a constant beats a live cold-junction read here (a colour/curve
 * that shifts under a user who made no edit is worse than one a few degrees
 * stale). */
static esp_err_t profile_plan_get_handler(httpd_req_t *req)
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
    if (end == id_str || id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    profile_t p;
    if (!profiles_http_get((uint8_t)id, &p)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    profile_exec_status_t st;
    profile_executor_get_status(&st);
    float start_c = PROFILE_PLAN_PREVIEW_AMBIENT_C;
    if (st.state != PROFILE_EXEC_IDLE && st.profile_id == (uint8_t)id) {
        start_c = st.run_start_c;
    }

    profile_plan_point_t points[PLAN_MAX_POINTS];
    size_t point_count = 0;
    int64_t total_s = profile_feasibility_plan_curve(p.segments, p.segment_count, start_c, points,
                                                     PLAN_MAX_POINTS, &point_count);

    char name_escaped[sizeof(p.name) * 2 + 1];
    json_escape(p.name, name_escaped, sizeof(name_escaped));

    /* Fixed part plus up to PLAN_MAX_POINTS (25 for PROFILE_MAX_SEGMENTS ==
     * 12) points at ~40 bytes each worst case ("{"t":123456.00,"c":-999.99},")
     * -- comfortably inside the httpd task's 8192-byte stack alongside the
     * other buffers this file already keeps there. */
    char json[192 + PLAN_MAX_POINTS * 48];
    size_t o = 0;
    int n = snprintf(json, sizeof(json), "{\"profile_id\":%ld,\"name\":\"%s\",\"total_planned_s\":", id,
                     name_escaped);
    o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    if (total_s < 0) {
        n = snprintf(json + o, sizeof(json) - o, "null,\"points\":[");
    } else {
        n = snprintf(json + o, sizeof(json) - o, "%lld,\"points\":[", (long long)total_s);
    }
    if (n > 0 && (size_t)n < sizeof(json) - o) o += (size_t)n;
    for (size_t i = 0; i < point_count; i++) {
        n = snprintf(json + o, sizeof(json) - o, "%s{\"t\":%.0f,\"c\":%.2f}", i == 0 ? "" : ",",
                    (double)points[i].t, (double)points[i].c);
        if (n < 0 || (size_t)n >= sizeof(json) - o) break;
        o += (size_t)n;
    }
    if (o + 2 < sizeof(json)) {
        json[o++] = ']';
        json[o++] = '}';
    }
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

    /* watchdog_cfg.h: a UI-only warning (main_page.html/ui_page_home.c's
     * extra confirmation dialogs) is bypassable with curl straight to this
     * endpoint. Do NOT refuse the start -- the owner wants this usable during
     * development -- but log it loudly server-side so it is never a silent
     * fact about a running firing. */
    if (watchdog_cfg_panic_disabled()) {
        ESP_LOGW(TAG, "profile_exec/start: id=%ld starting with the task-watchdog PANIC DISABLED -- "
                      "a hung task during this firing will NOT reboot the board", id);
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

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_CLEAR_TRIP (0x0A) --
 * the GUI's path to acknowledging a Pico-latched trip. No body: the ESP
 * derives trip_mask itself from its own cached Pico DIAG state rather than
 * trusting a value supplied by the browser -- see
 * safety_link_send_clear_trip()'s doc comment in safety_link.h for the full
 * design (staleness bound, why a resend after conditions change is still
 * safe). Refusal reasons are reported honestly rather than folded into a
 * generic error, same convention as profile_exec_pause/resume above: an
 * operator staring at "refused" with no reason is an operator who reaches
 * for SWD. Success here only means the broadcast was handed to the UART --
 * it is not proof the Pico accepted it; the caller must watch the next
 * /api/status poll for the trip to actually clear, same as everywhere else
 * this driver observes Pico state via telemetry rather than an ACK. */
static esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)
{
    if (!s_dash.safety) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "safety link not wired up");
        return ESP_OK;
    }

    esp_err_t err = safety_link_send_clear_trip(s_dash.safety);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"no trip currently latched, or Pico diagnostics are stale\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"send failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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
    /* Report the zones this board actually HAS, not the number of MAX31856
     * channels the hardware could carry. These differ whenever an operator
     * has declared fewer thermocouples than are wired (thermo_count=1 on a
     * 3-channel board is the bench's normal state), and every zones_config_*
     * getter already refuses an index >= thermo_count -- so the extra rows
     * and columns were cells that could never become valid, rendered as a
     * 3x3 grid of "not measured yet" on a kiln with one zone. */
    const uint8_t zone_count = zones_config_get_thermo_count();
    size_t o = 0;
    o += snprintf(json + o, sizeof(json) - o, "{\"zone_count\":%u,\"cells\":[", (unsigned)zone_count);
    bool first = true;
    for (uint8_t i = 0; i < zone_count; i++) {
        for (uint8_t j = 0; j < zone_count; j++) {
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
        /* An RGA describes how n>=2 control loops interact. On a board with
         * fewer than two declared zones there is nothing to interact, so
         * autotune_engine_compute_rga()'s generic "no 2 zones yet have every
         * cross-gain between them measured" reads as "keep tuning and it
         * will appear" -- it never will. Say which of the two it is. */
        char rga_reason[sizeof(rga.invalid_reason) * 2 + 1];
        if (zone_count < 2) {
            json_escape("this kiln has fewer than 2 zones -- an RGA needs at least 2 interacting zones",
                        rga_reason, sizeof(rga_reason));
        } else {
            json_escape(rga.invalid_reason, rga_reason, sizeof(rga_reason));
        }
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
    static const httpd_uri_t profile_plan_uri = {
        .uri = "/api/profile_plan", .method = HTTP_GET, .handler = profile_plan_get_handler,
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
    static const httpd_uri_t safety_clear_trip_uri = {
        .uri = "/api/safety/clear_trip", .method = HTTP_POST, .handler = safety_clear_trip_post_handler,
    };
    static const httpd_uri_t safety_log_level_uri = {
        .uri = "/api/safety/log_level", .method = HTTP_POST, .handler = safety_log_level_post_handler,
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
    static const httpd_uri_t unit_pref_uri = {
        .uri = "/api/unit_pref", .method = HTTP_POST, .handler = unit_pref_post_handler,
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
    err = httpd_register_uri_handler(server, &profile_plan_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/profile_plan) failed: %s", esp_err_to_name(err));
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
    err = httpd_register_uri_handler(server, &safety_clear_trip_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/safety/clear_trip) failed: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(server, &safety_log_level_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/safety/log_level) failed: %s", esp_err_to_name(err));
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
    err = httpd_register_uri_handler(server, &unit_pref_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/unit_pref) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "dashboard API up (io_ready=%d, thermo_ready=%d, safety_ready=%d)", s_dash.io != NULL,
             s_dash.thermo_bus != NULL && s_dash.thermo_bus->initialized, s_dash.safety != NULL);
    return ESP_OK;
}
