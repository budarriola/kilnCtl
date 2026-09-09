#include "diagnostics_http.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h" /* heap_caps_malloc() -- cfgfs_file_get_handler()/cfgfs_file_post_handler() below */
#include "esp_littlefs.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "cfg_fs.h"
#include "cfg_fs_mount.h"
#include "cfg_fs_status.h"
#include "crash_report.h"
#include "danger_mode.h"
#include "dashboard_http.h"
#include "display_power_cfg.h"
#include "hal_kv.h"
#include "http_form.h"
#include "kiln_cfg_store.h"
#include "kiln_io.h"
#include "lvgl_port.h"
#include "adaptive_tune.h" /* adaptive_tune_get_kibase_dualwrite_status() -- /api/cfgfs row */
#include "profile_executor.h" /* firing_stats_get_dualwrite_status() -- /api/cfgfs row */
#include "profiles_http.h" /* profiles_http_get_dualwrite_status() -- /api/cfgfs per-slot rows */
#include "profiles_types.h" /* PROFILES_MAX_COUNT */
#include "ramp_assist_cfg.h"
#include "relay_cycles.h" /* relay_cycles_reset_post_handler() below needs RELAY_CYCLES_COUNT;
                              relay_cycles_get_dualwrite_status() -- /api/cfgfs row */
#include "safety_link.h" /* SafetyLinkClass/safety_link_get_status() -- thermo_faults_get_handler()'s
                           * "safety" block below */
#include "thermo_owner.h"
#include "time_sync.h" /* time_sync_get_tz_dualwrite_status() -- /api/cfgfs TZ row */
#include "uart_task_ids.h" /* SAFETY_FLAG_TC_NOT_INSTALLED/SAFETY_FLAG_TC_INJECTED */
#include "unit_pref.h"
#include "watchdog_cfg.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zones_config_cfg_fs.h" /* zones_config_cfg_fs_load_raw() -- dual-write picture for /api/cfgfs */

#if CONFIG_LWIP_STATS
#include "lwip/stats.h"
#endif

static const char *TAG = "diagnostics_http";

/* Set by diagnostics_http_start()'s `safety` argument. NULL-tolerant, same
 * convention as every other hardware pointer in this codebase (kio/
 * thermo_bus in dashboard_http.c, etc.) -- diagnostics_timing_get_handler()
 * below checks it before calling safety_link_get_stats(). */
static SafetyLinkClass *s_diag_safety;

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t diagnostics_page_html_gz_start[] asm("_binary_diagnostics_page_html_gz_start");
extern const uint8_t diagnostics_page_html_gz_end[] asm("_binary_diagnostics_page_html_gz_end");
extern const uint8_t safety_page_html_gz_start[] asm("_binary_safety_page_html_gz_start");
extern const uint8_t safety_page_html_gz_end[] asm("_binary_safety_page_html_gz_end");

/* Same content-negotiation shape as readiness_http.c's page_get_handler():
 * web_client_accepts_gzip() covers the "no Accept-Encoding header" (legal,
 * served gzip per RFC 9110 s12.5.3) and "header present but excludes gzip"
 * (406, since this server keeps no uncompressed copy) cases; see
 * web_encoding.h for the full rationale. One tiny helper instead of three
 * near-identical handlers differing only in which *_gz_start/_end pair and
 * page name they use. */
static esp_err_t send_gz_page(httpd_req_t *req, const char *page_name, const uint8_t *start, const uint8_t *end)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, page_name);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t diagnostics_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "diagnostics_page.html", diagnostics_page_html_gz_start, diagnostics_page_html_gz_end);
}

static esp_err_t safety_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "safety_page.html", safety_page_html_gz_start, safety_page_html_gz_end);
}

/* GET /api/thermo/faults -- dedicated per-channel data for
 * thermo_faults_page.html. Added 2026-08-21 to fix "the page just says
 * Loading forever":
 *
 * ROOT CAUSE (confirmed live against the board at 192.168.1.156 before this
 * fix): the page used to depend entirely on GET /api/status's combined
 * "channels" array. That endpoint (dashboard_http.c's status_get_handler())
 * calls MAX31856_read_all() directly, ONE call covering every channel in a
 * single HTTP request/response, bypassing thermo_owner's queue entirely. If
 * any single channel's SPI transaction wedges, the whole /api/status
 * response -- relays, safety link, heap, firmware version, everything, not
 * just the thermocouples -- never lands, and the page's fetch() has no
 * timeout of its own, so it sits on "Loading..." forever: not a bug in the
 * JSON, not an HTTP error the page mishandles, but a response that never
 * arrives at all. (Live right now the board answers fine -- all three
 * channels healthy, see this change's commit message for the actual curl
 * output -- so this could not be reproduced live in the failed state; the
 * fix below removes the failure mode regardless of whether a given poll
 * happens to catch it.)
 *
 * FIX: query each channel SEPARATELY through thermo_owner, which already
 * gives exactly the bounded-per-channel answer this page needs:
 * thermo_owner_command_read()'s post_and_wait() waits at most
 * THERMO_OWNER_WAIT_MS (200ms, thermo_owner.c) for that ONE channel's
 * answer, win or lose, regardless of what the owner task itself is doing.
 * A channel whose SPI transaction is wedged still bounds every OTHER
 * channel's query to that same 200ms -- it cannot come back healthy, but it
 * cannot hang the response either, and the wedged channel itself is
 * reported as "timeout" (a real, displayed result -- requirement: never
 * silently report a silent channel as healthy), not dropped from the
 * payload. Worst case for MAX31856_CHANNEL_COUNT channels queried serially,
 * with the owner task fully wedged on every one, is
 * MAX31856_CHANNEL_COUNT * THERMO_OWNER_WAIT_MS -- three channels today,
 * comfortably under a second, and bounded rather than open-ended either
 * way.
 *
 * SCOPE: reports every WIRED channel slot (0..MAX31856_CHANNEL_COUNT-1),
 * not zones_config's thermo_count -- this board's zones config currently
 * reports thermo_count=1 while three MAX31856 channels actually answer at
 * boot (device log). A wired channel with a real fault is exactly the case
 * this page exists to surface; gating the list by zone configuration would
 * hide it behind a setting that has nothing to do with the sensor being
 * faulted. "shown":"wired_channels" in the response says which policy is in
 * effect so the page can say so too, rather than leaving it ambiguous. */
static esp_err_t thermo_faults_get_handler(httpd_req_t *req)
{
    char json[1536];
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

    APPEND("{\"shown\":\"wired_channels\",\"channel_count\":%u,\"channels\":[",
          (unsigned)MAX31856_CHANNEL_COUNT);

    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        MAX31856Reading reading;
        esp_err_t err = thermo_owner_command_read(ch, &reading);

        /* Four distinct, deliberately separate states -- collapsing
         * "timeout" into "spi_failed" (both leave reading.spi_failed true,
         * per thermo_owner_command_read()'s own never-stale contract) would
         * hide exactly the distinction the owner asked for: a channel that
         * never answered the owner task at all vs. one that answered with a
         * real SPI-level failure vs. one that was never wired up. */
        const char *state;
        if (err == ESP_ERR_TIMEOUT) {
            state = "timeout";
        } else if (err == ESP_ERR_NOT_FOUND) {
            state = "absent";
        } else if (reading.spi_failed) {
            state = "spi_failed";
        } else {
            state = "ok";
        }

        /* Same null-not-zero convention as dashboard_http.c's channels
         * array: NaN/UNKNOWN have no honest JSON numeric literal. */
        char cj_buf[16];
        if (isnan(reading.cj_temperature_c)) {
            snprintf(cj_buf, sizeof(cj_buf), "null");
        } else {
            snprintf(cj_buf, sizeof(cj_buf), "%.2f", (double)reading.cj_temperature_c);
        }
        char age_buf[16];
        if (reading.age_ms == MAX31856_READING_AGE_UNKNOWN) {
            snprintf(age_buf, sizeof(age_buf), "null");
        } else {
            snprintf(age_buf, sizeof(age_buf), "%lu", (unsigned long)reading.age_ms);
        }

        APPEND("%s{\"channel\":%u,\"state\":\"%s\",\"fault_status\":%u,\"fault_pin\":%s,"
              "\"cj_c\":%s,\"age_ms\":%s,\"stale\":%s}",
              ch == 0 ? "" : ",", (unsigned)ch, state, (unsigned)reading.fault_status,
              reading.fault_pin_asserted ? "true" : "false", cj_buf, age_buf,
              reading.stale ? "true" : "false");
    }
    APPEND("]");

    /* Safety processor's own MAX31856 (RP2040-side), folded in next to the
     * main-board channels above rather than shown only on the dashboard's
     * cached safety_temp_c tile (dashboard_http.c) -- 2026-09-08 owner
     * request, prompted directly by a night where that cached tile read a
     * plausible-looking temperature while the real safety TC was both
     * open-loop (chip not converting, THERMO_FAULT_* bits all clear) AND
     * unreachable for a raw register read because the link itself was down.
     * A cached "looks fine" number and a dead sensor are indistinguishable
     * from that tile alone; this block exists to make the distinction the
     * owner actually needed that night.
     *
     * "state" priority, most-diagnostic first:
     *   no_link         -- s_diag_safety NULL, or safety_link_get_status()
     *                       failed, or its own link_up is false: nothing
     *                       below this point is fresh data, only whatever
     *                       was last cached (age_ms says how stale).
     *   faulted         -- TEMP_VALID and a real THERMO_FAULT_* bit is set:
     *                       the MAX31856 IS converting and DID complete a
     *                       transfer, and is reporting a genuine hardware
     *                       condition (open circuit, TC/CJ out of range,
     *                       over/under voltage). cj_c is still meaningful
     *                       here -- a sane cold junction alongside a faulted
     *                       TC reading says the chip is alive and the
     *                       PROBE is the problem, not the chip.
     *   probe_fault     -- NOT TEMP_VALID, fault==0, BUT the Pico's V3 status
     *                       frame reports cj_valid==true (safety_link.h's
     *                       SAFETY_LINK_STATUS_FLAG2_CJ_VALID) with a real,
     *                       finite cj_c from the SAME successful transfer:
     *                       the MAX31856 chip is alive and converting, only
     *                       the external thermocouple probe/wiring or the
     *                       commissioned-type CR1 verify is the problem
     *                       (thermo_task.c's CR1-verify-downgrade path,
     *                       SaftyFW -- "safety TC invalid is one CR1 byte").
     *                       2026-09-08: this used to be indistinguishable
     *                       from not_converting below because link_task.c
     *                       (SaftyFW) NaN'd cj_c alongside tc_c whenever
     *                       temp_valid was false, discarding this exact
     *                       distinction; fixed by carrying cj_valid
     *                       independently in the V3 status frame's existing
     *                       spare flags2 bit (no protocol bump needed).
     *   not_converting  -- NOT TEMP_VALID, fault==0, and EITHER the peer
     *                       hasn't confirmed V3 support yet / predates it
     *                       (cj_valid_known false -- no cj-side evidence
     *                       exists at all, same honest "cannot be
     *                       distinguished with current link data" verdict
     *                       this comment used to give for every such case)
     *                       OR cj_valid_known is true and cj_valid is false
     *                       (cj_c is ALSO NaN -- the chip itself never
     *                       completed a conversion, e.g. thermo_task.c's
     *                       DRDY-silence path, SaftyFW). THIS is the state
     *                       seen the night this was written.
     *   ok              -- TEMP_VALID and fault==0.
     *
     * not_installed/injected are reported as independent booleans (not
     * folded into "state") because they are orthogonal facts already carried
     * in-band today, at zero extra wire cost -- LINK_FLAG_TC_NOT_INSTALLED/
     * _TC_INJECTED (link_frame.h), decoded into safety_link_status_t::flags
     * bits 6/7 by safety_link_frames.c's safety_apply_status() and already
     * surfaced to safety_cfg_http.c/safety_page.html; this is simply the
     * first place they are shown NEXT TO the other thermocouple/fault
     * entries rather than on their own commissioning page. */
    safety_link_status_t sl;
    esp_err_t safety_err = s_diag_safety ? safety_link_get_status(s_diag_safety, &sl) : ESP_FAIL;
    if (safety_err != ESP_OK || !sl.link_up) {
        APPEND(",\"safety\":{\"state\":\"no_link\",\"link_up\":false}");
    } else {
        const char *state = diag_safety_tc_state(sl.tc_temp_c, sl.tc_fault, sl.cj_temp_c,
                                                  sl.cj_valid_known, sl.cj_valid);
        bool not_installed = (sl.flags & SAFETY_FLAG_TC_NOT_INSTALLED) != 0u;
        bool injected = (sl.flags & SAFETY_FLAG_TC_INJECTED) != 0u;

        char tc_buf[16], cj_buf2[16];
        if (isnan(sl.tc_temp_c)) {
            snprintf(tc_buf, sizeof(tc_buf), "null");
        } else {
            snprintf(tc_buf, sizeof(tc_buf), "%.2f", (double)sl.tc_temp_c);
        }
        if (isnan(sl.cj_temp_c)) {
            snprintf(cj_buf2, sizeof(cj_buf2), "null");
        } else {
            snprintf(cj_buf2, sizeof(cj_buf2), "%.2f", (double)sl.cj_temp_c);
        }

        APPEND(",\"safety\":{\"state\":\"%s\",\"link_up\":true,\"link_age_ms\":%u,"
              "\"tc_c\":%s,\"cj_c\":%s,\"fault_status\":%u,"
              "\"not_installed\":%s,\"injected\":%s,"
              "\"cj_valid_known\":%s,\"cj_valid\":%s}",
              state, (unsigned)sl.age_ms, tc_buf, cj_buf2, (unsigned)sl.tc_fault,
              not_installed ? "true" : "false", injected ? "true" : "false",
              sl.cj_valid_known ? "true" : "false", sl.cj_valid ? "true" : "false");
    }
    APPEND("}");

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
#undef APPEND
}

/* Own copy of dashboard_http.c's json_escape() -- that one is `static` to its
 * own TU, and duplicating six lines is cheaper (and matches this codebase's
 * existing precedent, e.g. run_state.c/ota_record.c/crash_report.c's three
 * independent nvs_partition_init() copies) than introducing a shared header
 * for one tiny helper. */
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

/* GET /api/crash_report -- the last-crash summary crash_report.c persisted
 * (task/cause/PC/backtrace/reset-reason), for the diagnostics page's "Last
 * crash" section. {"present":false} is a complete, valid response (the
 * common case: no crash on record) -- every other field is only present
 * alongside "present":true. */
static esp_err_t crash_report_get_handler(httpd_req_t *req)
{
    crash_report_record_t rec;
    bool present = crash_report_get(&rec);

    char json[768];
    size_t o = 0;
    int n;

    if (!present) {
        n = snprintf(json, sizeof(json), "{\"present\":false}");
        o = (n > 0) ? (size_t)n : 0;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, o);
    }

    char task_esc[sizeof(rec.exc_task) * 2 + 1];
    char cause_str_esc[sizeof(rec.exc_cause_str) * 2 + 1];
    char reset_reason_esc[sizeof(rec.reset_reason) * 2 + 1];
    json_escape(rec.exc_task, task_esc, sizeof(task_esc));
    json_escape(rec.exc_cause_str, cause_str_esc, sizeof(cause_str_esc));
    json_escape(rec.reset_reason, reset_reason_esc, sizeof(reset_reason_esc));

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"present\":true,\"acknowledged\":%s,\"exc_cause\":%lu,\"exc_cause_str\":\"%s\","
          "\"exc_pc\":\"0x%08lx\",\"exc_addr\":\"0x%08lx\",\"exc_a0\":\"0x%08lx\","
          "\"exc_a1_sp\":\"0x%08lx\",\"exc_task\":\"%s\","
          "\"found_on_boot_reset_reason\":\"%s\","
          "\"frame_trustworthy\":%s,"
          "\"backtrace\":[",
          rec.acknowledged ? "true" : "false", (unsigned long)rec.exc_cause, cause_str_esc,
          (unsigned long)rec.exc_pc, (unsigned long)rec.exc_addr, (unsigned long)rec.exc_a0,
          (unsigned long)rec.exc_a1, task_esc, reset_reason_esc,
          crash_report_frame_trustworthy(&rec) ? "true" : "false");
    for (uint8_t i = 0; i < rec.bt_count && i < CRASH_REPORT_BT_MAX; i++) {
        /* Hex strings, not JSON numbers: these are code addresses, and the
         * only thing anyone does with them is paste them into addr2line.
         * Decimal ("299") is unusable for that and reads as a plausible
         * small integer rather than the obviously-wrong address it is. */
        APPEND("%s\"0x%08lx\"", i == 0 ? "" : ",", (unsigned long)rec.backtrace_pc[i]);
    }
    APPEND("],\"backtrace_corrupted\":%s}", rec.bt_corrupted ? "true" : "false");

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
#undef APPEND
}

/* POST /api/crash_report/ack -- operator has seen the record, stop showing
 * it as new. Does NOT erase the coredump/record -- see /clear for that. */
static esp_err_t crash_report_ack_post_handler(httpd_req_t *req)
{
    bool ok = crash_report_acknowledge();
    const char *json = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no crash record to acknowledge\"}";
    httpd_resp_set_type(req, "application/json");
    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return httpd_resp_send(req, json, strlen(json));
}

/* POST /api/crash_report/clear -- acknowledge AND erase the coredump image
 * plus this module's own NVS record, freeing the `coredump` partition slot
 * for the next crash. */
static esp_err_t crash_report_clear_post_handler(httpd_req_t *req)
{
    esp_err_t err = crash_report_clear();
    char json[128];
    int n;
    if (err == ESP_OK) {
        n = snprintf(json, sizeof(json), "{\"ok\":true}");
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n > 0) ? (size_t)n : 0);
}

/* GET /api/debug/lwip_stats -- ROADMAP.md M10's open item, "HTTP connection
 * resets under concurrency": a burst of 8 parallel /app.js fetches resets
 * one of them, and two mechanisms (heap exhaustion, socket-timeout) are
 * already ruled out. This is not a permanent diagnostics surface -- it
 * exists to let this specific investigation read lwIP's own pool/error
 * counters ("add this to your debugger's watchlist" is literally
 * lwip/stats.h's own comment on lwip_stats) from a live burst without a
 * JTAG halt, which would perturb the very timing being measured.
 *
 * Deliberately does NOT call stats_display(): that macro
 * (LWIP_PLATFORM_DIAG, port/esp32xx/include/arch/cc.h) only routes through
 * ESP_LOG at all when CONFIG_LWIP_DEBUG_ESP_LOG is on, and even then the
 * call it makes is hardcoded to ESP_LOG_LEVEL(ESP_LOG_DEBUG, ...) --
 * stripped at COMPILE time by this project's CONFIG_LOG_MAXIMUM_LEVEL=3
 * (INFO). Raising that globally to see one investigation's output would
 * compile debug-level logging into every other subsystem in the tree for
 * no reason. Reading lwip_stats.tcp straight into the JSON response
 * sidesteps both the DIAG macro and the log-level cap entirely, and cannot
 * be dropped by uart_log_bridge's queue the way a burst of printed lines
 * could be.
 *
 * lwip_stats.mem does NOT exist on this port and is not read here: MEM_STATS
 * (lwip/opt.h) is unconditionally 0 whenever MEM_LIBC_MALLOC == 1, which
 * ESP-IDF's lwipopts.h sets -- this platform routes lwIP's allocations
 * through the C library / ESP heap rather than lwIP's own arena, so there
 * is no separate lwIP heap pool to have a counter for. Found by the build
 * itself refusing to compile a member that does not exist, not assumed.
 *
 * Compiles to a 501 when CONFIG_LWIP_STATS is off (the normal build), so
 * this route costs nothing and reveals nothing outside this investigation. */
static esp_err_t lwip_stats_get_handler(httpd_req_t *req)
{
#if CONFIG_LWIP_STATS
    char json[256];
    int n = snprintf(json, sizeof(json),
                      "{\"ok\":true,"
                      "\"tcp\":{\"xmit\":%lu,\"recv\":%lu,\"drop\":%lu,\"chkerr\":%lu,\"lenerr\":%lu,"
                      "\"memerr\":%lu,\"rterr\":%lu,\"proterr\":%lu,\"opterr\":%lu,\"err\":%lu}}",
                      (unsigned long)lwip_stats.tcp.xmit,
                      (unsigned long)lwip_stats.tcp.recv, (unsigned long)lwip_stats.tcp.drop,
                      (unsigned long)lwip_stats.tcp.chkerr, (unsigned long)lwip_stats.tcp.lenerr,
                      (unsigned long)lwip_stats.tcp.memerr, (unsigned long)lwip_stats.tcp.rterr,
                      (unsigned long)lwip_stats.tcp.proterr, (unsigned long)lwip_stats.tcp.opterr,
                      (unsigned long)lwip_stats.tcp.err);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n > 0 && (size_t)n < sizeof(json)) ? (size_t)n : 0);
#else
    (void)req;
    const char *json = "{\"ok\":false,\"error\":\"CONFIG_LWIP_STATS not built\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "501 Not Implemented");
    return httpd_resp_send(req, json, strlen(json));
#endif
}

/* GET /api/watchdog_cfg -- current state of the dev-only task-watchdog-panic
 * disable switch (watchdog_cfg.h). Also carried in GET /api/status
 * (dashboard_http.c) for the persistent indicator; this endpoint exists so
 * the debug page's own control doesn't need to depend on that other page's
 * response shape. */
static esp_err_t watchdog_cfg_get_handler(httpd_req_t *req)
{
    char json[64];
    int n = snprintf(json, sizeof(json), "{\"panic_disabled\":%s}",
                     watchdog_cfg_panic_disabled() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* POST /api/watchdog_cfg -- body: disabled=0|1 (form-encoded, same
 * convention as every other small POST setter in this codebase --
 * dashboard_http.c's safety_log_level_post_handler() etc.). Persists to NVS
 * AND applies immediately, no reboot needed -- see
 * watchdog_cfg_set_panic_disabled(). The debug page itself gates turning
 * this ON (disabled=1) behind window.kcConfirm before ever sending the
 * request; this handler does not re-confirm, it just does what it's asked
 * and logs loudly (watchdog_cfg.c already does that logging). */
#define WATCHDOG_CFG_BODY_MAX 32
static esp_err_t watchdog_cfg_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > WATCHDOG_CFG_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[WATCHDOG_CFG_BODY_MAX + 1];
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
    int val_len = http_form_find_field(body, "disabled", val, sizeof(val));
    if (val_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"disabled\" field");
        return ESP_OK;
    }
    bool disabled = (val[0] == '1');

    esp_err_t err = watchdog_cfg_set_panic_disabled(disabled, "web debug page");
    char json[96];
    int n;
    if (err == ESP_OK) {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"panic_disabled\":%s}", disabled ? "true" : "false");
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef WATCHDOG_CFG_BODY_MAX

/* POST /api/relay_cycles/reset {relay: N} -- RELAY_LIFE_BUDGET.md.
 * The web diagnostics page's "Reset count" button (diagnostics_page.html,
 * gated by window.kcConfirm()) posts here after the operator has physically
 * replaced the relay.
 *
 * relay_cycles.c (see docs/RELAY_LIFE_BUDGET.md) added
 * relay_cycles_reset(unsigned relay) -- this handler now calls it instead of
 * answering the placeholder 501 an earlier pass returned. relay_cycles_reset()
 * itself owns the flash-worker dispatch (checking uart_bridge_ext_is_on_
 * flash_worker() first, same as every other guarded write in this codebase --
 * see that function's own comment) -- httpd handler tasks are not the flash
 * worker, so this call always takes the dispatch path, never the inline one,
 * but the function handles both so this handler does not have to know which. */
#define RELAY_CYCLES_RESET_BODY_MAX 32
static esp_err_t relay_cycles_reset_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > RELAY_CYCLES_RESET_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[RELAY_CYCLES_RESET_BODY_MAX + 1];
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

    char val[8];
    int val_len = http_form_find_field(body, "relay", val, sizeof(val));
    if (val_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"relay\" field");
        return ESP_OK;
    }
    char *endp = NULL;
    long relay = strtol(val, &endp, 10);
    if (endp == val || *endp != '\0' || relay < 0 || relay >= (long)RELAY_CYCLES_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay out of range 0..4");
        return ESP_OK;
    }

    /* Log before answering, same "old count, before the reset" intent the
     * plan asks for -- relay_cycles_reset() logs the same fact internally,
     * but that log line does not carry the HTTP caller/relay-mapping
     * context this one does, so this stays even though it is now somewhat
     * redundant. */
    relay_cycles_budget_t before;
    relay_cycles_budget((uint8_t)relay, &before);
    ESP_LOGI(TAG, "relay_cycles reset requested via HTTP for relay %ld (old count %lu)",
             relay, (unsigned long)before.cycles);

    bool ok = relay_cycles_reset((unsigned)relay);

    /* 256, not 192: opus review's corrected failure-path message (below) is
     * longer than the old "will retry on the next periodic persist" text --
     * measured worst case (relay at INT32_MIN's 11-digit width) is 211
     * bytes; sized with headroom rather than to the exact byte. */
    char json[256];
    int n;
    if (ok) {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"relay\":%ld,\"cycles\":0}", relay);
        httpd_resp_set_status(req, "200 OK");
    } else {
        /* relay_cycles_reset() only fails on a persist error (the flash
         * worker was unreachable, or the write itself failed) -- the index
         * was already validated above. The count is still zeroed in RAM
         * (relay_cycles_reset()'s own contract), so this is "not yet
         * durable", not "nothing happened" -- but opus review: it will only
         * actually become durable if something later calls relay_cycles_
         * maybe_persist() successfully, and boot_guard.h's RECOVERY MODE
         * deliberately never starts the tasks (profile_executor/autotune_
         * engine) that tick it, so the honest statement is "RAM-only until
         * the next persist, and lost on a reboot before that if the flash
         * worker is down/in recovery mode" -- not a guaranteed retry. */
        n = snprintf(json, sizeof(json),
            "{\"ok\":false,\"error\":\"reset applied in RAM only -- flash write failed or could not "
            "be dispatched; lost on reboot unless a later persist succeeds (won't happen "
            "automatically in recovery mode)\",\"relay\":%ld}", relay);
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef RELAY_CYCLES_RESET_BODY_MAX

/* POST /api/relay_cycles/restore c0=N&c1=N&c2=N&c3=N&c4=N -- backup-gate pass
 * 2026-09-07 (docs/FILESYSTEM_PLAN.md runbook). full_board_backup.py already
 * captures these five counts as /api/status's relay_counts array; this is
 * the matching restore path, added because none existed. Same form-body
 * convention as relay_cycles_reset_post_handler() just above, one field per
 * RELAY_CYCLES_COUNT slot rather than a single index. All-or-nothing: a
 * missing/malformed field refuses the WHOLE request before relay_cycles_
 * restore_all() is even called, and relay_cycles_restore_all() itself
 * repeats the sanity-ceiling check on every value (see that function's own
 * comment) -- defence in depth, not redundant trust, since this handler
 * cannot see that ceiling constant without including relay_cycles.c. */
#define RELAY_CYCLES_RESTORE_BODY_MAX 128
static esp_err_t relay_cycles_restore_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > RELAY_CYCLES_RESTORE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[RELAY_CYCLES_RESTORE_BODY_MAX + 1];
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

    uint32_t counts[RELAY_CYCLES_COUNT];
    for (uint8_t r = 0; r < RELAY_CYCLES_COUNT; r++) {
        char field[8];
        snprintf(field, sizeof(field), "c%u", r);
        char val[16];
        int val_len = http_form_find_field(body, field, val, sizeof(val));
        if (val_len <= 0) {
            char err[32];
            snprintf(err, sizeof(err), "missing field \"%s\"", field);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
            return ESP_OK;
        }
        char *endp = NULL;
        unsigned long v = strtoul(val, &endp, 10);
        if (endp == val || *endp != '\0') {
            char err[48];
            snprintf(err, sizeof(err), "field \"%s\" is not a valid number", field);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
            return ESP_OK;
        }
        counts[r] = (uint32_t)v;
    }

    bool ok = relay_cycles_restore_all(counts);

    char json[192];
    int n;
    if (ok) {
        n = snprintf(json, sizeof(json), "{\"ok\":true}");
        httpd_resp_set_status(req, "200 OK");
    } else {
        n = snprintf(json, sizeof(json),
            "{\"ok\":false,\"error\":\"restore refused or persist failed -- see board log for which\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef RELAY_CYCLES_RESTORE_BODY_MAX

/* GET /api/ramp_assist -- current state of the kiln-wide ramp-assist toggle
 * (ramp_assist_cfg.h). Also carried in GET /api/status (dashboard_http.c)
 * for the persistent indicator; this endpoint exists so the diagnostics
 * page's own control doesn't need to depend on that other page's response
 * shape -- same split watchdog_cfg_get_handler()/GET /api/watchdog_cfg
 * already establishes just above. */
static esp_err_t ramp_assist_get_handler(httpd_req_t *req)
{
    char json[64];
    int n = snprintf(json, sizeof(json), "{\"enabled\":%s}",
                     ramp_assist_cfg_enabled() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* POST /api/ramp_assist -- body: enabled=0|1 (form-encoded, same convention
 * as watchdog_cfg_post_handler() just above and dashboard_http.c's other
 * small POST setters). Persists to NVS AND applies immediately -- see
 * ramp_assist_cfg_set_enabled(). This is the flag ONLY: no ramp-stretching or
 * dwell-credit behaviour lives behind this handler, that consumer lands
 * separately and simply reads ramp_assist_cfg_enabled() at decision time --
 * see ramp_assist_cfg.h's header comment for why the toggle and the behaviour
 * it will gate are deliberately split across separate work. */
#define RAMP_ASSIST_BODY_MAX 32
static esp_err_t ramp_assist_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > RAMP_ASSIST_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[RAMP_ASSIST_BODY_MAX + 1];
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
    int val_len = http_form_find_field(body, "enabled", val, sizeof(val));
    if (val_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"enabled\" field");
        return ESP_OK;
    }
    bool enabled = (val[0] == '1');

    esp_err_t err = ramp_assist_cfg_set_enabled(enabled);
    char json[96];
    int n;
    if (err == ESP_OK) {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"enabled\":%s}", enabled ? "true" : "false");
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef RAMP_ASSIST_BODY_MAX

/* GET /api/diagnostics/timing -- HW_ABSTRACTION.md "Still open": display
 * flush time and thermocouple read latency, previously un-verifiable because
 * the board exposed no timing metrics at all. Both blocks come from
 * single-writer volatile counters kept where the work actually happens
 * (lvgl_port.c's ili9488_flush_cb(), MAX31856.c's MAX31856_read_all()) --
 * this handler only reads and formats them, same "no allocation, no lock
 * held across a device transfer" shape those two already had. Fixed-size
 * stack buffer, well under the httpd task's stack budget (project_httpd_
 * stack_near_overflow: 64 B free was seen after real load once -- this is a
 * small snapshot of six uint32_t's, not a growing buffer). */
static esp_err_t diagnostics_timing_get_handler(httpd_req_t *req)
{
    uint32_t d_last, d_min, d_max, d_count, d_mean;
    lvgl_port_get_flush_stats_ex(&d_last, &d_min, &d_max, &d_count, &d_mean);

    uint32_t t_last, t_min, t_max, t_count, t_mean;
    MAX31856_get_read_all_stats(&t_last, &t_min, &t_max, &t_count, &t_mean);

    /* link_reply_us -- HW_ABSTRACTION.md "Still open", added 2026-09-06:
     * see safety_link_stats_t::link_reply_us_count's doc comment
     * (safety_link.h) for exactly what this spans and why it replaces the
     * pre-HAL paper figures/the contaminated MCP-timed safety_ping(). NULL-
     * tolerant like every other hardware read in this handler's siblings --
     * s_diag_safety is NULL until the safety link comes up (or if it never
     * does this boot), and this block just reports all zeros rather than
     * failing the whole response, same as every other block here would if
     * its own hardware were absent. */
    uint32_t l_last = 0, l_min = 0, l_max = 0, l_count = 0, l_mean = 0;
    uint32_t l_timeouts = 0;
    if (s_diag_safety) {
        safety_link_stats_t stats;
        if (safety_link_get_stats(s_diag_safety, &stats) == ESP_OK) {
            l_last = stats.link_reply_us_last;
            l_min = stats.link_reply_us_min;
            l_max = stats.link_reply_us_max;
            l_count = stats.link_reply_us_count;
            l_mean = stats.link_reply_us_mean;
            /* Not a separate counter -- reuses the existing `timeouts` field
             * (safety_link_stats_t's own doc comment on link_reply_us_count
             * explains why a second one would only duplicate it). */
            l_timeouts = stats.timeouts;
        }
    }

    char json[480];
    int n = snprintf(json, sizeof(json),
                     "{\"display_flush_us\":{\"count\":%lu,\"last\":%lu,\"min\":%lu,\"max\":%lu,"
                     "\"mean\":%lu},"
                     "\"thermo_read_us\":{\"count\":%lu,\"last\":%lu,\"min\":%lu,\"max\":%lu,"
                     "\"mean\":%lu},"
                     "\"link_reply_us\":{\"count\":%lu,\"last\":%lu,\"min\":%lu,\"max\":%lu,"
                     "\"mean\":%lu,\"timeouts\":%lu}}",
                     (unsigned long)d_count, (unsigned long)d_last, (unsigned long)d_min,
                     (unsigned long)d_max, (unsigned long)d_mean,
                     (unsigned long)t_count, (unsigned long)t_last, (unsigned long)t_min,
                     (unsigned long)t_max, (unsigned long)t_mean,
                     (unsigned long)l_count, (unsigned long)l_last, (unsigned long)l_min,
                     (unsigned long)l_max, (unsigned long)l_mean, (unsigned long)l_timeouts);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n < 0) ? 0 : (size_t)n);
}

/* --- danger_mode.h's diagnostics-page section: status/start/stop/relay --- */

/* GET /api/diagnostics/danger -- current window state, for the page's own
 * countdown. Server-computed remaining_ms on every call (danger_mode.h's own
 * contract) is what makes this survive a page refresh: the page has no
 * client-side deadline to lose, it just re-asks the board what's left. */
static esp_err_t danger_get_handler(httpd_req_t *req)
{
    bool active = danger_mode_active();
    bool relay_known, relay_energized, heating_known, heating_enabled;
    relay_known = danger_mode_get_relay_status(&relay_energized, &heating_enabled);
    heating_known = relay_known; /* one safety-link read fills both -- see danger_mode.h */
    /* heat_requested is this module's OWN outstanding request (ESP-local,
     * always known) -- the "Firing mode" tile must be driven from THIS, not
     * heating_enabled (SAFETY_FLAG_ENABLED, "SaftyFW armed", true on any
     * healthy Pico regardless of any request -- see danger_mode.h's
     * doc comments on both). 2026-08-27 bug fix. */
    bool heat_requested = danger_mode_get_heat_requested();
    char json[200];
    int n = snprintf(json, sizeof(json),
                     "{\"active\":%s,\"remaining_ms\":%lu,\"safety_relay_known\":%s,"
                     "\"safety_relay_energized\":%s,\"heating_enabled_known\":%s,\"heating_enabled\":%s,"
                     "\"heat_requested\":%s}",
                     active ? "true" : "false", (unsigned long)danger_mode_remaining_ms(),
                     relay_known ? "true" : "false", relay_energized ? "true" : "false",
                     heating_known ? "true" : "false", heating_enabled ? "true" : "false",
                     heat_requested ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* POST /api/diagnostics/danger/start -- body: accept=1 (required, same
 * belt-and-suspenders convention as watchdog_cfg_post_handler's "disabled"
 * field above): the page's own accept-risk checkbox already gates showing
 * this control, but a server-side action this consequential should not fire
 * off a bare POST with no explicit field naming what was agreed to. */
#define DANGER_START_BODY_MAX 32
static esp_err_t danger_start_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > DANGER_START_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[DANGER_START_BODY_MAX + 1];
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
    if (http_form_find_field(body, "accept", val, sizeof(val)) <= 0 || val[0] != '1') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"accept=1\"");
        return ESP_OK;
    }

    if (!danger_mode_request_start()) {
        /* No HTTPD_409_CONFLICT in this esp_http_server's httpd_err_code_t --
         * same manual-status pattern crash_report_ack_post_handler() above
         * already uses for its own 409. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "refused -- a firing is currently running or paused");
        return ESP_OK;
    }

    char json[80];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"remaining_ms\":%lu}",
                     (unsigned long)danger_mode_remaining_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef DANGER_START_BODY_MAX

/* POST /api/diagnostics/danger/stop -- operator-requested early exit, no
 * reboot (danger_mode.h's header comment on why this differs from timeout). */
static esp_err_t danger_stop_post_handler(httpd_req_t *req)
{
    danger_mode_stop("diagnostics page, operator request");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/diagnostics/danger/relay -- body: relay=1..KILN_IO_RELAY_COUNT,
 * on=0|1. Same field convention the old dashboard_http.c POST /api/relay
 * handler used (removed 2026-08-27 with manual_page.html, its only caller --
 * see dashboard_http.h's dashboard_relay_result_t comment), deliberately not
 * reused directly: this endpoint's whole reason to exist is
 * refusing up front (409) when danger mode is not active, rather than
 * silently falling through to the normal safety-gated path, so an operator
 * can never mistake "the section isn't armed" for "the relay refused to
 * move." The actual write goes through dashboard_set_relay() -- the same
 * one kiln_io_owner.c's relay_on_blocked() already bypasses for real when
 * danger_mode_active() is true, so this handler adds no second copy of that
 * logic, only the up-front check and the touch() that extends the window. */
#define DANGER_RELAY_BODY_MAX 32
static esp_err_t danger_relay_post_handler(httpd_req_t *req)
{
    if (!danger_mode_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "danger mode is not active");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > DANGER_RELAY_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[DANGER_RELAY_BODY_MAX + 1];
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

    uint32_t safety_sources = 0;
    dashboard_relay_result_t rr = dashboard_set_relay((uint8_t)relay, want_on, &safety_sources);
    if (rr != DASHBOARD_RELAY_OK) {
        /* Should not happen while danger mode is active -- relay_on_blocked()
         * skips every gate that could produce these -- except ERR_NO_BOARD
         * (no expander at all, unrelated to any gate) and ERR_RANGE (already
         * checked above, kept here only as defense in depth). Not extending
         * the window on a refusal: nothing about this section changed. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay write failed");
        return ESP_OK;
    }

    danger_mode_touch();
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"remaining_ms\":%lu}",
                     (unsigned long)danger_mode_remaining_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef DANGER_RELAY_BODY_MAX

/* POST /api/diagnostics/danger/enable -- body: on=0|1. Owner request
 * 2026-08-27: entering the section no longer auto-sends
 * SAFETY_CMD_REQUEST_ENABLE; this is the explicit, separate action that
 * does, shown in the page as one more tile in the relay grid. Same
 * up-front-409-when-not-active shape as danger_relay_post_handler() above,
 * for the same reason. */
#define DANGER_ENABLE_BODY_MAX 16
static esp_err_t danger_enable_post_handler(httpd_req_t *req)
{
    if (!danger_mode_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "danger mode is not active");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > DANGER_ENABLE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[DANGER_ENABLE_BODY_MAX + 1];
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

    char on_val[4];
    if (http_form_find_field(body, "on", on_val, sizeof(on_val)) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on missing");
        return ESP_OK;
    }
    bool want_on = on_val[0] == '1';

    if (!danger_mode_set_heat_enable_request(want_on)) {
        /* Window closed between the active check above and here (a racing
         * timeout) -- vanishingly unlikely at 1s poll granularity, but a
         * real 409 rather than a silently-ignored request either way. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "danger mode is not active");
        return ESP_OK;
    }

    char json[80];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"remaining_ms\":%lu}",
                     (unsigned long)danger_mode_remaining_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef DANGER_ENABLE_BODY_MAX

/* GET /api/cfgfs -- observability for the `cfg` LittleFS partition
 * (docs/FILESYSTEM_USER_DATA_PLAN.md). The `cfg` partition is invisible
 * otherwise: mounted or not, how full, what's on it, and whether the
 * zones-config dual-write's file and NVS copies agree are all things that
 * were previously only findable by grepping the boot log. Most of the work
 * is in cfg_fs_status.c (pure, host-tested against a real temp directory);
 * this handler supplies the two things that module deliberately does NOT
 * know how to get itself -- LittleFS capacity (esp_littlefs_info(), ESP-IDF
 * only) and the zones NVS rev counter (own copy of the "zones_rev" key,
 * same duplication precedent as every other *_http.c file in this
 * component that reads one small NVS value for a status page rather than
 * pulling in zones_config_store.c's whole surface). */
#define CFGFS_NVS_NAMESPACE "kiln_cfg"
#define CFGFS_NVS_PARTITION "kiln_nvs"
#define CFGFS_NVS_KEY_ZONES_REV "zones_rev"

static uint32_t cfgfs_read_zones_nvs_rev(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, CFGFS_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, CFGFS_NVS_PARTITION);
    if (err != HAL_OK) {
        return 0;
    }
    uint32_t rev = 0;
    err = hal_kv_get_u32(&h, CFGFS_NVS_KEY_ZONES_REV, &rev);
    hal_kv_close(&h);
    return err == HAL_OK ? rev : 0;
}

/* This handler's locals used to live on the httpd task stack: a 2048-byte
 * JSON buffer plus a whole zones_cfg_t (~1.6-1.7 KB, dominated by
 * zones[MAX31856_CHANNEL_COUNT] and timing_profiles[MAX31856_CHANNEL_COUNT])
 * -- roughly 3.7 KB in this frame alone, on a task whose MEASURED
 * worst-case margin is 64 B (see CLAUDE.md's "httpd stack" note and
 * docs/audits/filesystem_migration_review_2026-09-07.md finding #1). That
 * is on top of cfg_fs_status_build_json()'s own frame just below it, which
 * separately stack-allocated two 32-entry cfg_fs_entry_t arrays (see that
 * function). Both are moved to the heap: a single request-scoped buffer
 * pool allocated up front and freed on every return path, so nothing this
 * handler needs ever lands on the task's own stack. malloc() failure (heap
 * pressure, not stack) is reported as 500 rather than silently truncating,
 * same convention as the JSON-build failure path below.
 *
 * BUFFER SIZE (2026-09-08 widening: per-item dual-write rows for every
 * bridge, not zones only; WIDENED AGAIN same day when relay_cycles/
 * adaptive_tune/firing_stats moved off the stale nvs_only list): worst case
 * is now 17 item rows (zones, kiln_cfg_store, 4 pref-backed items,
 * PROFILES_MAX_COUNT=8 profile slots, relay_cycles, adaptive_tune,
 * firing_stats) at up to ~120 bytes each (longest name "display_power" and
 * "firing_stats" -- both under the same 120 B/row estimate, both revs at
 * UINT32_MAX) = 17 * 120 = 2040 bytes for the items array alone (was ~1700 B
 * for 14 items), plus the pre-existing sections (header/capacity/format
 * ~300 B typical, nvs_only/nvs_permanent name lists ~300 B fixed now that
 * nvs_only is an empty array instead of three names, files[] typically a
 * handful of entries in real use though pathologically up to
 * CFG_FS_STATUS_MAX_FILES=32 max-length names could itself exceed any
 * reasonable buffer -- that pre-existing limit is unchanged by this pass).
 * 2040 + 300 + 300 = ~2640 B worst case; 3072 already covered the old 14-item
 * worst case (~2330 B) with ~740 B headroom, so the new ~2640 B worst case
 * still fits under 3072 with ~430 B headroom to spare -- NOT raised this
 * pass. cfg_fs_status_build_json() still fails loudly with
 * ESP_ERR_INVALID_SIZE rather than truncating if a pathological files[]
 * list (or a future item count) ever pushes past it. */
typedef struct {
    zones_cfg_t raw;
    char json[3072];
} cfgfs_status_scratch_t;

/* Fills one row of the /api/cfgfs dual-write item list and advances *n. A
 * full array (n == CFG_FS_STATUS_MAX_ITEMS) silently drops further rows --
 * see cfg_fs_status.h's CFG_FS_STATUS_MAX_ITEMS comment; today's fixed set
 * of 14 items sits well under that cap. */
static void cfgfs_add_item_ex(cfg_fs_dualwrite_item_t *items, size_t *n, const char *name, bool file_valid,
                               uint32_t file_rev, bool nvs_valid, uint32_t nvs_rev, bool diverged,
                               bool migration_deferred)
{
    if (*n >= CFG_FS_STATUS_MAX_ITEMS) {
        return;
    }
    items[*n] = (cfg_fs_dualwrite_item_t){
        .name = name, .file_valid = file_valid, .file_rev = file_rev, .nvs_valid = nvs_valid, .nvs_rev = nvs_rev,
        .diverged = diverged, .migration_deferred = migration_deferred,
    };
    (*n)++;
}

static void cfgfs_add_item(cfg_fs_dualwrite_item_t *items, size_t *n, const char *name, bool file_valid,
                            uint32_t file_rev, bool nvs_valid, uint32_t nvs_rev, bool diverged)
{
    cfgfs_add_item_ex(items, n, name, file_valid, file_rev, nvs_valid, nvs_rev, diverged, false);
}

static esp_err_t cfgfs_status_get_handler(httpd_req_t *req)
{
    bool mounted = cfg_fs_is_available();

    cfg_fs_capacity_info_t cap = { .known = false };
    if (mounted) {
        size_t total = 0, used = 0;
        if (esp_littlefs_info("cfg", &total, &used) == ESP_OK) {
            cap.known = true;
            cap.total_bytes = total;
            cap.used_bytes = used;
        }
    }

    cfgfs_status_scratch_t *s = malloc(sizeof(*s));
    if (!s) {
        ESP_LOGE(TAG, "cfgfs_status_get_handler: malloc(%u) failed", (unsigned)sizeof(*s));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    /* One dual-write row per migrated item across every persist/'*'_cfg_fs.c
     * bridge (70ed6514 fixed /api/cfgfs's stale lists but left this detail
     * wired up for zones only -- widened here; 2026-09-08 widened again to
     * cover relay_cycles/adaptive_tune/firing_stats, the last three items
     * docs/FILESYSTEM_USER_DATA_PLAN.md section 5 tracked). Each bridge's
     * own module reads its OWN NVS rev key and computes `diverged` itself
     * via cfg_fs_status_item_diverged() (a real decoded-content compare, not
     * a rev-only guess) -- this handler just collects what they report. */
    cfg_fs_dualwrite_item_t items[CFG_FS_STATUS_MAX_ITEMS];
    size_t n_items = 0;

    /* zones -- kept as the PRE-EXISTING approximation (rev comparison, not a
     * decoded-content compare): zones_config_cfg_fs.c's resolve() already
     * exists and does the real compare, but it also performs resync WRITES
     * as a side effect (see its own doc comment), which a status GET must
     * never trigger -- repeated polling would wear the flash and could mask
     * a real divergence by silently healing it before an operator sees it.
     * Doing a true read-only content compare here would mean decoding the
     * NVS zones blob a second time in this handler; left as a known,
     * documented gap rather than duplicating zones_config_json_decode_blob's
     * call site under time pressure -- the other bridges below DO get the
     * real compare, since their status accessors are read-only by
     * construction. */
    {
        bool file_valid = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        zones_config_cfg_fs_load_raw(&s->raw, &file_rev, &file_valid);
        nvs_rev = cfgfs_read_zones_nvs_rev();
        bool diverged = file_valid && (nvs_rev > file_rev);
        cfgfs_add_item(items, &n_items, "zones", file_valid, file_rev, true, nvs_rev, diverged);
    }

    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        kiln_cfg_store_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "kiln_cfg_store", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        unit_pref_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "unit_pref", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        ramp_assist_cfg_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "ramp_assist", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        display_power_cfg_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "display_power", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        time_sync_get_tz_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "tz", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }
    static const char *const profile_names[PROFILES_MAX_COUNT] = {
        "profile0", "profile1", "profile2", "profile3", "profile4", "profile5", "profile6", "profile7",
    };
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        profiles_http_get_dualwrite_status(id, &file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, profile_names[id], file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }

    /* 2026-09-08: the three items that used to be reported via the stale
     * "nvs_only" hardcoded list (cfg_fs_status.c) now have real
     * pref_cfg_fs.c/firing_stats_cfg_fs.c-backed bridges (762bb29e) -- moved
     * here so /api/cfgfs stops claiming they are unmigrated. */
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item_ex(items, &n_items, "relay_cycles", file_valid, file_rev, nvs_valid, nvs_rev, diverged,
                          relay_cycles_migration_worker_wait_deferred());
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        adaptive_tune_get_kibase_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item_ex(items, &n_items, "adaptive_tune", file_valid, file_rev, nvs_valid, nvs_rev, diverged,
                          adaptive_tune_kibase_migration_worker_wait_deferred());
    }
    {
        bool file_valid = false, nvs_valid = false, diverged = false;
        uint32_t file_rev = 0, nvs_rev = 0;
        firing_stats_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
        cfgfs_add_item(items, &n_items, "firing_stats", file_valid, file_rev, nvs_valid, nvs_rev, diverged);
    }

    /* Deferred auto-format progress (cfg_fs_mount.c) -- ESP-IDF-only getters,
     * so the picture is assembled here rather than inside the pure
     * cfg_fs_status.c module (see cfg_fs_status.h's cfg_fs_format_progress_t
     * comment). known=false is correct and common: most boots either mount
     * cleanly or never trigger the deferred task at all. */
    cfg_fs_format_progress_t fmt = { .known = cfg_fs_mount_format_ever_started() };
    if (fmt.known) {
        fmt.in_progress = cfg_fs_mount_format_in_progress();
        fmt.completed = cfg_fs_mount_format_completed();
        fmt.result = cfg_fs_mount_format_result();
        fmt.succeeded = fmt.completed && (fmt.result == ESP_OK);
        fmt.elapsed_ms = cfg_fs_mount_format_elapsed_ms();
    }

    size_t len = 0;
    esp_err_t err = cfg_fs_status_build_json(mounted ? "/cfg" : NULL, &cap, items, n_items, &fmt, s->json,
                                              sizeof(s->json), &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_fs_status_build_json() failed: %s (buffer too small?)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status build failed");
        free(s);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, s->json, len);
    free(s);
    return send_err;
}

/* GET /api/cfgfs/file?name=<name> and POST /api/cfgfs/file?name=<name> --
 * full_board_backup.py's filesystem-coverage addition (docs/FILESYSTEM_PLAN.md
 * "Add filesystem coverage to the backup"). /api/cfgfs above already lists
 * every file cfg_fs holds, with sizes -- this pair is deliberately NOT a
 * second listing surface, just the one primitive that was missing: fetch (or
 * restore) one named file's exact bytes, opaque to this handler. Every file
 * `cfg_fs` holds is already whatever byte layout its own writer chose
 * (pref_cfg_fs.c's 4-byte-rev-prefixed blobs, zones_config_cfg_fs.c's raw
 * JSON, ...) -- this endpoint never parses any of that, it only round-trips
 * raw bytes through cfg_fs_read()/cfg_fs_write_atomic(), so a backup/restore
 * of the filesystem does not need to know (or keep in sync with) any single
 * file's internal format. */
#define CFGFS_FILE_NAME_MAX CFG_FS_MAX_NAME
#define CFGFS_FILE_BODY_MAX 8192 /* generous headroom over any one cfg file this codebase writes today */

static bool cfgfs_file_name_get(httpd_req_t *req, char *name, size_t name_cap)
{
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "name", name, name_cap) != ESP_OK) {
        return false;
    }
    /* Same "no directory component, no traversal" contract cfg_fs_entry_t's
     * own name field carries (CFG_FS_MAX_NAME, bare filename) -- refuse
     * anything that could walk outside the `cfg` mount root before it ever
     * reaches cfg_fs_read()/cfg_fs_write_atomic(). */
    if (name[0] == '\0' || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        return false;
    }
    return true;
}

static esp_err_t cfgfs_file_get_handler(httpd_req_t *req)
{
    char name[CFGFS_FILE_NAME_MAX];
    if (!cfgfs_file_name_get(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/invalid \"name\"");
        return ESP_OK;
    }
    if (!cfg_fs_is_available()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "cfg filesystem not mounted");
        return ESP_OK;
    }
    /* HEAP in PSRAM, not internal DRAM nor this handler task's own stack --
     * same reasoning as backup_export.c's BACKUP_STREAM_BUF allocation and
     * this file's own httpd DRAM-pressure discipline elsewhere; 8KB is far
     * too large to put on a task stack in a codebase with a documented
     * stack-overflow bricking history (see CLAUDE.md's boot_guard section). */
    uint8_t *buf = heap_caps_malloc(CFGFS_FILE_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(name, buf, CFGFS_FILE_BODY_MAX, &len);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found or unreadable");
        return ESP_OK;
    }
    /* Raw bytes, not JSON -- full_board_backup.py base64-encodes this body
     * itself for the archive. Keeping this endpoint's own wire format
     * exactly the file's bytes (no base64, no envelope) means a restore
     * writes back with zero decode/re-encode risk of its own; only the PC
     * script's archive format needs base64, because JSON cannot hold
     * arbitrary binary. */
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t send_err = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return send_err;
}

typedef struct {
    const char *name;
    const uint8_t *bytes;
    size_t len;
    esp_err_t result;
} cfgfs_file_write_job_t;

static void cfgfs_file_write_job(void *arg)
{
    cfgfs_file_write_job_t *job = (cfgfs_file_write_job_t *)arg;
    job->result = cfg_fs_write_atomic(job->name, job->bytes, job->len);
}

extern esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);
extern bool uart_bridge_ext_is_on_flash_worker(void);

static esp_err_t cfgfs_file_post_handler(httpd_req_t *req)
{
    char name[CFGFS_FILE_NAME_MAX];
    if (!cfgfs_file_name_get(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/invalid \"name\"");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > CFGFS_FILE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    if (!cfg_fs_is_available()) {
        /* No HTTPD_409_CONFLICT in this esp_http_server's httpd_err_code_t --
         * same manual-status pattern danger_start_post_handler() above
         * already uses for its own 409. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "cfg filesystem not mounted");
        return ESP_OK;
    }
    /* INTERNAL DRAM, deliberately NOT PSRAM (unlike the GET handler's read
     * buffer above): this buffer is the write SOURCE for cfg_fs_write_atomic()
     * dispatched onto the flash worker below, and a flash operation disables
     * the cache, making PSRAM unreachable for the duration -- see
     * flash_worker.h's own HAZARD comment and relay_cycles_reset()'s
     * reset_persist_job_arg_t snapshot (a stack-local struct, never PSRAM)
     * for the precedent this follows. Freed well before the response is
     * sent, so this is a transient 8KB internal allocation, not a standing
     * one. */
    uint8_t *body = heap_caps_malloc(CFGFS_FILE_BODY_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, (char *)body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }

    /* Same flash-worker dispatch shape relay_cycles_reset()/factory_reset.c
     * use -- this httpd handler task's own stack is not the internal-SRAM
     * worker stack a flash write requires (see flash_worker.h's HAZARD
     * comment), so the write always runs on the worker; the
     * is_on_flash_worker() check exists only so this same function would
     * still be safe to call FROM the worker itself, which never happens on
     * this particular route today. */
    cfgfs_file_write_job_t job = { .name = name, .bytes = body, .len = received, .result = ESP_FAIL };
    if (uart_bridge_ext_is_on_flash_worker()) {
        cfgfs_file_write_job(&job);
    } else {
        esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(cfgfs_file_write_job, &job);
        if (submit_err != ESP_OK) {
            job.result = submit_err;
        }
    }
    free(body);

    char json[128];
    int n;
    if (job.result == ESP_OK) {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"name\":\"%s\",\"size_bytes\":%u}", name, (unsigned)received);
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(job.result));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
#undef CFGFS_FILE_BODY_MAX

esp_err_t diagnostics_http_start(SafetyLinkClass *safety)
{
    s_diag_safety = safety;
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t diagnostics_uri = {
        .uri = "/diagnostics", .method = HTTP_GET, .handler = diagnostics_page_get_handler,
    };
    static const httpd_uri_t safety_uri = {
        .uri = "/safety", .method = HTTP_GET, .handler = safety_page_get_handler,
    };
    static const httpd_uri_t thermo_faults_api_uri = {
        .uri = "/api/thermo/faults", .method = HTTP_GET, .handler = thermo_faults_get_handler,
    };
    static const httpd_uri_t crash_report_api_uri = {
        .uri = "/api/crash_report", .method = HTTP_GET, .handler = crash_report_get_handler,
    };
    static const httpd_uri_t cfgfs_status_api_uri = {
        .uri = "/api/cfgfs", .method = HTTP_GET, .handler = cfgfs_status_get_handler,
    };
    static const httpd_uri_t cfgfs_file_get_uri = {
        .uri = "/api/cfgfs/file", .method = HTTP_GET, .handler = cfgfs_file_get_handler,
    };
    static const httpd_uri_t cfgfs_file_post_uri = {
        .uri = "/api/cfgfs/file", .method = HTTP_POST, .handler = cfgfs_file_post_handler,
    };
    static const httpd_uri_t crash_report_ack_uri = {
        .uri = "/api/crash_report/ack", .method = HTTP_POST, .handler = crash_report_ack_post_handler,
    };
    static const httpd_uri_t crash_report_clear_uri = {
        .uri = "/api/crash_report/clear", .method = HTTP_POST, .handler = crash_report_clear_post_handler,
    };
    static const httpd_uri_t lwip_stats_get_uri = {
        .uri = "/api/debug/lwip_stats", .method = HTTP_GET, .handler = lwip_stats_get_handler,
    };
    static const httpd_uri_t diagnostics_timing_get_uri = {
        .uri = "/api/diagnostics/timing", .method = HTTP_GET, .handler = diagnostics_timing_get_handler,
    };
    static const httpd_uri_t watchdog_cfg_get_uri = {
        .uri = "/api/watchdog_cfg", .method = HTTP_GET, .handler = watchdog_cfg_get_handler,
    };
    static const httpd_uri_t watchdog_cfg_post_uri = {
        .uri = "/api/watchdog_cfg", .method = HTTP_POST, .handler = watchdog_cfg_post_handler,
    };
    static const httpd_uri_t ramp_assist_get_uri = {
        .uri = "/api/ramp_assist", .method = HTTP_GET, .handler = ramp_assist_get_handler,
    };
    static const httpd_uri_t ramp_assist_post_uri = {
        .uri = "/api/ramp_assist", .method = HTTP_POST, .handler = ramp_assist_post_handler,
    };
    static const httpd_uri_t relay_cycles_reset_uri = {
        .uri = "/api/relay_cycles/reset", .method = HTTP_POST, .handler = relay_cycles_reset_post_handler,
    };
    static const httpd_uri_t relay_cycles_restore_uri = {
        .uri = "/api/relay_cycles/restore", .method = HTTP_POST, .handler = relay_cycles_restore_post_handler,
    };
    static const httpd_uri_t danger_get_uri = {
        .uri = "/api/diagnostics/danger", .method = HTTP_GET, .handler = danger_get_handler,
    };
    static const httpd_uri_t danger_start_uri = {
        .uri = "/api/diagnostics/danger/start", .method = HTTP_POST, .handler = danger_start_post_handler,
    };
    static const httpd_uri_t danger_stop_uri = {
        .uri = "/api/diagnostics/danger/stop", .method = HTTP_POST, .handler = danger_stop_post_handler,
    };
    static const httpd_uri_t danger_relay_uri = {
        .uri = "/api/diagnostics/danger/relay", .method = HTTP_POST, .handler = danger_relay_post_handler,
    };
    static const httpd_uri_t danger_enable_uri = {
        .uri = "/api/diagnostics/danger/enable", .method = HTTP_POST, .handler = danger_enable_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &diagnostics_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/diagnostics) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &safety_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/safety) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &thermo_faults_api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/thermo/faults) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &crash_report_api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &cfgfs_status_api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/cfgfs) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &cfgfs_file_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/cfgfs/file) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &cfgfs_file_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/cfgfs/file) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &lwip_stats_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/debug/lwip_stats) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &diagnostics_timing_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diagnostics/timing) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &crash_report_ack_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report/ack) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &crash_report_clear_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report/clear) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &watchdog_cfg_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/watchdog_cfg) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &watchdog_cfg_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/watchdog_cfg) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &ramp_assist_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/ramp_assist) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &ramp_assist_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/ramp_assist) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &relay_cycles_reset_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/relay_cycles/reset) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &relay_cycles_restore_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/relay_cycles/restore) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &danger_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/diagnostics/danger) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &danger_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diagnostics/danger/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &danger_stop_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diagnostics/danger/stop) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &danger_relay_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diagnostics/danger/relay) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &danger_enable_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diagnostics/danger/enable) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "diagnostics/safety pages up (thermo-faults API still served, its page folded into /diagnostics)");
    return ESP_OK;
}
