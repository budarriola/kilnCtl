/* GET /api/status -- moved out of dashboard_http.c 2026-09-04 (ROADMAP.md
 * M15, the 1500-line rule; dashboard_http.c had grown to 2576 lines). This
 * is the biggest single handler in the original file (json_f() plus
 * status_get_handler(), the biggest piece of the split) so it gets its own
 * file. See dashboard_http_internal.h for the shared s_dash/DASH_TAG seam
 * and why this function is dashboard_status_get_handler(), not
 * status_get_handler() -- two OTHER static status_get_handler()s already
 * exist in App/drivers (adaptive_tune_http.c, wifi_provision_http.c). */

#include "dashboard_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "boot_button.h"
#include "lvgl_port.h"
#include "dashboard_json.h"
#include "kiln_io_owner.h"
#include "nvs_report.h"
#include "ota_http.h"
#include "relay_authority.h"
#include "time_sync.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "safety_cfg_store.h" /* CT_COMMISSIONING_PLAN.md step 4 -- ct_topology (0x031F) */
#include "safety_trip_words.h"
#include "sim_backend.h"
#include "uart_task_ids.h"
#include "unit_pref.h"
#include "watchdog_cfg.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h" /* CT_COMMISSIONING_PLAN.md step 4 -- relay_mask, for
                                     * single-zone attribution in summed CT mode */

/* CT_COMMISSIONING_PLAN.md step 4 -- reads the committed ct_topology (param
 * 0x031F, U8, 0=per_zone/1=summed) out of the ESP's own cached safety-cfg
 * params. Same "unset reads as per_zone" convention as zones_current_sweep_
 * task.c's zone_cfg_committed_ct_topology() (that one is private to the
 * sweep task and lives in App/drivers/control, not something this file may
 * reach into) -- duplicated here rather than shared because the two callers
 * are in different modules with no existing shared seam, and this is a
 * three-line linear scan over a table capped at SAFETY_CFG_PARAM_COUNT rows,
 * not logic worth a new header for. */
static bool dashboard_ct_topology_is_summed(void)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != 0x031Fu) {
            continue;
        }
        return row.set && row.value.u8_val != 0u;
    }
    return false;
}

/* CT_COMMISSIONING_PLAN.md step 4 -- in summed-CT mode the single shared
 * channel's reading can only be attributed to one zone's dashboard tile when
 * exactly one zone is presently commanded on (two or more zones on means the
 * reading is a mix nobody can separate back out). Returns the 0-based zone
 * index when exactly one qualifies, or -1 (no zones on, or more than one).
 * "Commanded on" here is read the same way the /api/status relays[] array
 * above already reports actual relay state -- ds->relay_on[], not a firing's
 * zone-status struct, so this also works with relays flipped by hand outside
 * any profile run. A zone whose relay_mask is 0 (nothing wired) never
 * qualifies as "on". */
static int dashboard_ct_summed_attrib_zone(const dashboard_status_t *ds)
{
    if (!ds->io_ready) {
        return -1;
    }
    int found = -1;
    int on_count = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t mask = 0;
        if (!zones_config_get_relay_mask(zi, &mask) || mask == 0u) {
            continue;
        }
        bool zone_on = false;
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            if ((mask & (1u << (relay - 1))) != 0u && ds->relay_on[relay - 1]) {
                zone_on = true;
                break;
            }
        }
        if (zone_on) {
            on_count++;
            found = (int)zi;
        }
    }
    return (on_count == 1) ? found : -1;
}

/* JSON has no way to spell a NaN or an infinity. printf spells them "nan" and
 * "inf", which are bare identifiers, so a single non-finite float turns the
 * whole response into a document JSON.parse() rejects -- and main_page.html's
 * poll() swallows that in an empty .catch(), leaving the panel on "Loading..."
 * with no visible error. This is the same failure the buffer-size comment
 * below describes, reached by a different route.
 *
 * Two producers here really do hand us NaN: the safety link initialises
 * trip_safety_tc_c/trip_deciding_threshold to NAN (safety_link.c), so any trip
 * whose reason carries no temperature -- S6a, the link/main-controller faults
 * -- reports one; and MAX31856.c leaves temp_c/cj_c NaN on a faulted or
 * open-circuit channel, so unplugging a thermocouple did it too.
 *
 * Emit null instead, which is what every other "we do not have this reading"
 * field on this endpoint already emits. */
static const char *json_f(char *buf, size_t buf_len, const char *fmt, float v)
{
    if (!isfinite(v)) {
        return "null";
    }
    int n = snprintf(buf, buf_len, fmt, (double)v);
    if (n < 0 || (size_t)n >= buf_len) {
        return "null";
    }
    return buf;
}

esp_err_t dashboard_status_get_handler(httpd_req_t *req)
{
    /* SUPERSEDED 2026-09-02: the "4096... headroom of ~100B... fits at 4096"
     * paragraph below was a one-off manual measurement, never a check --
     * opus review. It is now: dashboard_json.h's DASHBOARD_JSON_STATUS_
     * BUF_SIZE (this file's DASHBOARD_STATUS_JSON_BUF_SIZE is that same
     * macro, aliased so the rest of this function needed no other edit),
     * with test_dashboard_json.c's
     * test_status_json_worst_case_render_fits_documented_buffer() rendering
     * every field below at its real worst width (calling the SAME helper
     * functions this handler calls -- json_escape(), safety_trip_words_*(),
     * safety_fault_source_words() -- not reimplementing them) and asserting
     * the result against the buffer, plus two mutation tests proving that
     * check can actually go red (a shrunk buffer, and a field added without
     * a size bump). Running that check found the paragraph below's hand sum
     * was itself wrong by 99 bytes -- see dashboard_json.h's own comment on
     * DASHBOARD_JSON_STATUS_BUF_SIZE for the corrected number (4480, not
     * 4224) and why. The field-by-field prose below is kept for the
     * per-field reasoning it still gets right, not for its arithmetic. */

    /* 2026-08-28 (live regression, same day as the fault-cause/remedy pass
     * that caused it): bumped from 1700 to 2200/2300/2500 previously -- see
     * git history for that running tally -- but the numbered-cause field
     * added earlier today (trip_reason_cause via safety_trip_words_cause_
     * numbered(), cause_buf[320] below) plus diag_trip_reason_cause/_remedy,
     * trip_reason_remedy, heat_block_sources_words and trip_fault_sources_
     * words were never folded into that accounting, and 2500 was too small
     * the moment several were populated at once -- confirmed on the bench
     * (GET /api/status 500, "did not fit in 2500 bytes").
     *
     * JSON_BUF_SIZE (4096) is a field-by-field worst-case sum, not a rounder
     * number picked to make the incident go away: skeleton + numeric fields
     * (~900B with every counter at its u32/negative-float widest), 3 thermo
     * channels at their widest (~130B each), diag_trip_reason_cause/_remedy
     * at their longest table entries (83+112B), trip_reason_cause at its
     * FULL cause_buf[320]-1 capacity (the dominant term -- see that buffer's
     * own sizing comment below for why a hostile/uncommissioned current
     * reading can fill it), trip_reason_remedy (112B), heat_block_sources_
     * words/trip_fault_sources_words at their own 160-byte buffers' full
     * capacity (159B each), safety_build_commit/_datetime escaped WORST CASE
     * (every byte needing a backslash doubles -- 65/33-byte raw fields from
     * the UNTRUSTED RP2040 peer, so 130/66B), fw_version/fw_build escaped
     * similarly (62/78B), 3 nvs_sections entries, ,"thermo_spi_wedged":false (26B,
     * opus review, commit 9fc55d9, M5), plus ~104B worst case for the
     * heap_dma object added by DRAM_PSRAM_PLAN.md Phase 0 (4.1) --
     * `,"heap_dma":{"free":%lu,"largest_free_block":%lu,"min_free":%lu,
     * "total":%lu}` at 64 literal bytes plus 4 uint32_t fields at their
     * 10-digit widest -- and headroom of ~100B on top of the ~3996-byte
     * total this exact field list now sums to (verified by a standalone
     * harness mirroring this file's own APPEND macro against every field
     * above at its documented worst width: fits at 4096, and provably
     * truncates -- the `goto truncated` path fires -- once the same content
     * is asked to fit in a materially smaller buffer, proving this is a real
     * bound rather than a round number). Headroom shrank from >200B to
     * ~100B when heap_dma was added. DISPLAY_ST7796_PLAN.md 9.1 added
     * `,"flush_last_us":%u,"flush_max_us":%u,"flush_count":%u` (~81B worst
     * case: 3 literal prefixes at 18/17/16B plus 3 uint32_t fields at their
     * 10-digit widest) on top of that ~100B headroom -- too tight by this
     * file's own rule of thumb, so the buffer grew 4096 -> 4224 in the same
     * change rather than letting headroom go to ~19B. If another field is
     * ever added here, re-run that harness before assuming 4224 still fits.
     *
     * HEAP, not stack: httpd worker stack high-water mark was measured at
     * 2348 bytes free of 8192 on this exact endpoint (owner report,
     * 2026-08-28) -- BEFORE this fix. The old 2500-byte `char json[2500]`
     * was already stacked alongside cause_buf[320]/hb_words[160]/
     * tf_words[160]/the escape buffers below (~3.5KB of locals total), which
     * is consistent with that measurement being this close to the edge.
     * Growing json to 4096 ON THE STACK would make an already-tight worker
     * stack worse, not better -- the actual fix for the DRAM-fragmentation
     * failure mode this file's own ui_page_diagnostics.c comment documents
     * elsewhere. Heap-allocated instead: freed on every return path below
     * (success, truncated, and the new malloc-failure path), same
     * "diagnosable 500, never a hang" property truncated: already has. */
    /* The actual size lives in dashboard_json.h's DASHBOARD_JSON_STATUS_BUF_
     * SIZE now (moved there so test_dashboard_json.c can reach it -- see
     * that macro's own doc comment) -- this local name is kept so every
     * other reference in this function below did not need touching. */
#define DASHBOARD_STATUS_JSON_BUF_SIZE DASHBOARD_JSON_STATUS_BUF_SIZE
    char *json = heap_caps_malloc(DASHBOARD_STATUS_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/status: malloc(%u) failed for the response buffer",
                 (unsigned)DASHBOARD_STATUS_JSON_BUF_SIZE);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, DASHBOARD_STATUS_JSON_BUF_SIZE - o, __VA_ARGS__);                 \
        if (n < 0 || (size_t)n >= DASHBOARD_STATUS_JSON_BUF_SIZE - o) {                           \
            goto truncated;                                                                       \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    /* opus review: dashboard_status_t grew ~124B with relay_life[5]/
     * relay_life_type[5]/relay_life_tier (RELAY_LIFE_BUDGET.md)
     * and now runs well past what an already-tight httpd worker stack can
     * absorb -- 64 bytes free was MEASURED under load on this exact worker
     * (project_httpd_stack_near_overflow note) even before this struct grew.
     * Heap-allocated for the same reason `json` above is: freed on every
     * return path below (success, truncated, and this malloc-failure path). */
    dashboard_status_t *ds = heap_caps_malloc(sizeof(*ds), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ds == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/status: malloc(%u) failed for the status snapshot",
                 (unsigned)sizeof(*ds));
        free(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    dashboard_get_status(ds);

    APPEND("{\"io_ready\":%s", ds->io_ready ? "true" : "false");

    if (ds->io_ready) {
        APPEND(",\"relays\":[");
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            bool on = ds->relay_on[relay - 1];
            APPEND("%s{\"relay\":%u,\"on\":%s}", relay == 1 ? "" : ",", relay, on ? "true" : "false");
        }
        APPEND("]");
        if (ds->io_read_failed) {
            APPEND(",\"io_read_failed\":true");
        }
    }

    APPEND(",\"relay_cycles\":[");
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        APPEND("%s%lu", r == 0 ? "" : ",", (unsigned long)ds->relay_cycles[r]);
    }
    APPEND("]");

    /* RELAY_LIFE_BUDGET.md: budget state for all five counted
     * slots (four heater relays + the safety relay, RELAY_CYCLES_SAFETY_INDEX),
     * plus the overall tier the LCD/web indication gates on. percent/rated
     * are JSON null for an SSR-typed relay (has_budget false) rather than 0,
     * same "null means never/not-applicable" convention as age_ms/temps
     * elsewhere on this endpoint. */
    APPEND(",\"relay_life\":[");
    for (uint8_t r = 0; r < RELAY_CYCLES_COUNT; r++) {
        const relay_cycles_budget_t *b = &ds->relay_life[r];
        const char *type_str;
        switch (ds->relay_life_type[r]) {
        case RELAY_TYPE_CONTACTOR: type_str = "contactor"; break;
        case RELAY_TYPE_MERCURY:   type_str = "mercury"; break;
        default:                   type_str = "ssr"; break;
        }
        const char *tier_str;
        switch (b->tier) {
        case RELAY_BUDGET_TIER_WARN:  tier_str = "warn"; break;
        case RELAY_BUDGET_TIER_ERROR: tier_str = "error"; break;
        default:                      tier_str = "none"; break;
        }
        char rated_buf[16];
        char percent_buf[16];
        if (b->has_budget) {
            snprintf(rated_buf, sizeof(rated_buf), "%lu", (unsigned long)b->rated);
            snprintf(percent_buf, sizeof(percent_buf), "%.2f", (double)b->percent);
        } else {
            snprintf(rated_buf, sizeof(rated_buf), "null");
            snprintf(percent_buf, sizeof(percent_buf), "null");
        }
        APPEND("%s{\"relay\":%u,\"type\":\"%s\",\"cycles\":%lu,\"rated\":%s,\"percent\":%s,\"tier\":\"%s\"}",
               r == 0 ? "" : ",", r, type_str, (unsigned long)b->cycles, rated_buf, percent_buf, tier_str);
    }
    APPEND("]");
    {
        const char *overall_tier_str;
        switch (ds->relay_life_tier) {
        case RELAY_BUDGET_TIER_WARN:  overall_tier_str = "warn"; break;
        case RELAY_BUDGET_TIER_ERROR: overall_tier_str = "error"; break;
        default:                      overall_tier_str = "none"; break;
        }
        APPEND(",\"relay_life_tier\":\"%s\"", overall_tier_str);
    }

    APPEND(",\"thermo_ready\":%s", ds->thermo_ready ? "true" : "false");
    /* opus review, commit f3a1600, G2b: operator-visible surface for the
     * shared SPI owner's wedged latch -- see dashboard_http.h's field
     * comment. Reported unconditionally (not gated behind thermo_ready)
     * since a wedged owner also takes the display down with it. */
    APPEND(",\"thermo_spi_wedged\":%s", ds->thermo_spi_wedged ? "true" : "false");
    /* DISPLAY_ST7796_PLAN.md 9.1 capture procedure: `curl .../api/status |
     * jq .flush_max_us` (or flush_last_us for the most recent one) is now
     * the whole bench step -- no separate build or scope session needed. */
    APPEND(",\"flush_last_us\":%u,\"flush_max_us\":%u,\"flush_count\":%u",
           (unsigned)ds->flush_last_us, (unsigned)ds->flush_max_us, (unsigned)ds->flush_count);

    if (ds->thermo_ready) {
        APPEND(",\"channels\":[");
        for (size_t i = 0; i < ds->channel_count; i++) {
            const dashboard_channel_status_t *r = &ds->channels[i];
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
            char temp_buf[16];
            char cj_buf[16];
            APPEND(
                "%s{\"channel\":%u,\"temp_c\":%s,\"cj_c\":%s,\"valid\":%s,\"fault_status\":%u,"
                "\"spi_failed\":%s,\"stale\":%s,\"age_ms\":%s}",
                i == 0 ? "" : ",", r->channel,
                json_f(temp_buf, sizeof(temp_buf), "%.2f", r->temp_c),
                json_f(cj_buf, sizeof(cj_buf), "%.2f", r->cj_c),
                r->valid ? "true" : "false",
                r->fault_status, r->spi_failed ? "true" : "false", r->stale ? "true" : "false",
                age_buf);
        }
        APPEND("]");
    }

    APPEND(",\"safety_ready\":%s", ds->safety_ready ? "true" : "false");
    APPEND(",\"zones_config_valid\":%s", ds->zones_config_valid ? "true" : "false");

    /* ROADMAP.md M6 -- null (not 0), same convention board_temps.c's
     * GET /api/board_temps already established (TODO.md 10.7): a JSON null
     * cannot be mistaken for a real 0 C reading or a real 0 W power figure
     * the way a bare 0 could. */
    APPEND(",\"safety_temp_c\":%s", ds->safety_temp_valid ? "" : "null");
    if (ds->safety_temp_valid) {
        APPEND("%.2f", (double)ds->safety_temp_c);
    }
    /* ROADMAP.md "Safety TC display audit, 2026-09-05" -- see
     * dashboard_http.h's field comment; every consumer of safety_temp_c
     * must gate its "this is a distinct thermocouple fault" display on
     * this, not on safety_ready/safety_temp_valid alone. */
    APPEND(",\"safety_tc_is_separate_sensor\":%s", ds->safety_tc_is_separate_sensor ? "true" : "false");
    APPEND(",\"enclosure_temp_c\":%s", ds->enclosure_temp_valid ? "" : "null");
    if (ds->enclosure_temp_valid) {
        APPEND("%.2f", (double)ds->enclosure_temp_c);
    }
    APPEND(",\"power_w\":%s", ds->power_valid ? "" : "null");
    if (ds->power_valid) {
        APPEND("%.1f", (double)ds->power_w);
    }

    /* dashboard_http.h's ct_current_a comment -- each of the safety
     * processor's 3 raw current-sense channels, null (not 0) per-channel on
     * the same "never a plausible-looking fake reading" convention as
     * safety_temp_c/power_w above; array index i IS CT channel i -- this
     * comment used to say "channel i+1", which disagreed with every
     * operator-facing CT label (2026-08-30: the whole UI is 0-based for
     * zones, relays, thermocouples and CTs). The array itself never
     * changed; only the comment was wrong. */
    APPEND(",\"ct_current_a\":[");
    for (unsigned ci = 0; ci < 3; ci++) {
        bool ct_valid = !isnan(ds->ct_current_a[ci]);
        APPEND("%s%s", ci == 0 ? "" : ",", ct_valid ? "" : "null");
        if (ct_valid) {
            APPEND("%.3f", (double)ds->ct_current_a[ci]);
        }
    }
    APPEND("]");

    /* dashboard_http.h's ct_counts comment -- raw ADC counts per channel,
     * independent of calibration (LINK_PROTOCOL.md Frame E, 2026-09-06).
     * Whole array is null (not per-element) when ct_counts_valid is false --
     * unlike ct_current_a[] above, a legacy (pre-protocol-11) Pico never
     * populates any of the three, so a per-element null would misleadingly
     * suggest some channels might still be real. */
    APPEND(",\"ct_counts\":%s", ds->ct_counts_valid ? "[" : "null");
    if (ds->ct_counts_valid) {
        for (unsigned ci = 0; ci < 3; ci++) {
            APPEND("%s%u", ci == 0 ? "" : ",", (unsigned)ds->ct_counts[ci]);
        }
        APPEND("]");
    }

    /* CT_COMMISSIONING_PLAN.md step 4 -- real-amps display. `ct_fitted[ci]`
     * is false for channels 0/1 in summed-CT mode (GPIO28, channel index 2,
     * is the only wired sense channel in that topology -- CURRENT_SENSE.md);
     * always true in per_zone mode, where all three channels are physically
     * distinct probes. The frontend/LCD must render "not fitted" for a
     * false entry rather than a fabricated 0.00 A, same non-plausible-fake-
     * reading convention as the null-until-known fields above. */
    {
        bool summed = dashboard_ct_topology_is_summed();
        APPEND(",\"ct_topology\":\"%s\"", summed ? "summed" : "per_zone");
        APPEND(",\"ct_fitted\":[");
        for (unsigned ci = 0; ci < 3; ci++) {
            bool fitted = !summed || ci == 2u;
            APPEND("%s%s", ci == 0 ? "" : ",", fitted ? "true" : "false");
        }
        APPEND("]");
        /* Per-zone attribution of the shared channel 2 reading -- only
         * meaningful in summed mode, and only when exactly one zone is
         * commanded on right now (CT_COMMISSIONING_PLAN.md step 4). null
         * covers both "not summed" and "not exactly one zone on" -- the
         * frontend renders "-" for both, so one null convention is enough. */
        int attrib_zone = summed ? dashboard_ct_summed_attrib_zone(ds) : -1;
        APPEND(",\"ct_summed_attrib_zone\":%s", attrib_zone >= 0 ? "" : "null");
        if (attrib_zone >= 0) {
            APPEND("%d", attrib_zone);
        }
    }

    /* K4, the safety processor's own relay -- dashboard_http.h's field
     * comment. null (not a fabricated "false") until ds->safety_relay_known
     * -- same null-until-known convention every other safety_link-sourced
     * field on this endpoint already uses. */
    APPEND(",\"safety_relay_energized\":%s", ds->safety_relay_known ? "" : "null");
    if (ds->safety_relay_known) {
        APPEND("%s", ds->safety_relay_energized ? "true" : "false");
    }
    APPEND(",\"safety_heating_enabled\":%s", ds->safety_relay_known ? "" : "null");
    if (ds->safety_relay_known) {
        APPEND("%s", ds->safety_heating_enabled ? "true" : "false");
    }
    /* The ESP's own reason for refusing heat -- see dashboard_http.h. Always
     * present and always an integer: 0 is a real answer ("nothing is blocking
     * heat"), not an absence, so this one does NOT take the null convention
     * its neighbours use. */
    APPEND(",\"heat_block_sources\":%lu", (unsigned long)ds->heat_block_sources);
    /* Decoded words for the mask above -- safety_trip_words.h's shared table
     * (2026-08-27, owner: "all faults... what was detected wrong"), same
     * function ui_page_diagnostics.c's LCD trip page now calls, so the two
     * surfaces cannot drift. LIVE state, not what tripped it -- see
     * safety_link.h's trip_fault_sources comment for why that is a separate
     * field below. */
    {
        /* 160, not 128 -- 2026-08-28 audit fix (N6), same truncation risk as
         * tf_words below: six comma-joined source strings are 141 bytes. */
        char hb_words[160];
        APPEND(",\"heat_block_sources_words\":\"%s\"",
               safety_fault_source_words(ds->heat_block_sources, hb_words, sizeof(hb_words)));
    }
    APPEND(",\"zone_blocked_mask\":%u", (unsigned)ds->zone_blocked_mask);

    /* TODO.md 9.0's deferred "GUI names both versions and which one is
     * older" item. self_protocol_version is always known; peer fields are
     * null until the Pico has announced itself (same convention as
     * safety_temp_c/power_w above). */
    APPEND(",\"self_protocol_version\":%u", (unsigned)ds->self_protocol_version);
    APPEND(",\"link_version_known\":%s", ds->link_version_known ? "true" : "false");
    if (ds->link_version_known) {
        APPEND(",\"link_version_compatible\":%s", ds->link_version_compatible ? "true" : "false");
        APPEND(",\"peer_protocol_version\":%u", (unsigned)ds->peer_protocol_version);
    } else {
        APPEND(",\"link_version_compatible\":null");
        APPEND(",\"peer_protocol_version\":null");
    }

    /* ROADMAP.md M5 -- SAFETY_CMD_DIAG (Frame B) and SAFETY_CMD_TRIP_EVENT
     * (Frame D), same null-until-received convention as everything else on
     * this endpoint. diag_trip_mask/diag_warn_mask are bitmasks (one bit per
     * guard, SaftyFW's safety_guards.h) -- left as raw integers, since a
     * caller needing per-guard names for those iterates the bits itself;
     * diag_trip_reason/trip_reason are the single "the" reason and now DO
     * get decoded cause/remedy text below, via safety_trip_words.h's shared
     * table (2026-08-27 scope change). */
    APPEND(",\"diag_ever_received\":%s", ds->diag_ever_received ? "true" : "false");
    if (ds->diag_ever_received) {
        APPEND(",\"diag_trip_reason\":%u", (unsigned)ds->diag_trip_reason);
        APPEND(",\"diag_trip_reason_words\":\"%s\"", safety_trip_words_short(ds->diag_trip_reason));
        APPEND(",\"diag_trip_reason_cause\":\"%s\"", safety_trip_words_cause(ds->diag_trip_reason));
        APPEND(",\"diag_trip_reason_remedy\":\"%s\"", safety_trip_words_remedy(ds->diag_trip_reason));
        APPEND(",\"diag_warn_mask\":%u", (unsigned)ds->diag_warn_mask);
        APPEND(",\"diag_trip_mask\":%u", (unsigned)ds->diag_trip_mask);
        APPEND(",\"diag_state\":%u", (unsigned)ds->diag_state);
        /* 2026-09-09: raw boot_reason byte plus three decoded booleans for
         * the fatal-fault bits specifically (SAFETY_LINK_DIAG_BOOT_STACK_
         * OVERFLOW/_MALLOC_FAILED/_ASSERT_FAILED, safety_link.h) -- this is
         * "last fatal fault: X" for a caller that doesn't want to decode
         * the raw byte itself. Full localisation (which task overflowed,
         * which line asserted) is NOT on this wire -- see kilnlink_diag.h's
         * comment on these bits -- only on SaftyFW's own boot-time console
         * UART banner (main.c) or via SWD. */
        APPEND(",\"diag_boot_reason\":%u", (unsigned)ds->diag_boot_reason);
        APPEND(",\"diag_boot_stack_overflow\":%s",
               (ds->diag_boot_reason & SAFETY_LINK_DIAG_BOOT_STACK_OVERFLOW) ? "true" : "false");
        APPEND(",\"diag_boot_malloc_failed\":%s",
               (ds->diag_boot_reason & SAFETY_LINK_DIAG_BOOT_MALLOC_FAILED) ? "true" : "false");
        APPEND(",\"diag_boot_assert_failed\":%s",
               (ds->diag_boot_reason & SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED) ? "true" : "false");
        APPEND(",\"diag_age_ms\":%u", (unsigned)ds->diag_age_ms);
        APPEND(",\"diag_context_age_100ms\":%u", (unsigned)ds->diag_context_age_100ms);
        APPEND(",\"diag_context_frames_ok\":%lu", (unsigned long)ds->diag_context_frames_ok);
        APPEND(",\"diag_context_frames_bad\":%lu", (unsigned long)ds->diag_context_frames_bad);
        APPEND(",\"diag_tx_frames_dropped\":%lu", (unsigned long)ds->diag_tx_frames_dropped);
    }

    APPEND(",\"trip_event_ever_received\":%s", ds->trip_event_ever_received ? "true" : "false");
    if (ds->trip_event_ever_received) {
        APPEND(",\"trip_reason\":%u", (unsigned)ds->trip_reason);
        APPEND(",\"trip_reason_words\":\"%s\"", safety_trip_words_short(ds->trip_reason));
        {
            /* 2026-08-28 scope change: the cause line now carries the actual
             * detected numbers where this firmware has them -- see
             * safety_trip_words_cause_numbered()'s header comment for
             * exactly which guards do/don't. 320, not 200: the composed S3/S9
             * sentence prose is ~130 bytes plus four %.2f floats, and a
             * pathological float magnitude (%.2f of 1e38 is ~45 chars) can
             * push a single conversion well past the 6-8 bytes a "normal"
             * amps reading takes -- -Werror=format-truncation cannot catch
             * this because the values are runtime floats, not literals, so
             * the buffer is sized for the worst case snprintf can actually
             * produce, not the common case. */
            char cause_buf[320];
            APPEND(",\"trip_reason_cause\":\"%s\"",
                   safety_trip_words_cause_numbered(ds->trip_reason, ds->trip_safety_tc_c,
                                                     ds->trip_deciding_threshold,
                                                     ds->trip_current_a, ds->trip_context_age_100ms,
                                                     cause_buf, sizeof(cause_buf)));
        }
        APPEND(",\"trip_reason_remedy\":\"%s\"", safety_trip_words_remedy(ds->trip_reason));
        APPEND(",\"trip_event_age_ms\":%lu", (unsigned long)ds->trip_event_age_ms);
        char trip_tc_buf[16];
        char trip_thr_buf[16];
        APPEND(",\"trip_safety_tc_c\":%s",
               json_f(trip_tc_buf, sizeof(trip_tc_buf), "%.1f", ds->trip_safety_tc_c));
        APPEND(",\"trip_deciding_threshold\":%s",
               json_f(trip_thr_buf, sizeof(trip_thr_buf), "%.1f", ds->trip_deciding_threshold));
        /* S6a only (safety_link.h's trip_fault_sources field comment): THIS
         * board's own fault_sources bitmask, snapshotted the instant this
         * trip latched -- distinct from heat_block_sources above, which is
         * live and may have changed since. Reported for every trip_reason
         * (harmlessly 0/"none" when the trip wasn't S6a) rather than gated,
         * so the JSON shape doesn't change per reason.
         *
         * 2026-08-28 audit fix (N3): trip_fault_sources_valid gates whether
         * the mask/words below are trustworthy -- see dashboard_http.h's
         * field comment. A reboot-time resend of an old, already-latched
         * trip leaves this false; the mask is still emitted (so the JSON
         * shape never changes) but words says so explicitly rather than
         * rendering a plausible-looking wrong cause. */
        APPEND(",\"trip_fault_sources\":%lu", (unsigned long)ds->trip_fault_sources);
        APPEND(",\"trip_fault_sources_valid\":%s", ds->trip_fault_sources_valid ? "true" : "false");
        {
            /* 2026-08-28 audit fix (N6): 128 truncates a multi-source mask
             * mid-word -- all six safety_fault_source_words() strings
             * comma-joined are 141 bytes, and three sources alone is already
             * ~70. Truncation here is RUNTIME (the helper is bounds-checked,
             * never a format string), so -Werror=format-truncation can never
             * catch this class -- sizing generously is the only guard. */
            char tf_words[160];
            /* 2026-08-28 audit fix (N4): mask==0 here is NEVER a genuine
             * "none" the way it is for heat_block_sources above -- S6a is
             * defined as the ESP having asserted the isolated fault line, so
             * a captured zero mask means the source cleared before the frame
             * arrived, not that nothing was wrong. Combined with N3's
             * validity gate: either reason renders the same "not captured"
             * message, since an operator cannot act on either differently. */
            if (ds->trip_fault_sources_valid && ds->trip_fault_sources != 0u) {
                APPEND(",\"trip_fault_sources_words\":\"%s\"",
                       safety_fault_source_words(ds->trip_fault_sources, tf_words, sizeof(tf_words)));
            } else {
                APPEND(",\"trip_fault_sources_words\":\"not captured -- the source cleared before "
                       "the trip was reported\"");
            }
        }
    }

    /* TODO.md owner-report item 5: the safety processor's own build identity
     * + config CRC (CommonFW/docs/LINK_PROTOCOL.md sec 7: "Show the safety
     * processor's own build identity, not just the ESP's"), null until a
     * FW_VERSION frame has parsed far enough to report it -- see
     * dashboard_http.h's safety_build_known comment. Escaped for the same
     * "operator/build-time string, still worth escaping" reason
     * fw_version/fw_build are above. */
    APPEND(",\"safety_build_known\":%s", ds->safety_build_known ? "true" : "false");
    if (ds->safety_build_known) {
        char commit_esc[sizeof(ds->safety_build_commit) * 2 + 1];
        char datetime_esc[sizeof(ds->safety_build_datetime) * 2 + 1];
        json_escape(ds->safety_build_commit, commit_esc, sizeof(commit_esc));
        json_escape(ds->safety_build_datetime, datetime_esc, sizeof(datetime_esc));
        APPEND(",\"safety_build_dirty\":%s", ds->safety_build_dirty ? "true" : "false");
        APPEND(",\"safety_build_commit\":\"%s\"", commit_esc);
        APPEND(",\"safety_build_datetime\":\"%s\"", datetime_esc);
        APPEND(",\"safety_config_version\":%u", (unsigned)ds->safety_config_version);
        APPEND(",\"safety_config_crc\":%u", (unsigned)ds->safety_config_crc);
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
        char fw_version_esc[sizeof(ds->fw_version) * 2 + 1];
        char fw_build_esc[sizeof(ds->fw_build) * 2 + 1];
        json_escape(ds->fw_version, fw_version_esc, sizeof(fw_version_esc));
        json_escape(ds->fw_build, fw_build_esc, sizeof(fw_build_esc));
        APPEND(",\"fw_version_known\":%s", ds->fw_version_known ? "true" : "false");
        APPEND(",\"fw_version\":\"%s\"", fw_version_esc);
        APPEND(",\"fw_build\":\"%s\"", fw_build_esc);
    }
    APPEND(",\"uptime_s\":%lu", (unsigned long)ds->uptime_s);
    APPEND(",\"reset_reason\":\"%s\"", ds->reset_reason);
    APPEND(",\"heap_internal\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
           (unsigned long)ds->heap_internal_free, (unsigned long)ds->heap_internal_largest_free_block,
           (unsigned long)ds->heap_internal_min_free, (unsigned long)ds->heap_internal_total);
    APPEND(",\"heap_spiram\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
           (unsigned long)ds->heap_spiram_free, (unsigned long)ds->heap_spiram_largest_free_block,
           (unsigned long)ds->heap_spiram_min_free, (unsigned long)ds->heap_spiram_total);
    APPEND(",\"heap_dma\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
           (unsigned long)ds->heap_dma_free, (unsigned long)ds->heap_dma_largest_free_block,
           (unsigned long)ds->heap_dma_min_free, (unsigned long)ds->heap_dma_total);

    /* Owner request 2026-08-27 -- see dashboard_http.h's field comment for
     * what "size" vs "partition_size" vs "used" each mean. null when the
     * underlying read failed, same convention as safety_temp_c etc. above. */
    APPEND(",\"flash_size\":%s", ds->flash_size_known ? "" : "null");
    if (ds->flash_size_known) {
        APPEND("%lu", (unsigned long)ds->flash_size);
    }
    APPEND(",\"flash_partition_size\":%lu", (unsigned long)ds->flash_partition_size);
    APPEND(",\"flash_used\":%s", ds->flash_used_known ? "" : "null");
    if (ds->flash_used_known) {
        APPEND("%lu", (unsigned long)ds->flash_used);
    }

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
    APPEND(",\"temp_unit\":\"%s\"", unit_pref_suffix(ds->temp_unit));

    /* 2026-09-02, forthcoming "ramp assist" feature: ADDITIVE field, same
     * "older client just never heard of this key" reasoning as temp_unit
     * above. The flag ONLY -- see ramp_assist_cfg.h's header comment; this
     * value does not yet change anything about how a ramp or dwell runs. */
    APPEND(",\"ramp_assist_enabled\":%s", ds->ramp_assist_enabled ? "true" : "false");

    /* 2026-08-30, PROFILES.md "Scheduled start + candling": ADDITIVE fields,
     * same "older client just never heard of these keys" reasoning as
     * temp_unit above. time_tz is already guaranteed printable-ASCII by
     * time_sync_tz_is_valid()/time_sync_tz_effective() (settings_http.c's
     * setter and time_sync.c's NVS load both refuse anything else), so no
     * json_escape() call is needed here -- unlike the untrusted-peer string
     * fields elsewhere in this handler. DISPLAY/SCHEDULING-INTENT ONLY: see
     * time_sync.h's header comment -- nothing in this firmware may use
     * these two epoch fields to measure a duration or drive control logic. */
    APPEND(",\"time_synced\":%s", ds->time_synced ? "true" : "false");
    APPEND(",\"time_now_epoch\":%lld", (long long)ds->time_now_epoch);
    APPEND(",\"time_last_sync_epoch\":%lld", (long long)ds->time_last_sync_epoch);
    APPEND(",\"time_tz\":\"%s\"", ds->time_tz);

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
    /* Permanently visible for the same reason boot_button_bypass_active is:
     * "this board has no OTA auth right now" is a state an operator must be
     * able to see without going looking. True when the AP password is empty,
     * which would let anyone in range compute a valid MAC from public
     * information -- ota_http.c now refuses in that state rather than
     * HMACing with a zero-length key. */
    APPEND(",\"ota_auth_disabled\":%s", ota_http_auth_disabled() ? "true" : "false");
    APPEND(",\"boot_button_bypass_remaining_s\":%lu",
           (unsigned long)(boot_button_bypass_remaining_ms() / 1000u));

    /* lvgl_port.h -- same always-visible-in-status reasoning as
     * watchdog_panic_disabled and ota_auth_disabled above: a board still
     * running the known-inaccurate touch bootstrap guess (no per-board
     * calibration ever completed) must say so everywhere this status is
     * read, not only in a boot log an operator has probably already
     * scrolled past. false here is the actionable case -- small controls
     * (topbar back/home icons) may not register touches until a
     * calibration run completes. */
    APPEND(",\"touch_calibrated\":%s", lvgl_port_touch_is_calibrated() ? "true" : "false");

    APPEND("}");

#undef APPEND
    free(ds);

    httpd_resp_set_type(req, "application/json");
    {
        /* free() AFTER send completes -- httpd_resp_send() is synchronous
         * (copies/streams `json` before returning), so this is not a
         * use-after-free; freeing before the call would be. */
        esp_err_t send_err = httpd_resp_send(req, json, o);
        free(json);
        return send_err;
    }

    /* Reached only if `json` is too small for the status it holds -- see
     * TODO.md's "/api/status has ~46 bytes of margin" item and
     * zones_http.c's zones_get_handler() truncated: label, whose pattern
     * this mirrors. safety_build_commit/safety_build_datetime arrive over
     * the isolated UART from the RP2040 (safety_link.c's FW_VERSION parse),
     * so a corrupt or hostile peer can inflate the escaped length of those
     * two fields well past what a "well-formed build string" sizing
     * assumption would allow -- see json_escape() below, which doubles
     * every byte that needs a backslash. The old behaviour here was to
     * `goto send` and emit whatever had been written so far: a truncated,
     * syntactically invalid document that main_page.html's poll() throws on
     * and silently swallows, leaving the dashboard on "Loading..." forever
     * with no visible cause. A 500 with a valid JSON body at least says
     * what happened instead of hanging silently. */
truncated:
    ESP_LOGE(DASH_TAG, "GET /api/status did not fit in %u bytes -- raise the buffer",
             (unsigned)DASHBOARD_STATUS_JSON_BUF_SIZE);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    /* free() AFTER send, same convention as this function's own success path
     * just above and every other heap-buffer handler in this pass
     * (dashboard_http.c/zones_http.c/profiles_http.c) -- httpd_resp_sendstr()
     * is synchronous, so the ordering is not a correctness question either
     * way, but one convention beats two (coordinator review, 2026-08-31: this
     * was the one place in the whole pass still freeing before its send). */
    {
        esp_err_t send_err = httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"status did not fit in the response "
                                  "buffer -- this is a firmware sizing bug, not a bad configuration\"}");
        free(ds);
        free(json);
        return send_err;
    }
}

