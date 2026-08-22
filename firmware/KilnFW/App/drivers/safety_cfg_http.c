#include "safety_cfg_http.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_form.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "safety_cfg_store.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "safety_cfg_http";

static SafetyLinkClass *s_link = NULL;

/* Small bodies -- up to SAFETY_CFG_PARAM_COUNT id/value pairs plus commit=1,
 * "id=<n>&value=<v>" repeated per field (see parse_set_param_body()'s own
 * comment for why this shape, not a single-key form, is what this handler
 * expects). Generous headroom: worst case every one of the 57 known fields
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
} safety_cfg_http_snapshot_t;

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
    APPEND(",\"params\":[");

    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t p;
        if (!safety_cfg_store_get_by_index(i, &p)) {
            continue; /* cannot happen for i < count, defensive only */
        }
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"set\":%s", i == 0 ? "" : ",",
               (unsigned)p.param_id, p.name, type_name(p.type), p.set ? "true" : "false");
        /* COMMISSIONING.md sec 3.1: "An unset parameter carries `"set": false`
         * and OMITS `value` entirely rather than sending a zero the page
         * might print." -- the omission happens here, not by printing a
         * sentinel, so the page literally cannot mistake it for a real 0. */
        if (p.set) {
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
        }
        uint16_t peer_crc = 0;
        bool peer_known = false;
        (void)safety_link_get_peer_build_status(s_link, &peer_known, NULL, NULL, NULL, NULL, NULL, NULL,
                                                  &peer_crc);
        snap.live_crc_known = peer_known;
        snap.live_crc = peer_crc;
    }
    snap.cached_crc = safety_cfg_store_cached_crc();
    uint32_t fetched = safety_cfg_store_fetched_ms_ago();
    snap.fetched_ms_ago_or_neg1 = (fetched == UINT32_MAX) ? -1 : (int64_t)fetched;

    static char json[SAFETY_CFG_JSON_MAX];
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
    }
    reason_out[0] = '\0';
    return true;
}

static esp_err_t commissioning_post_handler(httpd_req_t *req)
{
    static char body[SAFETY_CFG_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    static safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body(body, pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed id/value pairs");
        return ESP_OK;
    }

    char reason[160];
    bool ok = apply_pairs(s_link, pairs, n, commit, reason, sizeof(reason));

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
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

esp_err_t safety_cfg_http_start(SafetyLinkClass *link_or_null)
{
    s_link = link_or_null;

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

    const httpd_uri_t *uris[] = { &page_uri, &get_uri, &post_uri, &bench_uri };
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
