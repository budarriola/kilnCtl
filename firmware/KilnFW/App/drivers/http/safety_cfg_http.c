#include "safety_cfg_http.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "estop_verification.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // vTaskDelay/pdMS_TO_TICKS -- ct_auto_zero_post_handler()'s poll loop

#include "http_form.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "safety_cfg_store.h"
#include "s8_rate_guard_estimate.h" // S8 rate-guard auto-calc write path (docs/audits/s8_auto_calc_design_2026-09-09.md)
#include "zones_config_accessors.h" // zones_config_get_model()/_get_model_fit_context() -- s8 auto-calc's input
#include "zones_config_query.h"     // zones_config_get_thermo_count()
#include "uart_task_ids.h" // SAFETY_FLAG_TC_NOT_INSTALLED/SAFETY_FLAG_TC_INJECTED
#include "web_encoding.h"
#include "wifi_provision_http.h"

// CT_COMMISSIONING_PLAN.md step 2 -- ct_auto_zero_post_handler()'s
// preconditions (no profile/autotune running).
#include "autotune_engine.h"
#include "profile_executor.h"

static const char *TAG = "safety_cfg_http";

static SafetyLinkClass *s_link = NULL;
static kiln_io_t *s_hw_io = NULL; // CT_COMMISSIONING_PLAN.md step 2 -- ct_auto_zero_post_handler() only

/* Small bodies -- up to SAFETY_CFG_PARAM_COUNT id/value pairs plus commit=1,
 * "id=<n>&value=<v>" repeated per field (see parse_set_param_body()'s own
 * comment for why this shape, not a single-key form, is what this handler
 * expects). Generous headroom: worst case every one of the 58 known fields
 * submitted as an f32 (~12 chars) plus its id (~6 chars) plus separators is
 * under 1200 bytes; this is double that. Checked against Content-Length
 * before a single byte is read, same discipline as every other handler in
 * this codebase. */
#define SAFETY_CFG_BODY_MAX 2560

static bool read_body(httpd_req_t *req, char *buf, size_t cap)
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

static const char *type_name(uint8_t type)
{
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL: return "bool";
    case KILNLINK_PARAM_TYPE_U8:   return "u8";
    case KILNLINK_PARAM_TYPE_U16:  return "u16";
    case KILNLINK_PARAM_TYPE_F32:  return "f32";
    default:                       return "?";
    }
}

/* ---------------------------------------------------------------------- */
/* GET /api/safety/commissioning                                          */
/* ---------------------------------------------------------------------- */

/* Everything the JSON builder needs, gathered up front so the builder itself
 * (build_commissioning_json() below) is pure -- no httpd_req_t, no
 * SafetyLinkClass, no NVS -- and can be exercised directly from a host test
 * with hand-built inputs, same "extract the pure part" discipline
 * profile_executor.h documents for its watchdog decision function.
 * fetched_ms_ago_or_neg1 == -1 means "never fetched" (safety_cfg_store_
 * fetched_ms_ago()'s UINT32_MAX sentinel, narrowed to a signed field so
 * "never" and "0 ms ago" can never be confused). */
typedef struct {
    bool link_up;
    bool live_crc_known;
    uint16_t live_crc;      /* meaningless if !live_crc_known */
    uint16_t cached_crc;
    bool commissioned;
    int64_t fetched_ms_ago_or_neg1;
    bool unset_reliable; /* see peer_reports_unset_reliably()'s comment (M1) */

    /* Live status-frame flags (2026-09-03) -- TASK 2's "writer without a
     * reader" fix for SAFETY_FLAG_TC_NOT_INSTALLED/_TC_INJECTED (both were
     * SET by SaftyFW's link_frame_pack_status() with no name/consumer on
     * this side), plus TASK 1's new BORROWED status. These ride this
     * endpoint (not /api/status, dashboard_http.c) for the same reason
     * tc_placement_mode already does -- see safety_page.html's own comment
     * on pollPlacement(). tc_not_installed/tc_injected are only meaningful
     * when link_up is true (a dead link's last-cached flags byte is stale);
     * borrowed_known follows safety_link_status_t's own "V1/V2 frame -> not
     * yet known, never a false not-borrowed" contract regardless of
     * link_up. */
    bool tc_not_installed;
    bool tc_injected;
    bool borrowed_known;
    bool borrowed;
    uint8_t borrowed_zone_index; /* SAFETY_LINK_BORROWED_ZONE_UNKNOWN if not commissioned on the Pico */

    /* RELAY_LIFE_BUDGET.md -- ESP-only, never fetched from the
     * Pico (see safety_cfg_store_get_safety_relay_type()'s doc comment), so
     * unlike every other field above this is always known/valid, never
     * gated on link_up. */
    relay_type_t relay_type;

    /* CT_COMMISSIONING_PLAN.md step 1 -- ESP-local, never fetched from the
     * Pico, same "always known" reasoning as relay_type above. has_value
     * false means "never commissioned"; a_fs/zero_mv/source are then
     * meaningless, same "set:false omits value" contract the Pico-fetched
     * params[] entries already use. */
    bool ct_cal_has_value[SAFETY_CT_CAL_CHANNELS];
    float ct_cal_a_fs[SAFETY_CT_CAL_CHANNELS];
    float ct_cal_zero_mv[SAFETY_CT_CAL_CHANNELS];
    safety_ct_cal_source_t ct_cal_source[SAFETY_CT_CAL_CHANNELS];

    /* S8 rate-guard write provenance (docs/audits/s8_auto_calc_design_2026-
     * 09-09.md "Part 3") -- ESP-local, same "always known" reasoning as
     * relay_type/ct_cal above. rate_guard_has_provenance false means "never
     * recorded" (a board that predates this feature, or was just bench-
     * preset) -- the page must render the raw 0x0204 value from params[]
     * with no source label rather than guess. */
    bool rate_guard_has_provenance;
    safety_rate_guard_source_t rate_guard_source;
    float rate_guard_value; /* meaningless unless rate_guard_has_provenance */
} safety_cfg_http_snapshot_t;

static const char *ct_cal_source_name(safety_ct_cal_source_t s)
{
    switch (s) {
    case SAFETY_CT_CAL_SOURCE_MANUAL:    return "manual";
    case SAFETY_CT_CAL_SOURCE_SWEEP:     return "sweep";
    case SAFETY_CT_CAL_SOURCE_AUTO_ZERO: return "auto-zero";
    default:                             return "unknown";
    }
}

/* "contactor"/"mercury" only -- ssr is never a legal safety relay type, so
 * this never needs to represent it. Used both to render GET's JSON and to
 * validate POST's submitted value. */
static const char *relay_type_name(relay_type_t type)
{
    return (type == RELAY_TYPE_MERCURY) ? "mercury" : "contactor";
}

/* KILNLINK_CONFIG_PAGE_UNSET_BIT (the per-entry "this field is genuinely
 * unset" flag GET_CONFIG_PAGE replies carry) was added to the wire at
 * KILNLINK_PROTOCOL_VERSION 8 (kilnlink_version.h). A peer OLDER than that
 * never sets the bit at all -- kilnlink_config_page.c:111 decodes
 * entry_set=true for EVERY field on such a peer regardless of whether it
 * actually holds a value, and safety_cfg_store.c:refetch() faithfully
 * records that. Left ungated, an uncommissioned v7 Pico would render
 * abs_max_temp_c as {"set":true,"value":0} -- the exact "0 means the
 * overtemperature guard never trips" defect this whole commissioning-write
 * audit exists to close. */
#define SAFETY_CFG_MIN_PROTOCOL_FOR_RELIABLE_UNSET 8u

/* 2026-08-27 audit fix (M1). True only when this board actually KNOWS the
 * peer's protocol version (a FW_VERSION frame has been received at least
 * once -- safety_link_get_peer_version_status()'s own "known" gate) AND that
 * version is new enough to have ever set KILNLINK_CONFIG_PAGE_UNSET_BIT.
 * "Peer version unknown" is deliberately treated the SAME as "known and too
 * old", not as "assume reliable" -- this board cannot prove the bit means
 * anything either way until it has actually heard from the peer, and the
 * failure direction that matters here is never mistaking an unset field for
 * a real 0. */
static bool peer_reports_unset_reliably(bool peer_version_known, uint16_t peer_protocol_version)
{
    return peer_version_known && peer_protocol_version >= SAFETY_CFG_MIN_PROTOCOL_FOR_RELIABLE_UNSET;
}

/* COMMISSIONING.md sec 3.1: "stale" means cached_config_crc != live_config_crc
 * -- and, per this file's own header note (never let a caller mistake the
 * cache for live truth), a live CRC this ESP has never actually learned
 * counts as a mismatch too: there is nothing to compare the cache against
 * yet, so it cannot be vouched for as current. */
static bool snapshot_is_stale(const safety_cfg_http_snapshot_t *s)
{
    if (!s->live_crc_known) {
        return true;
    }
    return s->cached_crc != s->live_crc;
}

/* Builds the exact JSON body COMMISSIONING.md sec 3.1 specifies. Returns the
 * number of bytes written (< out_cap), or 0 if the buffer was too small to
 * hold even the fixed header -- the same "goto send with whatever fit"
 * discipline every other GET handler's APPEND() macro in this codebase uses
 * would be wrong here (a truncated JSON body is not valid JSON at all,
 * unlike a truncated list that just drops trailing rows), so this function
 * instead refuses outright rather than emit a body-shaped string that isn't
 * actually parseable JSON. out_cap is sized generously by the caller
 * (SAFETY_CFG_JSON_MAX) precisely so this never happens in practice. */
static size_t build_commissioning_json(const safety_cfg_http_snapshot_t *s, char *out, size_t out_cap)
{
    size_t o = 0;
    int written;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        written = snprintf(out + o, out_cap - o, __VA_ARGS__);                                   \
        if (written < 0 || (size_t)written >= out_cap - o) {                                     \
            return 0;                                                                             \
        }                                                                                          \
        o += (size_t)written;                                                                     \
    } while (0)

    APPEND("{\"link_up\":%s", s->link_up ? "true" : "false");
    APPEND(",\"live_config_crc\":%u", (unsigned)(s->live_crc_known ? s->live_crc : 0u));
    APPEND(",\"cached_config_crc\":%u", (unsigned)s->cached_crc);
    APPEND(",\"stale\":%s", snapshot_is_stale(s) ? "true" : "false");
    APPEND(",\"commissioned\":%s", s->commissioned ? "true" : "false");
    if (s->fetched_ms_ago_or_neg1 < 0) {
        APPEND(",\"fetched_ms_ago\":null");
    } else {
        APPEND(",\"fetched_ms_ago\":%lld", (long long)s->fetched_ms_ago_or_neg1);
    }
    /* 2026-08-27 audit fix (M1). false means: the peer is on a protocol
     * version (or is of unknown version) that never sets KILNLINK_CONFIG_
     * PAGE_UNSET_BIT, so every entry below is forced to "set":false
     * regardless of what the cache actually holds -- see peer_reports_
     * unset_reliably()'s comment. The page uses this to show an explicit
     * "cannot tell what's really set" banner rather than silently rendering
     * stale-but-plausible values as confirmed commissioning. */
    APPEND(",\"unset_reporting_reliable\":%s", s->unset_reliable ? "true" : "false");
    /* 2026-09-03: live status-frame flags, gated on link_up the same way the
     * page's other live readings already are -- a stale flags byte from a
     * dead link must not be rendered as current. */
    APPEND(",\"tc_not_installed\":%s", (s->link_up && s->tc_not_installed) ? "true" : "false");
    APPEND(",\"tc_injected\":%s", (s->link_up && s->tc_injected) ? "true" : "false");
    APPEND(",\"borrowed_known\":%s", s->borrowed_known ? "true" : "false");
    if (s->borrowed_known) {
        APPEND(",\"borrowed\":%s", s->borrowed ? "true" : "false");
        if (s->borrowed && s->borrowed_zone_index != SAFETY_LINK_BORROWED_ZONE_UNKNOWN) {
            APPEND(",\"borrowed_zone_index\":%u", (unsigned)s->borrowed_zone_index);
        }
    }
    APPEND(",\"relay_type\":\"%s\"", relay_type_name(s->relay_type));
    APPEND(",\"ct_cal\":[");
    for (size_t ch = 0; ch < SAFETY_CT_CAL_CHANNELS; ch++) {
        APPEND("%s{\"has_value\":%s", ch == 0 ? "" : ",", s->ct_cal_has_value[ch] ? "true" : "false");
        if (s->ct_cal_has_value[ch]) {
            APPEND(",\"a_fs\":%.6g,\"zero_mv\":%.6g,\"source\":\"%s\"", (double)s->ct_cal_a_fs[ch],
                   (double)s->ct_cal_zero_mv[ch], ct_cal_source_name(s->ct_cal_source[ch]));
        }
        APPEND("}");
    }
    APPEND("]");
    APPEND(",\"rate_guard_provenance\":{\"has_value\":%s",
           s->rate_guard_has_provenance ? "true" : "false");
    if (s->rate_guard_has_provenance) {
        APPEND(",\"source\":\"%s\",\"value\":%.6g",
               s->rate_guard_source == SAFETY_RATE_GUARD_SOURCE_AUTO ? "auto" : "manual",
               (double)s->rate_guard_value);
    }
    APPEND("}");
    APPEND(",\"params\":[");

    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t p;
        if (!safety_cfg_store_get_by_index(i, &p)) {
            continue; /* cannot happen for i < count, defensive only */
        }
        bool set = p.set && s->unset_reliable;
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"set\":%s", i == 0 ? "" : ",",
               (unsigned)p.param_id, p.name, type_name(p.type), set ? "true" : "false");
        /* COMMISSIONING.md sec 3.1: "An unset parameter carries `"set": false`
         * and OMITS `value` entirely rather than sending a zero the page
         * might print." -- the omission happens here, not by printing a
         * sentinel, so the page literally cannot mistake it for a real 0.
         * M1: also omitted whenever this peer's "set" bit cannot be trusted
         * at all, even if the cache's own p.set happens to be true. */
        if (set) {
            switch (p.type) {
            case KILNLINK_PARAM_TYPE_BOOL:
                APPEND(",\"value\":%s", p.value.bool_val ? "true" : "false");
                break;
            case KILNLINK_PARAM_TYPE_U8:
                APPEND(",\"value\":%u", (unsigned)p.value.u8_val);
                break;
            case KILNLINK_PARAM_TYPE_U16:
                APPEND(",\"value\":%u", (unsigned)p.value.u16_val);
                break;
            case KILNLINK_PARAM_TYPE_F32:
                if (isfinite(p.value.f32_val)) {
                    APPEND(",\"value\":%.6g", (double)p.value.f32_val);
                } else {
                    /* A non-finite float has no JSON representation --
                     * report it the same way an unset field is: omit
                     * `value` rather than emit invalid JSON or a fabricated
                     * number (NaN/Inf can only reach here from a genuinely
                     * corrupt wire reply, since every codec this cache
                     * fetches from already round-trips real IEEE-754
                     * bytes). */
                }
                break;
            default:
                break;
            }
        }
        APPEND("}");
    }
    APPEND("]}");

#undef APPEND
    return o;
}

/* Fixed upper bound for build_commissioning_json()'s output: a per-param
 * entry is at most ~80 bytes (id+name up to ~24 chars+type+set+value), times
 * SAFETY_CFG_PARAM_COUNT, plus a small fixed header -- generous headroom
 * over the ~57*80 + 128 ~= 4700 bytes a full response actually needs. */
#define SAFETY_CFG_JSON_MAX (SAFETY_CFG_PARAM_COUNT * 128u + 256u)

static esp_err_t commissioning_get_handler(httpd_req_t *req)
{
    safety_cfg_http_snapshot_t snap = {0};

    if (s_link) {
        safety_link_status_t st;
        if (safety_link_get_status(s_link, &st) == ESP_OK) {
            snap.link_up = st.link_up;
            snap.commissioned =
                st.diag_ever_received && !(st.diag_flags & SAFETY_LINK_DIAG_FLAG_CALIBRATION_MISSING);
            snap.tc_not_installed = (st.flags & SAFETY_FLAG_TC_NOT_INSTALLED) != 0u;
            snap.tc_injected = (st.flags & SAFETY_FLAG_TC_INJECTED) != 0u;
            snap.borrowed_known = st.borrowed_known;
            snap.borrowed = st.borrowed;
            snap.borrowed_zone_index = st.borrowed_zone_index;
        }
        uint16_t peer_crc = 0;
        bool peer_known = false;
        (void)safety_link_get_peer_build_status(s_link, &peer_known, NULL, NULL, NULL, NULL, NULL, NULL,
                                                  &peer_crc);
        snap.live_crc_known = peer_known;
        snap.live_crc = peer_crc;

        /* 2026-08-27 audit fix (M1). */
        bool peer_version_known = false;
        bool peer_version_compatible = false;
        uint16_t peer_protocol_version = 0;
        (void)safety_link_get_peer_version_status(s_link, &peer_version_known, &peer_version_compatible,
                                                    &peer_protocol_version, NULL);
        snap.unset_reliable = peer_reports_unset_reliably(peer_version_known, peer_protocol_version);
    }
    snap.relay_type = safety_cfg_store_get_safety_relay_type();
    for (size_t ch = 0; ch < SAFETY_CT_CAL_CHANNELS; ch++) {
        snap.ct_cal_has_value[ch] = safety_cfg_store_get_ct_cal_input(
            ch, &snap.ct_cal_a_fs[ch], &snap.ct_cal_zero_mv[ch], &snap.ct_cal_source[ch]);
    }
    (void)safety_cfg_store_get_rate_guard_meta(&snap.rate_guard_source, &snap.rate_guard_value,
                                                &snap.rate_guard_has_provenance);
    snap.cached_crc = safety_cfg_store_cached_crc();
    uint32_t fetched = safety_cfg_store_fetched_ms_ago();
    snap.fetched_ms_ago_or_neg1 = (fetched == UINT32_MAX) ? -1 : (int64_t)fetched;

    /* 2026-09-05 DRAM_PSRAM_PLAN.md: no NVS/flash call anywhere in this file
     * (safety config is pushed over the safety UART link, not stored via
     * NVS locally), so this scratch buffer is safe to move off internal
     * DRAM -- it is never a source/dest for a flash write, and this handler
     * runs on httpd_worker, whose own stack stays internal. */
    static EXT_RAM_BSS_ATTR char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    if (len == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "commissioning JSON build failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning                                         */
/* ---------------------------------------------------------------------- */

/* One operator-submitted (id, raw text value) pair, before type-checking --
 * parse_set_param_body() below fills these in the order they appeared in the
 * body; type lookup and numeric parsing happen in apply_pairs(), not here,
 * so this struct/parser stays reusable for the bench-preset path's own,
 * differently-sourced pairs (see bench_preset_post_handler()). */
typedef struct {
    uint16_t param_id;
    char value_text[24]; /* generous over the longest legal u16/f32 literal */
} safety_cfg_post_pair_t;

#define SAFETY_CFG_POST_MAX_PAIRS SAFETY_CFG_PARAM_COUNT

/* Parses "id=<n>&value=<v>" pairs, repeated, plus an optional trailing
 * "commit=1" flag, out of a form-urlencoded POST body.
 *
 * WIRE SHAPE CHOSEN HERE, not dictated by COMMISSIONING.md sec 3.1's prose
 * ("form-urlencoded, id=<n>&value=<v> pairs plus commit=1"): that text is
 * ambiguous about whether repeated pairs reuse the SAME "id"/"value" keys
 * (which is what a plain <input name="id"> per row would naturally produce)
 * or use per-row-numbered keys (id0/value0, id1/value1, ...). http_form.h's
 * http_form_find_field() only ever returns the FIRST match for a given key,
 * so a same-key-repeated body cannot be parsed with that helper at all --
 * this function is a small, separate, ORDER-preserving tokenizer instead:
 * it walks '&'-delimited tokens left to right and pairs each "id" token with
 * the "value" token immediately following it, tolerating "commit=1"
 * anywhere in the sequence. This is the interpretation this pass
 * implements; the page (owned by a parallel agent this pass, per the task's
 * file-ownership split) must emit repeated id=/value= tokens in that
 * adjacent order for a multi-field submit to parse. Returns the number of
 * pairs found (<= SAFETY_CFG_POST_MAX_PAIRS), or -1 if more pairs were
 * present than fit, an "id" token had no following "value" token, or any
 * token was malformed -- nothing is applied by THIS function either way, it
 * only parses. */
static int parse_set_param_body(const char *body, safety_cfg_post_pair_t *out, size_t out_cap,
                                 bool *out_commit)
{
    *out_commit = false;
    size_t n = 0;
    const char *p = body;
    bool pending_id = false;
    uint16_t pending_id_val = 0;

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t token_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', token_len);
        if (!eq) {
            return -1; /* malformed token, no '=' */
        }
        size_t key_len = (size_t)(eq - p);
        const char *val_start = eq + 1;
        size_t val_len = token_len - key_len - 1;

        if (key_len == 2 && strncmp(p, "id", 2) == 0) {
            if (pending_id) {
                return -1; /* two "id" tokens in a row with no "value" between them */
            }
            char idbuf[8];
            if (http_form_url_decode(val_start, val_len, idbuf, sizeof(idbuf)) < 0) {
                return -1;
            }
            char *end = NULL;
            long v = strtol(idbuf, &end, 10);
            if (end == idbuf || *end != '\0' || v < 0 || v > 0xFFFF) {
                return -1;
            }
            pending_id_val = (uint16_t)v;
            pending_id = true;
        } else if (key_len == 5 && strncmp(p, "value", 5) == 0) {
            if (!pending_id) {
                return -1; /* "value" with no preceding "id" */
            }
            if (n >= out_cap) {
                return -1; /* more pairs than this table has parameters for */
            }
            out[n].param_id = pending_id_val;
            if (http_form_url_decode(val_start, val_len, out[n].value_text,
                                      sizeof(out[n].value_text)) < 0) {
                return -1;
            }
            n++;
            pending_id = false;
        } else if (key_len == 6 && strncmp(p, "commit", 6) == 0) {
            char cbuf[4];
            if (http_form_url_decode(val_start, val_len, cbuf, sizeof(cbuf)) >= 0) {
                *out_commit = (strcmp(cbuf, "1") == 0);
            }
        }
        /* Any other key is ignored, not rejected -- a future field this
         * handler doesn't know about yet must not turn every submission into
         * a hard 400. */

        if (!amp) {
            break;
        }
        p = amp + 1;
    }

    if (pending_id) {
        return -1; /* trailing "id" with no "value" */
    }
    return (int)n;
}

/* Parses one pair's text value against `type` into `out`. Returns false
 * (out untouched) on a malformed number or a bool/enum value that fails
 * strtoul/strtof's own parse -- range-checking beyond "fits the wire type"
 * is deliberately NOT done here (e.g. tc_source's enum range): that
 * cross-field/enum-range validation is COMMIT_CONFIG's job on the Pico,
 * per COMMISSIONING.md sec 2's "validation happens at COMMIT_CONFIG, not at
 * SET_PARAM, because the rules that matter are cross-field" -- duplicating
 * a second, possibly-diverging range check here would be exactly the kind
 * of two-definitions-of-valid this codebase's other stores avoid. */
static bool parse_value_for_type(const char *text, uint8_t type, kilnlink_param_value_t *out)
{
    char *end = NULL;
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || (v != 0 && v != 1)) {
            return false;
        }
        out->bool_val = (uint8_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_U8: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || v < 0 || v > 0xFF) {
            return false;
        }
        out->u8_val = (uint8_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_U16: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || v < 0 || v > 0xFFFF) {
            return false;
        }
        out->u16_val = (uint16_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_F32: {
        float v = strtof(text, &end);
        if (end == text || *end != '\0' || !isfinite(v)) {
            return false;
        }
        out->f32_val = v;
        return true;
    }
    default:
        return false;
    }
}

/* Forward declaration -- confirm_commit_landed() below needs this before its
 * own definition later in the file (kept where it always lived, right next
 * to apply_pairs() which is its other caller). */
static const char *commit_reject_reason_words(uint8_t reason);

/* True if two kilnlink_param_value_t of the same wire `type` hold the same
 * value. F32 is compared bit-for-bit (memcmp), not with an epsilon -- both
 * ends of this link encode/decode the identical 4-byte IEEE-754 layout
 * (kilnlink_param_value.h), so a value that survived SET_PARAM -> COMMIT_CONFIG
 * -> flash -> GET_CONFIG_PAGE unchanged reads back BIT-IDENTICAL or it did not
 * survive at all; there is no legitimate case of "close enough" here. */
static bool param_value_equal(uint8_t type, const kilnlink_param_value_t *a, const kilnlink_param_value_t *b)
{
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL: return a->bool_val == b->bool_val;
    case KILNLINK_PARAM_TYPE_U8:   return a->u8_val == b->u8_val;
    case KILNLINK_PARAM_TYPE_U16:  return a->u16_val == b->u16_val;
    case KILNLINK_PARAM_TYPE_F32:  return memcmp(&a->f32_val, &b->f32_val, sizeof(a->f32_val)) == 0;
    default:                       return false;
    }
}

/* Positive confirmation that a commit this function just reported ACKed (and
 * not rejected within safety_link_send_commit_config()'s own reply window)
 * actually landed on the Pico's flash, per COMMISSIONING.md/the 2026-08-26
 * commissioning-write audit: "ok cannot fail" because SET_PARAM/COMMIT_CONFIG
 * are both fire-and-forget broadcasts (uart_protocol_send_broadcast() reports
 * only "the local UART accepted the bytes"), and a REJECTED reply that misses
 * the ~144 ms reply window used to be silently discarded, defining "no
 * rejection seen" as acceptance.
 *
 * This function is what turns that around: it forces a LIVE re-fetch of the
 * Pico's just-committed record (safety_cfg_store_refetch(), a real blocking
 * round trip -- never the ESP's own stale NVS cache) and checks that every
 * field THIS caller just submitted now reads back exactly the value that was
 * sent. That is strictly stronger than comparing config_crc before/after:
 * a commit that legitimately writes bytes identical to what was already
 * committed bumps nothing a CRC could detect, but the read-back still
 * matches what was sent, so it is correctly reported as success. Only a
 * field that reads back UNSET, or SET to something other than what was sent,
 * is reported as a failure -- i.e. the only two ways a "successful" commit
 * could still be a lie.
 *
 * Returns true (reason_out untouched) iff every submitted pair's value is
 * confirmed. On failure, reason_out names the first mismatching field and, if
 * a COMMIT_CONFIG_REJECTED frame turns up late in the stash while this
 * function was busy doing the live re-fetch (safety_link_take_stashed_
 * commit_rejected()), attaches the Pico's OWN reason instead of a generic
 * "does not match" message -- the read-back is what DECIDES pass/fail, the
 * stash only explains WHY when it can. */
static bool confirm_commit_landed(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                                   char *reason_out, size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(link, &peer_known, NULL, NULL, NULL, NULL, NULL, NULL,
                                             &best_known_crc);

    if (!safety_cfg_store_refetch(link, peer_known ? best_known_crc : 0)) {
        uint16_t rp = 0;
        uint8_t rr = 0;
        if (safety_link_take_stashed_commit_rejected(link, &rp, &rr)) {
            uint8_t rt = 0;
            const char *rn = NULL;
            if (rp != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID && safety_cfg_store_lookup(rp, &rt, &rn)) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written", rn,
                         (unsigned)rp, commit_reject_reason_words(rr));
            } else {
                snprintf(reason_out, reason_cap, "commit rejected: %s -- values were staged but NOT written",
                         commit_reject_reason_words(rr));
            }
        } else {
            snprintf(reason_out, reason_cap,
                     "the safety processor accepted the commit but this board could not read the "
                     "config back to confirm it -- treating the write as UNCONFIRMED, not successful");
        }
        return false;
    }

    for (int i = 0; i < n_pairs; i++) {
        uint8_t type = 0;
        const char *name = NULL;
        if (!safety_cfg_store_lookup(pairs[i].param_id, &type, &name)) {
            /* 2026-08-27 audit fix (LOW): this used to be `continue`, silently
             * SKIPPING verification of a pair whose lookup failed -- believed
             * unreachable (apply_pairs() already refused any unknown id
             * before staging began), but a skipped verification that then
             * lets the OVERALL commit report success is exactly the failure
             * shape this whole audit exists to close. If this branch is ever
             * actually reached, the honest answer is "could not confirm",
             * never "confirmed". */
            snprintf(reason_out, reason_cap,
                     "internal error: could not verify id %u (%s) after commit -- treating the write "
                     "as UNCONFIRMED, not successful",
                     (unsigned)pairs[i].param_id, name ? name : "unknown");
            return false;
        }
        kilnlink_param_value_t sent;
        if (!parse_value_for_type(pairs[i].value_text, type, &sent)) {
            /* Same reasoning as the lookup failure just above -- believed
             * unreachable (apply_pairs() already parsed this value
             * successfully before staging), same fail-closed answer. */
            snprintf(reason_out, reason_cap,
                     "internal error: could not re-verify the value submitted for %s (id %u) after "
                     "commit -- treating the write as UNCONFIRMED, not successful",
                     name, (unsigned)pairs[i].param_id);
            return false;
        }

        bool found = false;
        safety_cfg_param_t confirmed = {0};
        size_t count = safety_cfg_store_param_count();
        for (size_t j = 0; j < count; j++) {
            safety_cfg_param_t row;
            if (safety_cfg_store_get_by_index(j, &row) && row.param_id == pairs[i].param_id) {
                confirmed = row;
                found = true;
                break;
            }
        }

        if (!found || !confirmed.set || !param_value_equal(type, &confirmed.value, &sent)) {
            uint16_t rp = 0;
            uint8_t rr = 0;
            if (safety_link_take_stashed_commit_rejected(link, &rp, &rr) &&
                (rp == pairs[i].param_id || rp == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID)) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written", name,
                         (unsigned)pairs[i].param_id, commit_reject_reason_words(rr));
            } else {
                snprintf(reason_out, reason_cap,
                         "the safety processor ACKed the commit, but %s (id %u) does not read back "
                         "as the submitted value -- treating the write as FAILED, not successful",
                         name, (unsigned)pairs[i].param_id);
            }
            return false;
        }
    }
    return true;
}

/* kilnlink_commit_config_reject_reason_t -> a short human phrase, for
 * apply_pairs()'s rejection message below. Matches the wording
 * config_params.h's config_params_reject_reason_t doc comment and
 * kilnlink_commit_config_rejected.h's own reason enum use to describe each
 * case -- kept here, not in a shared header, for the same "ESP web surface
 * owns its own wording" split safety_trip_words.h's own comment documents
 * for the LCD/web trip-reason tables (this one just has one caller instead
 * of two). */
static const char *commit_reject_reason_words(uint8_t reason)
{
    switch (reason) {
    case KILNLINK_COMMIT_CONFIG_REJECT_RANGE: return "value out of range";
    case KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION: return "contradicts another staged field";
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED: return "relay is ARMED -- config writes are refused while ARMED";
    case KILNLINK_COMMIT_CONFIG_REJECT_STORAGE: return "the safety processor's flash write failed";
    default: return "refused (unrecognised reason)";
    }
}

/* Stages every pair via safety_link_send_set_param(), then (if `commit`)
 * sends COMMIT_CONFIG. Writes a human-readable outcome into reason_out
 * (always NUL-terminated if reason_cap > 0) and returns true only if every
 * stage succeeded AND (if requested) the commit was ACKed and ACCEPTED.
 *
 * COMMISSIONING.md sec 3.1 asks this endpoint to "name the offending field
 * and the rule it broke" on rejection -- SAFETY_CMD_COMMIT_CONFIG_REJECTED
 * (0x20) now carries exactly that (ROADMAP.md loose end, closed): a REJECTED
 * commit is no longer indistinguishable from an ACCEPTED one at this layer.
 * safety_link_send_commit_config()'s out_rejected/out_param_id/out_reason
 * report it; this function turns the param_id back into a field NAME via
 * safety_cfg_store_lookup() (the same table safety_cfg_store.c's ESP-side
 * cache uses) and the reason code into words via commit_reject_reason_words()
 * above, then reports both -- never "sent, awaiting confirmation" for a
 * rejection this build can now actually see. A param_id this build's own
 * table does not recognise (KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID, or
 * a real id from a newer Pico this ESP predates) falls back to reporting the
 * numeric id, same "refused individually... reported by numeric id" fallback
 * the unknown-id case just below already uses. */
/* param 0x0212 (estop_active_level) -- see safety_cfg_store.h's table entry
 * and discrete_pin_policy.h. A successful commit that includes this param
 * must invalidate any standing E-stop bench-verification record
 * (estop_verification.h): the operator's confirmation was made against a
 * specific polarity, and a re-send of this param -- even one that ends up
 * setting the SAME value -- is grounds to distrust a verification made
 * before this ESP can prove which polarity it was against. */
#define SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL 0x0212u

static bool apply_pairs(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                        bool commit, char *reason_out, size_t reason_cap)
{
    if (!link) {
        snprintf(reason_out, reason_cap, "safety link not available on this board");
        return false;
    }
    for (int i = 0; i < n_pairs; i++) {
        uint8_t type = 0;
        const char *name = NULL;
        if (!safety_cfg_store_lookup(pairs[i].param_id, &type, &name)) {
            /* COMMISSIONING.md sec 2: "unknown ids are refused individually
             * and named in the reply, rather than the whole transfer
             * failing" -- this IS that per-id refusal, reported by numeric
             * id since this build's table has no name for it. */
            snprintf(reason_out, reason_cap, "unknown parameter id %u", (unsigned)pairs[i].param_id);
            return false;
        }
        kilnlink_param_value_t value;
        if (!parse_value_for_type(pairs[i].value_text, type, &value)) {
            snprintf(reason_out, reason_cap, "invalid value for %s (id %u)", name,
                     (unsigned)pairs[i].param_id);
            return false;
        }
        esp_err_t err = safety_link_send_set_param(link, pairs[i].param_id, type, value);
        if (err != ESP_OK) {
            snprintf(reason_out, reason_cap, "communication with the safety processor failed while "
                                              "staging %s (id %u): %s",
                     name, (unsigned)pairs[i].param_id, esp_err_to_name(err));
            return false;
        }
    }
    if (commit) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(link, &reject_param_id, &reject_reason, &rejected);
        if (err != ESP_OK) {
            snprintf(reason_out, reason_cap, "the safety processor did not acknowledge the commit "
                                              "(%s) -- values were staged but NOT written",
                     esp_err_to_name(err));
            return false;
        }
        if (rejected) {
            uint8_t reject_type = 0;
            const char *reject_name = NULL;
            if (reject_param_id != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID &&
                safety_cfg_store_lookup(reject_param_id, &reject_type, &reject_name)) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written",
                         reject_name, (unsigned)reject_param_id, commit_reject_reason_words(reject_reason));
            } else if (reject_param_id != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: parameter id %u -- %s -- values were staged but NOT written",
                         (unsigned)reject_param_id, commit_reject_reason_words(reject_reason));
            } else {
                snprintf(reason_out, reason_cap, "commit rejected: %s -- values were staged but NOT written",
                         commit_reject_reason_words(reject_reason));
            }
            return false;
        }
        /* ESP_OK and not rejected within the reply window is NOT proof the
         * write landed (see confirm_commit_landed()'s header comment for the
         * full audit trail) -- force a live read-back before this function
         * is allowed to report success. */
        if (!confirm_commit_landed(link, pairs, n_pairs, reason_out, reason_cap)) {
            return false;
        }
        /* Landed for real -- now invalidate a standing E-stop verification if
         * estop_active_level was one of the committed params. See
         * SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL's comment above for why this
         * fires on ANY commit of the param, not just a value change. */
        for (int i = 0; i < n_pairs; i++) {
            if (pairs[i].param_id == SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL) {
                /* The clear's own result decides this POST's result. If the
                 * record could not be cleared (and estop_verification_clear()
                 * only returns ESP_OK once a read-back confirms it), a
                 * standing verified=1 survives a polarity commit and
                 * /api/readiness keeps reporting "confirmed by operator" for
                 * a polarity nobody verified. Answering {"ok":true} there
                 * would be this file's own "logging unchecked success" class
                 * -- the commit DID land, so the reason says so explicitly
                 * rather than implying the values were not written. */
                esp_err_t clear_err = estop_verification_clear();
                if (clear_err != ESP_OK) {
                    /* Kept inside reason[160] deliberately: the full
                     * "do not trust the readiness page" explanation lives in
                     * estop_verification.c's own ESP_LOGE, not here. */
                    snprintf(reason_out, reason_cap,
                             "estop_active_level committed, but the E-stop verification record "
                             "could NOT be cleared (%s) -- re-run the bench procedure",
                             esp_err_to_name(clear_err));
                    return false;
                }
                break;
            }
        }
    }
    reason_out[0] = '\0';
    return true;
}

/* Public single-field stage+commit+confirm wrapper -- owner request
 * 2026-09-10 ("if i change the max temp in the web gui it should change it
 * in the pico too."). safety_ceiling_sync.c (zones_http_post.c's helper)
 * uses this as the `safety_ceiling_writer_fn` callback for abs_max_temp_c
 * (param id 0x0104): it needs exactly this file's apply_pairs()/confirm_
 * commit_landed() machinery -- stage, commit, and a live read-back that
 * proves the value actually landed, not merely that it was ACKed within the
 * reply window -- and does not deserve a second implementation of any of
 * that. Declared in safety_cfg_http.h.
 *
 * Single pair, always committed (`commit=true` is unconditional -- there is
 * no legitimate reason to stage this field without committing it). Reports
 * failure (including the ARMED refusal, verbatim via commit_reject_reason_
 * words()) through `reason_out`/`reason_cap` exactly like every other
 * caller of apply_pairs() in this file. */
bool safety_cfg_http_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
                                          char *reason_out, size_t reason_cap)
{
    if (!reason_out || reason_cap == 0) {
        return false;
    }
    safety_cfg_post_pair_t pair;
    pair.param_id = param_id;
    snprintf(pair.value_text, sizeof(pair.value_text), "%.9g", (double)value);
    return apply_pairs(link, &pair, 1, /*commit=*/true, reason_out, reason_cap);
}

static esp_err_t commissioning_post_handler(httpd_req_t *req)
{
    /* 2026-09-05 DRAM_PSRAM_PLAN.md: same rationale as commissioning_get_
     * handler's json[] above -- no NVS/flash call in this file, so these
     * scratch buffers are safe to move off internal DRAM. */
    static EXT_RAM_BSS_ATTR char body[SAFETY_CFG_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    static EXT_RAM_BSS_ATTR safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body(body, pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed id/value pairs");
        return ESP_OK;
    }
    /* Zero recognised pairs is a refusal, not a success. A body whose tokens
     * are all unrecognised (e.g. "0x0101=1200", using the documentation's hex
     * spelling of an id rather than the decimal "id=257&value=..." this
     * endpoint parses) previously fell through apply_pairs() with n == 0 and
     * answered {"ok":true} -- reporting that a commissioning value had been
     * accepted when nothing whatsoever had been staged. A commissioning
     * endpoint claiming success for a no-op is exactly the wrong failure
     * direction for the subsystem that stops a runaway.
     *
     * `commit` alone is legitimate (a bare commit of already-staged values),
     * so only refuse when there is neither a pair nor a commit. */
    if (n == 0 && !commit) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "no recognised id/value pairs in body -- expected id=<decimal>&value=<v>");
        return ESP_OK;
    }

    char reason[160];
    bool ok = apply_pairs(s_link, pairs, n, commit, reason, sizeof(reason));

    /* S8 rate-guard write provenance -- this generic endpoint is how an
     * operator hand-enters max_rate_c_per_min (0x0204) today, so a
     * successful COMMITTED write of it here is, by definition, a MANUAL
     * write: tag it so the commissioning page never shows a stale
     * "auto-derived" label for a value the operator just typed over it.
     * Only tagged when `commit` actually happened -- apply_pairs() already
     * guarantees confirm_commit_landed() ran in that case, so this is the
     * same "verify before tagging" discipline the auto-apply endpoint uses.
     * A stage-only (commit=false) submission changes nothing on the Pico
     * yet, so it must not touch this record either. */
    if (ok && commit) {
        for (int i = 0; i < n; i++) {
            if (pairs[i].param_id == 0x0204) {
                char *end = NULL;
                float value = strtof(pairs[i].value_text, &end);
                if (end != pairs[i].value_text && isfinite(value)) {
                    esp_err_t nvs_err = ESP_OK;
                    safety_cfg_store_set_rate_guard_meta(SAFETY_RATE_GUARD_SOURCE_MANUAL, value, &nvs_err);
                }
                break;
            }
        }
    }

    char resp[256];
    int len;
    if (ok) {
        len = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    } else {
        char escaped[192];
        size_t o = 0;
        for (const char *c = reason; *c && o + 2 < sizeof(escaped); c++) {
            if (*c == '"' || *c == '\\') {
                escaped[o++] = '\\';
            }
            escaped[o++] = *c;
        }
        escaped[o] = '\0';
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", escaped);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning/relay_type                              */
/* ---------------------------------------------------------------------- */

/* RELAY_LIFE_BUDGET.md. Body: "type=contactor" or "type=mercury"
 * (http_form's usual application/x-www-form-urlencoded shape) -- deliberately
 * NOT routed through parse_set_param_body()/apply_pairs() above, since this
 * is not a Pico param at all (see safety_cfg_store_get_safety_relay_type()'s
 * doc comment): no safety_link involved, no commit round trip, just a local
 * NVS write. "ssr" (and anything else unrecognised) is REJECTED with 400 --
 * the safety relay never offers ssr, and safety_cfg_store_set_safety_relay_
 * type() enforces the same rule as its own second line of defense, but the
 * point of checking here too is to give the operator a specific 400 instead
 * of a generic "nothing happened". */
#define SAFETY_RELAY_TYPE_BODY_MAX 64

static esp_err_t relay_type_post_handler(httpd_req_t *req)
{
    char body[SAFETY_RELAY_TYPE_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized body");
        return ESP_OK;
    }

    char value[16] = {0};
    if (http_form_find_field(body, "type", value, sizeof(value)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'type' field");
        return ESP_OK;
    }

    relay_type_t type;
    if (strcmp(value, "contactor") == 0) {
        type = RELAY_TYPE_CONTACTOR;
    } else if (strcmp(value, "mercury") == 0) {
        type = RELAY_TYPE_MERCURY;
    } else {
        /* Explicitly covers "ssr" -- the safety relay never offers it -- and
         * any other unrecognised value. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "type must be 'contactor' or 'mercury' -- the safety relay "
                             "does not offer 'ssr'");
        return ESP_OK;
    }

    esp_err_t nvs_err = ESP_OK;
    if (!safety_cfg_store_set_safety_relay_type(type, &nvs_err)) {
        /* Unreachable given the check above, but never claim success for a
         * call that refused. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay type rejected");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "safety relay type set to %s", relay_type_name(type));
    httpd_resp_set_type(req, "application/json");
    if (nvs_err != ESP_OK) {
        /* 2026-09-06 audit fix: applied live, but NOT persisted -- the type
         * reverts to whatever was last saved on the next reboot. Report this
         * honestly instead of an unconditional {"ok":true}. */
        char resp[160];
        int len = snprintf(resp, sizeof(resp),
                            "{\"ok\":true,\"persisted\":false,\"err\":\"%s\"}",
                            esp_err_to_name(nvs_err));
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }
    return httpd_resp_sendstr(req, "{\"ok\":true,\"persisted\":true}");
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning/ct_cal                                  */
/* ---------------------------------------------------------------------- */

/* CT_COMMISSIONING_PLAN.md step 1: the operator types the probe's rated
 * A_fs and its zero_mv output in their own units; this handler converts
 * (safety_ct_cal_store_set_ct_cal_input()) and stages the derived
 * k_ct_v_per_a[ch]/zero_counts[ch] through the SAME generic apply_pairs()/
 * confirm_commit_landed() path every other field on this page already uses
 * -- no separate write/verify story for these two fields. Body:
 * "ch=<0-2>&a_fs=<v>&zero_mv=<v>&commit=1"; commit is optional the same way
 * the generic endpoint's is (stage-only submissions are legitimate). Always
 * writes with source=MANUAL -- this endpoint IS manual entry by an operator;
 * the sweep and auto-zero paths call safety_cfg_store_set_ct_cal_input()
 * directly with their own source, never through this HTTP surface. */
#define SAFETY_CT_CAL_BODY_MAX 128

static esp_err_t ct_cal_post_handler(httpd_req_t *req)
{
    char body[SAFETY_CT_CAL_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized body");
        return ESP_OK;
    }

    char ch_text[8] = {0}, a_fs_text[24] = {0}, zero_mv_text[24] = {0}, commit_text[4] = {0};
    if (http_form_find_field(body, "ch", ch_text, sizeof(ch_text)) < 0 ||
        http_form_find_field(body, "a_fs", a_fs_text, sizeof(a_fs_text)) < 0 ||
        http_form_find_field(body, "zero_mv", zero_mv_text, sizeof(zero_mv_text)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected ch=<0-2>&a_fs=<v>&zero_mv=<v>");
        return ESP_OK;
    }
    bool commit = (http_form_find_field(body, "commit", commit_text, sizeof(commit_text)) >= 0) &&
                  strcmp(commit_text, "1") == 0;

    char *end = NULL;
    long ch_l = strtol(ch_text, &end, 10);
    if (end == ch_text || *end != '\0' || ch_l < 0 || ch_l >= (long)SAFETY_CT_CAL_CHANNELS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch must be 0, 1, or 2");
        return ESP_OK;
    }
    size_t ch = (size_t)ch_l;

    float a_fs = strtof(a_fs_text, &end);
    if (end == a_fs_text || *end != '\0' || !isfinite(a_fs)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid a_fs value");
        return ESP_OK;
    }
    if (a_fs < SAFETY_CT_CAL_A_FS_MIN || a_fs > SAFETY_CT_CAL_A_FS_MAX) {
        char msg[96];
        snprintf(msg, sizeof(msg), "a_fs must be between %g and %g A",
                 (double)SAFETY_CT_CAL_A_FS_MIN, (double)SAFETY_CT_CAL_A_FS_MAX);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    float zero_mv = strtof(zero_mv_text, &end);
    if (end == zero_mv_text || *end != '\0' || !isfinite(zero_mv)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid zero_mv value");
        return ESP_OK;
    }
    if (zero_mv < SAFETY_CT_CAL_ZERO_MV_MIN || zero_mv > SAFETY_CT_CAL_ZERO_MV_MAX) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zero_mv must be between %g and %g mV",
                 (double)SAFETY_CT_CAL_ZERO_MV_MIN, (double)SAFETY_CT_CAL_ZERO_MV_MAX);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    /* 2026-09-06 audit fix: this used to persist the ESP-local record
     * (source=MANUAL) BEFORE apply_pairs() ran. If the Pico commit then
     * failed, the channel was left permanently MANUAL in the local cache
     * (zone_sweep_plan_k_ct() skips a manually-calibrated channel forever)
     * while the Pico still enforced the OLD k_ct/zero_counts -- a silent,
     * permanent divergence between what this cache claims and what the
     * Pico actually applies. Fixed by validating/converting FIRST (pure,
     * no persistence -- safety_cfg_store_ct_cal_channel_gain() +
     * safety_ct_cal_convert() directly, the same math safety_cfg_store_
     * set_ct_cal_input() runs internally), staging/committing to the Pico
     * next, and persisting the local record ONLY once that succeeds. */
    float gain = safety_cfg_store_ct_cal_channel_gain(ch);
    float k_ct_v_per_a = 0.0f;
    uint16_t zero_counts = 0;
    if (!safety_ct_cal_convert(a_fs, zero_mv, gain, &k_ct_v_per_a, &zero_counts)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "CT calibration input rejected (out of range, or an internal error)");
        return ESP_OK;
    }

    static const uint16_t K_CT_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0308, 0x0309, 0x030A };
    static const uint16_t ZERO_COUNTS_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0302, 0x0303, 0x0304 };

    safety_cfg_post_pair_t pairs[2];
    pairs[0].param_id = K_CT_IDS[ch];
    snprintf(pairs[0].value_text, sizeof(pairs[0].value_text), "%.9g", (double)k_ct_v_per_a);
    pairs[1].param_id = ZERO_COUNTS_IDS[ch];
    snprintf(pairs[1].value_text, sizeof(pairs[1].value_text), "%u", (unsigned)zero_counts);

    char reason[160];
    bool ok = apply_pairs(s_link, pairs, 2, commit, reason, sizeof(reason));

    bool persisted = false;
    esp_err_t nvs_err = ESP_OK;
    if (ok) {
        /* Re-runs the same conversion internally (identical inputs, so an
         * identical result) and persists the local record only now that the
         * Pico side has accepted (or staged) the same values. A failure
         * here is only "applied live but will not survive a reboot" -- the
         * pure validation above already proved a_fs/zero_mv/gain are sane,
         * so this can only fail on the NVS write itself. */
        persisted = safety_cfg_store_set_ct_cal_input(ch, a_fs, zero_mv, SAFETY_CT_CAL_SOURCE_MANUAL,
                                                        NULL, NULL, &nvs_err) &&
                    nvs_err == ESP_OK;
    }

    char resp[300];
    int len;
    if (ok && persisted) {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"k_ct_v_per_a\":%.9g,\"zero_counts\":%u,\"persisted\":true}",
                        (double)k_ct_v_per_a, (unsigned)zero_counts);
    } else if (ok) {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"k_ct_v_per_a\":%.9g,\"zero_counts\":%u,\"persisted\":false,"
                        "\"err\":\"%s\"}",
                        (double)k_ct_v_per_a, (unsigned)zero_counts, esp_err_to_name(nvs_err));
    } else {
        char escaped[192];
        size_t o = 0;
        for (const char *c = reason; *c && o + 2 < sizeof(escaped); c++) {
            if (*c == '"' || *c == '\\') {
                escaped[o++] = '\\';
            }
            escaped[o++] = *c;
        }
        escaped[o] = '\0';
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", escaped);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning/ct_auto_zero                            */
/* ---------------------------------------------------------------------- */

/* CT_COMMISSIONING_PLAN.md step 2. Body: "channel=<0-2>&confirm=<0|1>&
 * override_manual=<0|1>". Every request (confirm or not) runs the FULL
 * precondition check and a FRESH measurement -- there is no server-side
 * session between a preview and its confirm, deliberately: trusting a
 * stale preview across two independent HTTP requests would let real
 * conditions (a relay commanded on, a trip, a profile start) drift between
 * "shown to the operator" and "committed" with nothing re-checking them.
 * The cost is one extra ~10s measurement when the operator does confirm --
 * cheap next to the alternative of committing against stale preconditions.
 *
 * confirm=0 (or absent): preconditions + measure + refusal checks, but does
 * NOT touch safety_cfg_store_set_ct_cal_input()/the Pico -- returns the
 * proposed zero_mv-at-probe, the previous stored value, and the delta, for
 * the operator to review.
 * confirm=1: same checks, then commits through the EXACT ct_cal path
 * ct_cal_post_handler() uses (safety_ct_cal_convert() -> apply_pairs() ->
 * safety_cfg_store_set_ct_cal_input(), Pico first, ESP-local record only on
 * success -- see that handler's own 2026-09-06 reorder comment) with
 * source=AUTO_ZERO. Refuses if the channel's current source is MANUAL
 * unless override_manual=1 (manual always wins otherwise, same rule
 * safety_cfg_store_set_ct_cal_input() itself enforces for the SWEEP
 * source -- AUTO_ZERO is deliberately allowed to override manual, but only
 * with this explicit operator opt-in, since AUTO_ZERO overwriting a manual
 * entry the operator typed on purpose is not "the sweep clobbering it
 * silently").
 *
 * Blocks the ENTIRE esp_http_server task for the full measurement
 * (~10-12s at CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES/SAFTYFW_PERIOD_
 * CURRENT_TASK_MS, current_task.c) -- esp_http_server here runs as a single
 * task, not one worker thread per connection, so every other HTTP request
 * (dashboard poll, another commissioning action, OTA, etc.) is stalled for
 * up to ~15s while this handler runs. That is accepted, not overlooked:
 * this is an operator-driven, one-at-a-time commissioning action that
 * itself requires every relay off and no profile/autotune running (see the
 * precondition gate below), so nothing else on the board should be making
 * HTTP calls that matter during the window anyway. This is a DIFFERENT
 * hazard from the link_task/current_task blocking problem this whole
 * async-BEGIN/poll design (link_frame.h's own comment) exists to avoid on
 * the Pico side -- that one risks the 30ms link watchdog deadline, not just
 * HTTP responsiveness. */
#define SAFETY_CT_AUTO_ZERO_BODY_MAX 64
#define SAFETY_CT_AUTO_ZERO_POLL_MS 200u
#define SAFETY_CT_AUTO_ZERO_TIMEOUT_MS 15000u
// CURRENT_SENSE.md's model: zero_mv = zero_counts * (3.3/4096) / gain * 1000.
// Inverse of safety_ct_cal_convert()'s zero_counts formula.
static float ct_auto_zero_counts_to_mv(uint16_t zero_counts, float gain)
{
    if (!(gain > 0.0f)) {
        gain = SAFETY_CT_CAL_DEFAULT_GAIN;
    }
    return ((float)zero_counts * (3.3f / 4096.0f) / gain) * 1000.0f;
}

/* Pure precondition gate, factored out of ct_auto_zero_post_handler() so it
 * is directly host-testable (this file's static functions have no other
 * seam, same reasoning apply_pairs()/parse_set_param_body() are tested this
 * way) without needing a working mock httpd_req_t body-read path. Returns
 * NULL when every precondition is satisfied, or a static reason string
 * (never allocated, safe to httpd_resp_sendstr() as-is) naming the first one
 * that fails, checked in the same order the plan lists them: link/trip/K4,
 * then relay-off duration, then profile/autotune, then manual-wins. */
static const char *ct_auto_zero_check_preconditions(bool link_up, bool trip_latched, bool k4_closed,
                                                     bool have_io, bool relays_on, uint32_t relays_off_ms,
                                                     bool profile_running_or_paused, bool autotune_active,
                                                     bool has_existing,
                                                     safety_ct_cal_source_t existing_source,
                                                     bool override_manual)
{
    if (!link_up) {
        return "safety link is down";
    }
    if (trip_latched) {
        return "a trip is latched";
    }
    if (!k4_closed) {
        return "K4 (safety relay) is not closed";
    }
    if (!have_io) {
        return "board relay I/O not available this boot";
    }
    if (relays_on) {
        return "at least one heater relay is commanded on";
    }
    if (relays_off_ms == UINT32_MAX || relays_off_ms < 5000u) {
        return "relays have not been off for at least 5 s";
    }
    if (profile_running_or_paused) {
        return "a profile is running or paused";
    }
    if (autotune_active) {
        return "autotune is running";
    }
    if (has_existing && existing_source == SAFETY_CT_CAL_SOURCE_MANUAL && !override_manual) {
        return "channel is manually calibrated -- pass override_manual=1 to replace it";
    }
    if (!has_existing) {
        /* A zero-only measurement has nothing to say about A_fs -- the old
         * code filled in a fabricated a_fs_for_convert=1.0f here and wrote a
         * matching k_ct=1.0 (V/A) to the Pico, which is not "unaffected by a
         * zero-only measurement" the way the removed comment claimed, it is
         * a wrong gain silently committed for any channel that had never
         * been calibrated at all. Refuse instead -- auto-zero only ever
         * refines an existing A_fs, never invents one. */
        return "channel has no A_fs yet -- set it manually before auto-zeroing";
    }
    return NULL;
}

/* Pure post-measurement re-check, factored out the same way as
 * ct_auto_zero_check_preconditions() above so it is directly host-testable.
 * The precondition gate above only proves the preconditions held at the
 * MOMENT it ran -- the measurement that follows takes ~10-12s, during which
 * a relay could be commanded on and back off, or a profile/autotune could
 * start, without ever being caught by a check that only ran before the
 * measurement began. Returns NULL when every postcondition still holds, or
 * a static reason string naming the first violation.
 *
 * off_ms_after is read fresh AFTER the poll loop completes; requiring it to
 * be at least 5000 + waited_ms (not just >= 5000) is what actually proves
 * continuity -- kiln_io_relays_off_ms() resets to a small value the instant
 * relay_shadow becomes nonzero even briefly, so a relay that pulsed on and
 * back off mid-measurement shows up here as a value far short of that sum,
 * even though a POINT-IN-TIME "relays on now?" read afterward would already
 * show them off again. */
static const char *ct_auto_zero_check_postconditions(bool relays_on_after, uint32_t off_ms_after,
                                                       uint32_t waited_ms, bool profile_running_or_paused_after,
                                                       bool autotune_active_after)
{
    if (relays_on_after) {
        return "a heater relay is commanded on now -- refusing to commit a measurement that may have been "
               "taken with current flowing";
    }
    if (off_ms_after == UINT32_MAX || off_ms_after < 5000u + waited_ms) {
        return "a relay was energized during the measurement window -- refusing to commit a stale reading";
    }
    if (profile_running_or_paused_after) {
        return "a profile started during the measurement";
    }
    if (autotune_active_after) {
        return "autotune started during the measurement";
    }
    return NULL;
}

static esp_err_t ct_auto_zero_post_handler(httpd_req_t *req)
{
    char body[SAFETY_CT_AUTO_ZERO_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized body");
        return ESP_OK;
    }

    char ch_text[8] = {0}, confirm_text[4] = {0}, override_text[4] = {0};
    if (http_form_find_field(body, "channel", ch_text, sizeof(ch_text)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected channel=<0-2>");
        return ESP_OK;
    }
    bool confirm = (http_form_find_field(body, "confirm", confirm_text, sizeof(confirm_text)) >= 0) &&
                   strcmp(confirm_text, "1") == 0;
    bool override_manual =
        (http_form_find_field(body, "override_manual", override_text, sizeof(override_text)) >= 0) &&
        strcmp(override_text, "1") == 0;

    char *end = NULL;
    long ch_l = strtol(ch_text, &end, 10);
    if (end == ch_text || *end != '\0' || ch_l < 0 || ch_l >= (long)SAFETY_CT_CAL_CHANNELS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel must be 0, 1, or 2");
        return ESP_OK;
    }
    uint8_t channel = (uint8_t)ch_l;

    // --- Preconditions -----------------------------------------------------
    if (!s_link) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"safety link not available this boot\"}");
    }
    /* st and pstat (below) are heap-allocated, internal DRAM, in narrow
     * scopes that end well before the httpd_worker stack-budget pass's
     * target frame is left (2026-09-08) -- both are read once into plain
     * bools/floats right after the call that fills them and freed
     * immediately, so neither has to be tracked across this handler's many
     * later return paths. Internal DRAM, not PSRAM: this handler's confirm
     * path (below) reaches flash via safety_cfg_store_set_ct_cal_input(),
     * and a PSRAM-backed allocation touched around a flash write is the
     * known panic class documented elsewhere in this codebase -- these two
     * buffers are unrelated to that write, but internal DRAM for every
     * heap buffer in a handler that has ANY flash-writing path removes the
     * question rather than depending on "this particular buffer's lifetime
     * doesn't overlap the write" staying true after a future edit.
     * MALLOC_CAP_8BIT alone does NOT guarantee internal DRAM -- with PSRAM
     * enabled it is satisfiable from PSRAM too, so every allocation in this
     * handler spells out MALLOC_CAP_INTERNAL explicitly rather than relying
     * on CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL's threshold to keep landing
     * these small structs in internal RAM by coincidence
     * (docs/audits/unreviewed_changes_review_2026-09-08.md finding D3). */
    bool link_up, trip_latched, k4_closed;
    {
        safety_link_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!st) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_OK;
        }
        memset(st, 0, sizeof(*st));
        link_up = (safety_link_get_status(s_link, st) == ESP_OK) && st->link_up;
        trip_latched = st->fault_asserted ||
                       (st->diag_ever_received && st->diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
        k4_closed = (st->flags & SAFETY_FLAG_RELAY) != 0u;
        free(st);
    }
    bool have_io = s_hw_io != NULL;
    bool relays_on = have_io && kiln_io_get_relay_shadow(s_hw_io) != 0u;
    uint32_t off_ms = have_io ? kiln_io_relays_off_ms(s_hw_io) : UINT32_MAX;
    bool profile_running_or_paused;
    {
        profile_exec_status_t *pstat = heap_caps_malloc(sizeof(*pstat), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!pstat) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_OK;
        }
        memset(pstat, 0, sizeof(*pstat));
        profile_executor_get_status(pstat);
        profile_running_or_paused = (pstat->state == PROFILE_EXEC_RUNNING || pstat->state == PROFILE_EXEC_PAUSED);
        free(pstat);
    }
    bool autotune_active = autotune_engine_is_active();

    safety_ct_cal_source_t existing_source = SAFETY_CT_CAL_SOURCE_MANUAL;
    float existing_a_fs = 0.0f, existing_zero_mv = 0.0f;
    bool has_existing =
        safety_cfg_store_get_ct_cal_input(channel, &existing_a_fs, &existing_zero_mv, &existing_source);

    const char *refusal = ct_auto_zero_check_preconditions(
        link_up, trip_latched, k4_closed, have_io, relays_on, off_ms, profile_running_or_paused,
        autotune_active, has_existing, existing_source, override_manual);
    if (refusal) {
        char resp[192];
        int len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", refusal);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    // --- Measure -------------------------------------------------------------
    esp_err_t begin_err = safety_link_send_ct_auto_zero_begin(s_link, channel);
    if (begin_err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"could not send the auto-zero request\"}");
    }

    /* "Reset one side of a pair" hazard: DONE latches on the Pico until the
     * NEXT BEGIN (see kilnlink_ct_auto_zero_status.h), so if THIS request's
     * BEGIN frame is lost on the wire, the very first poll below can see a
     * DONE that is actually the previous measurement's stale result --
     * accepting it silently would commit an old channel's old reading under
     * this request's name. The disambiguator is `state` reaching IN_PROGRESS:
     * a fresh BEGIN always drives the state to IN_PROGRESS before it ever
     * reaches DONE, so DONE is only trusted once this loop has actually
     * observed that transition for itself. */
    kilnlink_ct_auto_zero_status_t az = {0};
    uint32_t waited_ms = 0;
    bool done = false;
    bool stale_done = false;
    bool observed_in_progress = false;
    while (waited_ms < SAFETY_CT_AUTO_ZERO_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(SAFETY_CT_AUTO_ZERO_POLL_MS));
        waited_ms += SAFETY_CT_AUTO_ZERO_POLL_MS;
        if (safety_link_get_ct_auto_zero_status(s_link, &az) != ESP_OK) {
            continue; // transient poll miss -- keep trying within the overall timeout
        }
        if (az.channel != channel) {
            continue; // status for some other channel's earlier request
        }
        if (az.state == KILNLINK_CT_AUTO_ZERO_STATE_IN_PROGRESS) {
            observed_in_progress = true;
            continue;
        }
        if (az.state == KILNLINK_CT_AUTO_ZERO_STATE_DONE) {
            if (!observed_in_progress) {
                stale_done = true;
            } else {
                done = true;
            }
            break;
        }
    }
    if (stale_done) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"measurement did not start -- the Pico "
                                        "still reports a stale result from an earlier request; check the "
                                        "safety link\"}");
    }
    if (!done) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"measurement did not complete in time -- "
                                        "check the Pico link\"}");
    }

    // --- Re-check preconditions: the measurement above took ~10-12s, during
    // which a relay, profile, or autotune could have started and stopped
    // again without ever being caught by the point-in-time gate above. See
    // ct_auto_zero_check_postconditions()'s own comment. ---
    bool relays_on_after = have_io && kiln_io_get_relay_shadow(s_hw_io) != 0u;
    uint32_t off_ms_after = have_io ? kiln_io_relays_off_ms(s_hw_io) : UINT32_MAX;
    // Second narrow heap-scoped profile_exec_status_t read, same reasoning
    // and same internal-DRAM choice as the first one above -- not a stack
    // local, so there is no frame cost to "declaring a second one" any more.
    bool profile_running_or_paused_after;
    {
        profile_exec_status_t *pstat2 = heap_caps_malloc(sizeof(*pstat2), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!pstat2) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_OK;
        }
        memset(pstat2, 0, sizeof(*pstat2));
        profile_executor_get_status(pstat2);
        profile_running_or_paused_after =
            (pstat2->state == PROFILE_EXEC_RUNNING || pstat2->state == PROFILE_EXEC_PAUSED);
        free(pstat2);
    }
    bool autotune_active_after = autotune_engine_is_active();
    const char *post_refusal = ct_auto_zero_check_postconditions(
        relays_on_after, off_ms_after, waited_ms, profile_running_or_paused_after, autotune_active_after);
    if (post_refusal) {
        char resp[192];
        int len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", post_refusal);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    float gain = safety_cfg_store_ct_cal_channel_gain(channel);
    float measured_zero_mv = ct_auto_zero_counts_to_mv(az.zero_counts, gain);
    float delta_mv = has_existing ? (measured_zero_mv - existing_zero_mv) : measured_zero_mv;

    // "current flowing with every relay off is S3's fault condition and must
    // not be calibrated away" -- CT_COMMISSIONING_PLAN.md step 2.
    if (fabsf(delta_mv) > 100.0f) {
        char resp[256];
        int len = snprintf(resp, sizeof(resp),
                            "{\"ok\":false,\"reason\":\"measured zero (%.2f mV) differs from the stored "
                            "value (%.2f mV) by more than 100 mV -- this looks like real current, not "
                            "offset drift; refusing to calibrate it away\",\"measured_zero_mv\":%.3f,"
                            "\"previous_zero_mv\":%.3f,\"delta_mv\":%.3f}",
                            (double)measured_zero_mv, (double)existing_zero_mv, (double)measured_zero_mv,
                            (double)existing_zero_mv, (double)delta_mv);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    if (!confirm) {
        char resp[300];
        int len = snprintf(resp, sizeof(resp),
                            "{\"ok\":true,\"phase\":\"measured\",\"channel\":%u,\"samples\":%u,"
                            "\"measured_zero_mv\":%.3f,\"previous_zero_mv\":%.3f,\"delta_mv\":%.3f,"
                            "\"manual_conflict\":%s}",
                            (unsigned)channel, (unsigned)az.samples_taken, (double)measured_zero_mv,
                            (double)existing_zero_mv, (double)delta_mv,
                            (has_existing && existing_source == SAFETY_CT_CAL_SOURCE_MANUAL) ? "true"
                                                                                              : "false");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    // --- Commit (confirm=1) -- same order as ct_cal_post_handler(): Pico
    // first, ESP-local record only once the Pico side has accepted it. ---
    // has_existing is guaranteed true here -- ct_auto_zero_check_preconditions()
    // above already refused the whole request when the channel has no A_fs
    // yet, so existing_a_fs is always a real, previously-validated value.
    float a_fs_for_convert = existing_a_fs;
    float k_ct_v_per_a = 0.0f;
    uint16_t zero_counts = az.zero_counts;
    if (!safety_ct_cal_convert(a_fs_for_convert, measured_zero_mv, gain, &k_ct_v_per_a, &zero_counts)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"measured value rejected by conversion "
                                        "(out of range)\"}");
    }

    static const uint16_t K_CT_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0308, 0x0309, 0x030A };
    static const uint16_t ZERO_COUNTS_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0302, 0x0303, 0x0304 };
    safety_cfg_post_pair_t pairs[2];
    pairs[0].param_id = K_CT_IDS[channel];
    snprintf(pairs[0].value_text, sizeof(pairs[0].value_text), "%.9g", (double)k_ct_v_per_a);
    pairs[1].param_id = ZERO_COUNTS_IDS[channel];
    snprintf(pairs[1].value_text, sizeof(pairs[1].value_text), "%u", (unsigned)zero_counts);

    char reason[160];
    bool ok = apply_pairs(s_link, pairs, 2, true /* always commit on confirm */, reason, sizeof(reason));

    bool persisted = false;
    esp_err_t nvs_err = ESP_OK;
    if (ok) {
        persisted = safety_cfg_store_set_ct_cal_input(channel, a_fs_for_convert, measured_zero_mv,
                                                        SAFETY_CT_CAL_SOURCE_AUTO_ZERO, NULL, NULL,
                                                        &nvs_err) &&
                    nvs_err == ESP_OK;
    }

    char resp[300];
    int len;
    if (ok && persisted) {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"phase\":\"committed\",\"channel\":%u,\"zero_mv\":%.3f,"
                        "\"persisted\":true}",
                        (unsigned)channel, (double)measured_zero_mv);
    } else if (ok) {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"phase\":\"committed\",\"channel\":%u,\"zero_mv\":%.3f,"
                        "\"persisted\":false,\"err\":\"%s\"}",
                        (unsigned)channel, (double)measured_zero_mv, esp_err_to_name(nvs_err));
    } else {
        char escaped[192];
        size_t o = 0;
        for (const char *c = reason; *c && o + 2 < sizeof(escaped); c++) {
            if (*c == '"' || *c == '\\') {
                escaped[o++] = '\\';
            }
            escaped[o++] = *c;
        }
        escaped[o] = '\0';
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", escaped);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning/bench_preset                            */
/* ---------------------------------------------------------------------- */

/* COMMISSIONING.md sec 4.1: "A dev-only preset may be applied for bench
 * testing... It must be VISIBLY a bench preset: applying it leaves
 * calibration_missing set." The four sec-1 fields (tc_source,
 * borrowed_zone_index, tc_placement_mode, abs_max_temp_c -- tc_type and
 * ct_channel_map are also sec-1 but not in the "no compiled-in default"
 * quartet CONFIG_REFERENCE.md sec 1 names) are DELIBERATELY NOT in this
 * table: CONFIG_REFERENCE.md sec 1 itself says the guards depending on them
 * "stay disabled" and calibration_missing stays true "until each is set" --
 * leaving them unset is what guarantees calibration_missing survives this
 * preset, REGARDLESS of exactly which cross-field rule the Pico's
 * COMMIT_CONFIG happens to check for "commissioned". Every value below is
 * CONFIG_REFERENCE.md's own documented default for secs 2-5 (the table this
 * preset is drawn from verbatim) -- a bench tester gets a board that behaves
 * exactly like a freshly-flashed one for every guard that HAS a safe
 * default, with only the four no-default fields left for a real
 * commissioning pass to fill in. mains_voltage_v is also left out: sec 3's
 * own table says "Unset -> report --, never assume", so there is no
 * "bench value" for it either -- the same reasoning as the sec-1 quartet,
 * just for a cosmetic-risk field instead of a dangerous one. */
static const struct {
    uint16_t id;
    uint8_t type;
    kilnlink_param_value_t value;
} SAFETY_CFG_BENCH_PRESET[] = {
    { 0x0201, KILNLINK_PARAM_TYPE_F32, { .f32_val = 100.0f } },  /* firing_margin_c */
    { 0x0202, KILNLINK_PARAM_TYPE_F32, { .f32_val = 75.0f } },   /* overshoot_margin_c */
    { 0x0203, KILNLINK_PARAM_TYPE_U16, { .u16_val = 120 } },     /* overshoot_time_s */
    { 0x0204, KILNLINK_PARAM_TYPE_F32, { .f32_val = 0.0f } },    /* max_rate_c_per_min (ships disabled) */
    { 0x0205, KILNLINK_PARAM_TYPE_U16, { .u16_val = 60 } },      /* rate_window_s */
    { 0x0206, KILNLINK_PARAM_TYPE_U16, { .u16_val = 60 } },      /* blind_grace_s */
    { 0x0207, KILNLINK_PARAM_TYPE_U16, { .u16_val = 600 } },     /* frozen_window_s */
    { 0x0208, KILNLINK_PARAM_TYPE_F32, { .f32_val = 200.0f } },  /* tc_disagreement_c */
    { 0x0209, KILNLINK_PARAM_TYPE_U16, { .u16_val = 300 } },     /* tc_disagreement_time_s */
    { 0x020A, KILNLINK_PARAM_TYPE_F32, { .f32_val = 0.0f } },    /* tc_expected_offset_c */
    { 0x020B, KILNLINK_PARAM_TYPE_F32, { .f32_val = 60.0f } },   /* cj_warn_c */
    { 0x020C, KILNLINK_PARAM_TYPE_F32, { .f32_val = 85.0f } },   /* cj_max_c */
    { 0x020D, KILNLINK_PARAM_TYPE_U16, { .u16_val = 60 } },      /* cj_time_s */
    { 0x020E, KILNLINK_PARAM_TYPE_U16, { .u16_val = 10 } },      /* borrowed_stale_s */
    { 0x020F, KILNLINK_PARAM_TYPE_U16, { .u16_val = 60 } },      /* borrowed_stale_trip_s */
    { 0x0301, KILNLINK_PARAM_TYPE_F32, { .f32_val = 2.0f } },    /* i_present_a */
    { 0x0305, KILNLINK_PARAM_TYPE_U16, { .u16_val = 150 } },     /* correlation_window_s */
    { 0x0306, KILNLINK_PARAM_TYPE_U16, { .u16_val = 20 } },      /* stuck_on_time_s */
    { 0x0307, KILNLINK_PARAM_TYPE_U16, { .u16_val = 10 } },      /* trip_verify_s */
    { 0x030B, KILNLINK_PARAM_TYPE_F32, { .f32_val = 0.715f } },  /* gain[0] -- R46/R43 physical default */
    { 0x030C, KILNLINK_PARAM_TYPE_F32, { .f32_val = 0.715f } },  /* gain[1] */
    { 0x030D, KILNLINK_PARAM_TYPE_F32, { .f32_val = 0.715f } },  /* gain[2] */
    { 0x030F, KILNLINK_PARAM_TYPE_U16, { .u16_val = 120 } },     /* power_window_s */
    { 0x0401, KILNLINK_PARAM_TYPE_U16, { .u16_val = 5 } },       /* context_max_age_s */
    { 0x0402, KILNLINK_PARAM_TYPE_U16, { .u16_val = 10 } },      /* link_timeout_s */
    { 0x0403, KILNLINK_PARAM_TYPE_U16, { .u16_val = 120 } },     /* link_dead_hard_s */
    { 0x0404, KILNLINK_PARAM_TYPE_U16, { .u16_val = 200 } },     /* mainfault_debounce_ms */
    { 0x0405, KILNLINK_PARAM_TYPE_U16, { .u16_val = 500 } },     /* telemetry_period_ms */
    { 0x0501, KILNLINK_PARAM_TYPE_U16, { .u16_val = 60 } },      /* startup_grace_s */
    { 0x0502, KILNLINK_PARAM_TYPE_U16, { .u16_val = 50 } },      /* estop_debounce_ms */
    { 0x0503, KILNLINK_PARAM_TYPE_U16, { .u16_val = 1000 } },    /* watchdog_timeout_ms */
    { 0x0504, KILNLINK_PARAM_TYPE_U16, { .u16_val = 10 } },      /* config_check_period_s */
};
#define SAFETY_CFG_BENCH_PRESET_COUNT (sizeof(SAFETY_CFG_BENCH_PRESET) / sizeof(SAFETY_CFG_BENCH_PRESET[0]))

static esp_err_t bench_preset_post_handler(httpd_req_t *req)
{
    if (!s_link) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "safety link not available on this board");
        return ESP_OK;
    }
    for (size_t i = 0; i < SAFETY_CFG_BENCH_PRESET_COUNT; i++) {
        esp_err_t err = safety_link_send_set_param(s_link, SAFETY_CFG_BENCH_PRESET[i].id,
                                                    SAFETY_CFG_BENCH_PRESET[i].type,
                                                    SAFETY_CFG_BENCH_PRESET[i].value);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bench_preset: staging id 0x%04X failed: %s",
                     (unsigned)SAFETY_CFG_BENCH_PRESET[i].id, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "communication with the safety processor failed mid-preset");
            return ESP_OK;
        }
    }
    uint16_t reject_param_id = 0;
    uint8_t reject_reason = 0;
    bool rejected = false;
    esp_err_t commit_err = safety_link_send_commit_config(s_link, &reject_param_id, &reject_reason, &rejected);
    if (commit_err != ESP_OK) {
        ESP_LOGW(TAG, "bench_preset: commit failed: %s", esp_err_to_name(commit_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             "bench preset staged but the safety processor did not acknowledge the commit");
        return ESP_OK;
    }
    if (rejected) {
        /* Not expected in practice (the preset's own values are chosen to
         * pass CONFIG_REFERENCE.md's range/contradiction rules -- see this
         * function's own header comment), but report it honestly rather than
         * claiming {"ok":true} for a commit the Pico actually refused. */
        ESP_LOGW(TAG, "bench_preset: commit REJECTED (param_id=0x%04X, reason=%u)", (unsigned)reject_param_id,
                 (unsigned)reject_reason);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bench preset staged but the safety "
                                        "processor rejected the commit\"}");
    }
    ESP_LOGI(TAG, "bench_preset: applied (%u fields) -- calibration_missing remains set, "
                  "the sec-1 commissioning fields were deliberately not sent",
             (unsigned)SAFETY_CFG_BENCH_PRESET_COUNT);
    /* SAFETY_CFG_BENCH_PRESET always includes 0x0204 (max_rate_c_per_min),
     * reset to its dormant 0.0 default -- this record's provenance for
     * whatever value the guard held BEFORE the preset is now meaningless,
     * and leaving it standing would show a stale "auto-derived"/"hand-
     * entered" label next to a value the preset just replaced. */
    safety_cfg_store_clear_rate_guard_meta();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------------------------------------------------------------------- */
/* GET/POST /api/safety/rate_guard/auto -- S8 auto-calc write path         */
/* docs/audits/s8_auto_calc_design_2026-09-09.md "Part 3"                 */
/* ---------------------------------------------------------------------- */

/* Gathers every zone's identified model + fit context into s8_rate_guard_
 * estimate()'s input shape and runs it. Pure gathering (no I/O beyond the
 * zones_config accessors every other read-only endpoint in this codebase
 * already calls directly, e.g. readiness_http.c's own per-zone loop) --
 * kept separate from the handler below so the decision logic can be
 * exercised without an httpd_req_t. */
static s8_rate_guard_estimate_reason_t rate_guard_gather_and_estimate(float *out_c_per_min)
{
    s8_rate_guard_zone_input_t zones[MAX31856_CHANNEL_COUNT];
    memset(zones, 0, sizeof(zones));
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT; /* defensive, matches s8_rate_guard_estimate()'s own clamp */
    }
    for (uint8_t i = 0; i < thermo_count; i++) {
        float k_dc = 0.0f, tau_s = 0.0f, dead_time_s = 0.0f;
        float fit_temp_c = 0.0f, fit_ambient_c = 0.0f;
        bool have_model = zones_config_get_model(i, &k_dc, &tau_s, &dead_time_s) && k_dc > 0.0f && tau_s > 0.0f;
        bool have_fit_ctx = zones_config_get_model_fit_context(i, &fit_temp_c, &fit_ambient_c);
        /* 2026-09-10 fix: `have_fit_ctx` alone does not mean the fit is
         * USABLE -- a zone migrated up from a pre-v24 record reads back
         * ZONE_MODEL_FIT_TEMP_UNKNOWN successfully (the accessor call
         * succeeds; it just returns the sentinel), and that sentinel is
         * finite, so s8_rate_guard_estimate() cannot be relied on alone to
         * catch it if this flag admits it as "valid" input from a caller
         * that otherwise looks fine. s8_rate_guard_estimate() independently
         * rejects the sentinel too (belt and braces, since this was the
         * exact live bug: this flag WAS built as `have_model && have_fit_
         * ctx` with no sentinel check, and the sentinel is what every zone
         * on this bench carries today, pre-any-post-v24 re-identification).
         */
        bool fit_ctx_usable = have_fit_ctx && fit_temp_c != ZONE_MODEL_FIT_TEMP_UNKNOWN;
        zones[i].valid = have_model && fit_ctx_usable;
        zones[i].k_dc = k_dc;
        zones[i].tau_s = tau_s;
        zones[i].fit_temp_c = fit_temp_c;

        /* Sum of the OTHER zones' measured steady-state coupling gain onto
         * THIS zone -- S8 watches one TC shared by all zones, so the
         * all-zones-full-duty basis needs this; see s8_rate_guard_estimate.h
         * "REAL FIRINGS ARE COUPLED". zones_config_get_coupling()'s row is
         * [affected=i][stepped=j]; exclude the diagonal (this zone's own
         * gain, already counted via k_dc) and sum the rest. A row read
         * failure (uncommissioned coupling) leaves the sum at 0.0f, which
         * for a genuinely single-zone board is correct and for a multi-zone
         * board that has not run coupling identification yet is the best
         * available answer -- the same "no worse than the previous
         * own-zone-only basis" floor, not a claim of full coverage. */
        float coupling_row[MAX31856_CHANNEL_COUNT] = {0};
        float coupling_sum = 0.0f;
        if (zones_config_get_coupling(i, coupling_row)) {
            for (uint8_t j = 0; j < thermo_count && j < MAX31856_CHANNEL_COUNT; j++) {
                if (j == i) {
                    continue;
                }
                coupling_sum += coupling_row[j];
            }
        }
        zones[i].coupling_gain_sum_c_per_duty = coupling_sum;
    }
    return s8_rate_guard_estimate(zones, thermo_count, out_c_per_min);
}

/* Reads the CURRENT value of 0x0204 (max_rate_c_per_min) from this ESP's
 * cache -- NOT a live fetch (same cache commissioning_get_handler's own JSON
 * reads from). `*out_is_set` follows the same M1 "unset means unset,
 * regardless of what the raw cache says, on a peer too old to report it
 * reliably" rule the rest of this file already applies -- but since this
 * path only ever runs on a board with a link up (the estimate itself needs
 * a live plant model, which only exists post-commissioning), treating an
 * unreliable-unset peer as "not set" is the conservative direction: it
 * makes s8_rate_guard_auto_decide() treat the guard as dormant and APPLY,
 * which is only wrong in the (already out-of-support) direction of a
 * pre-v8 Pico, and even then only skips a confirm step for a guard whose
 * real current value this build cannot trust anyway. */
static bool rate_guard_current_value(float *out_value, bool *out_is_set)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t p;
        if (!safety_cfg_store_get_by_index(i, &p) || p.param_id != 0x0204) {
            continue;
        }
        if (!p.set) {
            *out_is_set = false;
            return true;
        }
        *out_value = p.value.f32_val;
        *out_is_set = true;
        return true;
    }
    return false; /* 0x0204 not in this build's table at all -- should not happen */
}

/* Shared by both GET (preview) and POST (act): computes the candidate,
 * reads the current value, and classifies. Returns false (nothing else
 * meaningful) only if no zone has a usable identification, or 0x0204's
 * current value could not be read at all. */
static bool rate_guard_auto_compute(float *out_candidate, float *out_current, bool *out_current_is_set,
                                     s8_rate_guard_auto_decision_t *out_decision, char *err_out,
                                     size_t err_cap)
{
    s8_rate_guard_estimate_reason_t reason = rate_guard_gather_and_estimate(out_candidate);
    /* OK and both CLAMPED_* variants all produce a usable *out_candidate --
     * see s8_rate_guard_estimate.h's enum comment. Only NO_DATA means no
     * candidate was produced at all. The CLAMPED distinction exists for the
     * caller (the GET/POST handlers below) to surface to the operator, not
     * to gate whether a candidate exists. */
    if (reason != S8_RATE_GUARD_ESTIMATE_OK && reason != S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR &&
        reason != S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_CEILING) {
        snprintf(err_out, err_cap, "no zone has a usable identification yet (run autotune on at "
                                    "least one zone first)");
        return false;
    }
    if (!rate_guard_current_value(out_current, out_current_is_set)) {
        snprintf(err_out, err_cap, "internal error: max_rate_c_per_min (0x0204) missing from this "
                                    "build's parameter table");
        return false;
    }
    *out_decision = s8_rate_guard_auto_decide(*out_candidate, *out_current, *out_current_is_set);
    return true;
}

/* GET: compute-and-report only, never writes anything -- safe to poll from
 * the commissioning page on every load, same as the rest of this endpoint's
 * GET side. */
static esp_err_t rate_guard_auto_get_handler(httpd_req_t *req)
{
    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision = S8_RATE_GUARD_AUTO_APPLY;
    char err[160];
    char resp[256];
    int len;
    if (!rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, err, sizeof(err))) {
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", err);
    } else {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"candidate_c_per_min\":%.6g,\"current_set\":%s,"
                        "\"current_c_per_min\":%.6g,\"would_loosen\":%s}",
                        (double)candidate, current_is_set ? "true" : "false", (double)current,
                        decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* POST: body is either empty (act on the tighten/apply policy only) or
 * "confirm=1" (operator has reviewed a loosening suggestion from a prior GET
 * and explicitly wants it applied anyway -- the ONLY way a loosening ever
 * gets written by this endpoint). Every write goes through apply_pairs()
 * (commit=true), which forces confirm_commit_landed()'s live read-back --
 * same "never trust SET_PARAM/COMMIT_CONFIG's own ok" discipline as every
 * other write on this page -- before this handler tags the provenance
 * record or reports success. */
static esp_err_t rate_guard_auto_post_handler(httpd_req_t *req)
{
    bool confirm = false;
    if (req->content_len > 0) {
        char body[32];
        if (read_body(req, body, sizeof(body))) {
            char value[4] = {0};
            confirm = http_form_find_field(body, "confirm", value, sizeof(value)) >= 0 &&
                      strcmp(value, "1") == 0;
        }
    }

    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision = S8_RATE_GUARD_AUTO_APPLY;
    char err[160];
    char resp[320];
    int len;
    if (!rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, err, sizeof(err))) {
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", err);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    if (decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY && !confirm) {
        /* Never loosen silently -- report the suggestion, write nothing. */
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"applied\":false,\"candidate_c_per_min\":%.6g,"
                        "\"current_c_per_min\":%.6g,\"reason\":\"this would LOOSEN the guard from "
                        "%.6g to %.6g C/min -- POST again with confirm=1 to apply it anyway\"}",
                        (double)candidate, (double)current, (double)current, (double)candidate);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    safety_cfg_post_pair_t pair;
    pair.param_id = 0x0204;
    snprintf(pair.value_text, sizeof(pair.value_text), "%.9g", (double)candidate);

    char reason[160];
    bool ok = apply_pairs(s_link, &pair, 1, true /* always commit -- this endpoint has no stage-only mode */,
                           reason, sizeof(reason));
    if (!ok) {
        char escaped[192];
        size_t o = 0;
        for (const char *c = reason; *c && o + 2 < sizeof(escaped); c++) {
            if (*c == '"' || *c == '\\') {
                escaped[o++] = '\\';
            }
            escaped[o++] = *c;
        }
        escaped[o] = '\0';
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", escaped);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
    }

    /* apply_pairs() already forced a live read-back that confirms 0x0204
     * now reads `candidate` on the Pico -- only NOW is it correct to tag
     * this record AUTO. A failed persist here (out_nvs_err) costs a UI
     * label on the next boot, never the safety value itself, so it does not
     * change this response. */
    esp_err_t nvs_err = ESP_OK;
    safety_cfg_store_set_rate_guard_meta(SAFETY_RATE_GUARD_SOURCE_AUTO, candidate, &nvs_err);

    len = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"applied\":true,\"value_c_per_min\":%.6g,\"was_loosen\":%s}",
                    (double)candidate, (decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- The page itself ---------------------------------------------------
 *
 * Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html in
 * this component, and the same defensive Accept-Encoding check every one of
 * them makes before relying on the client understanding gzip. */
extern const uint8_t safety_commissioning_page_html_gz_start[] asm(
    "_binary_safety_commissioning_page_html_gz_start");
extern const uint8_t safety_commissioning_page_html_gz_end[] asm(
    "_binary_safety_commissioning_page_html_gz_end");

static esp_err_t commissioning_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "safety_commissioning_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)safety_commissioning_page_html_gz_start,
                           (size_t)(safety_commissioning_page_html_gz_end -
                                    safety_commissioning_page_html_gz_start));
}

/* ---------------------------------------------------------------------- */

esp_err_t safety_cfg_http_start(SafetyLinkClass *link_or_null, kiln_io_t *io_or_null)
{
    s_link = link_or_null;
    s_hw_io = io_or_null;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/api/safety/commissioning", .method = HTTP_GET, .handler = commissioning_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/safety/commissioning", .method = HTTP_POST, .handler = commissioning_post_handler,
    };
    static const httpd_uri_t bench_uri = {
        .uri = "/api/safety/commissioning/bench_preset", .method = HTTP_POST,
        .handler = bench_preset_post_handler,
    };
    static const httpd_uri_t relay_type_uri = {
        .uri = "/api/safety/commissioning/relay_type", .method = HTTP_POST,
        .handler = relay_type_post_handler,
    };
    static const httpd_uri_t ct_cal_uri = {
        .uri = "/api/safety/commissioning/ct_cal", .method = HTTP_POST,
        .handler = ct_cal_post_handler,
    };
    static const httpd_uri_t ct_auto_zero_uri = {
        .uri = "/api/safety/commissioning/ct_auto_zero", .method = HTTP_POST,
        .handler = ct_auto_zero_post_handler,
    };
    static const httpd_uri_t rate_guard_auto_get_uri = {
        .uri = "/api/safety/rate_guard/auto", .method = HTTP_GET, .handler = rate_guard_auto_get_handler,
    };
    static const httpd_uri_t rate_guard_auto_post_uri = {
        .uri = "/api/safety/rate_guard/auto", .method = HTTP_POST, .handler = rate_guard_auto_post_handler,
    };

    /* The HTML page. Registered alongside the API rather than in a separate
     * module because the two are useless apart -- and because a missing page
     * route is invisible from a browser: an unknown path 302s to "/" (the
     * captive-portal behaviour every route on this server inherits), so a
     * failed registration would look like a working redirect rather than an
     * error. The loop below returning on the first failure is what makes it
     * visible in the log instead. */
    static const httpd_uri_t page_uri = {
        .uri = "/safety/commissioning", .method = HTTP_GET,
        .handler = commissioning_page_get_handler,
    };

    const httpd_uri_t *uris[] = { &page_uri,       &get_uri,          &post_uri,       &bench_uri,
                                   &relay_type_uri, &ct_cal_uri,       &ct_auto_zero_uri,
                                   &rate_guard_auto_get_uri, &rate_guard_auto_post_uri };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s", uris[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "safety commissioning API up (link_or_null=%p, %u known params)", (void *)link_or_null,
             (unsigned)safety_cfg_store_param_count());
    return ESP_OK;
}
