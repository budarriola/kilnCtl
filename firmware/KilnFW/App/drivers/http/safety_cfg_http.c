#include "safety_cfg_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

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
#include "freertos/task.h" // vTaskDelay/pdMS_TO_TICKS -- ct_auto_zero_job()'s poll loop

#include "hal_time.h" /* hal_time_now_us() -- HAL_INCLUDE_BOUNDARY: this file must not include esp_timer.h
                        * directly, same convention safety_ceiling_sync.c's own include documents */
#include "http_async_job.h" /* docs/HTTP_POST_OWNER_MIGRATION.md slice A1 -- ct_auto_zero_post_handler()
                              * hands the ~10-15s measurement+commit off to http_async_job_try_start()
                              * instead of blocking httpd_worker inline. */
#include "http_form.h"
#include "kiln_cfg_store.h" /* kiln_cfg_store_autosave_from_live() -- 2026-09-15 review HIGH 3,
                              * commissioning_post_handler()'s own comment below */
#include "kiln_package.h" /* kiln_pkg_safety_t -- safety_cfg_http_apply_package_and_confirm(), item 5 */
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "safety_ceiling_sync.h" /* SAFETY_PARAM_ID_ABS_MAX_TEMP_C -- apply_package_and_confirm()'s
                                  * ceiling-field exclusion, item 5. No cycle: safety_ceiling_sync.h
                                  * only includes safety_ceiling_policy.h/safety_link.h, never this file. */
#include "safety_ceiling_policy.h" /* safety_ceiling_refusal_class_t -- 2026-09-10 opus review finding */
#include "safety_cfg_store.h"
#include "safety_cfg_writer_guard.h" /* single-flight vs every other Pico safety-config writer */
#include "safety_trip_words.h" /* safety_fault_source_short_name() -- 2026-09-24 fault-edge instrumentation */
#include "safety_cfg_write.h" /* safety_cfg_post_pair_t + the stage/commit/confirm-by-
                               * read-back primitive. It moved to drivers/safety/ on
                               * 2026-09-16: writing a safety parameter over the safety
                               * link is safety-layer work, and a safety module having to
                               * include THIS http/ header to reach it was a layering
                               * inversion that produced a real zeroed-ceiling defect.
                               * This file is now a CALLER; body parsing stays here. */
#include "s8_rate_guard_estimate.h" // S8 rate-guard auto-calc write path (docs/audits/s8_auto_calc_design_2026-09-09.md)
#include "zones_config_accessors.h" // zones_config_get_model()/_get_model_fit_context() -- s8 auto-calc's input
#include "zones_config_query.h"     // zones_config_get_thermo_count()
#include "zone_coupling_solve.h"    // zone_coupling_matrix_provenance_ok()/_use_measured_diag_k_dc()
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

/* Single-flight wrapper for every SYNCHRONOUS SET_PARAM/COMMIT_CONFIG writer
 * in this file (docs/HTTP_POST_OWNER_MIGRATION.md A2 residual). Claims
 * SAFETY_CFG_WRITER_HTTP_SYNC for the whole handler body and releases it on
 * the way out, owner-checked, whatever the body returned -- so no early
 * return inside `fn` can leak the claim. A claim refusal (an async job, sweep,
 * kiln config swap, reconcile tick or another sync writer owns the guard) is
 * a 409 with the same busy JSON this file always sent, nothing staged.
 * The claim is only a flag in a leaf spinlock: it is taken with no module
 * lock held and `fn` takes its own locks below it, never the reverse. Adds
 * one pointer-sized frame; no buffer. */
static esp_err_t cfg_writer_guarded(httpd_req_t *req, esp_err_t (*fn)(httpd_req_t *))
{
    if (!safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_HTTP_SYNC)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"another commissioning operation is running\"}");
    }
    esp_err_t err = fn(req);
    (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_HTTP_SYNC);
    return err;
}

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

    /* 2026-09-23: SaftyFW thermo_task.c's live-config mismatch counter,
     * surfaced via the same V3 status frame flags2 byte as borrowed above
     * (SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED). Same "known" gate
     * discipline as borrowed_known -- UNKNOWN on a V1/V2 frame or an
     * unconfirmed V3 peer, never a false "never reasserted". Sticky for the
     * rest of the Pico's boot once true, not a count. */
    bool tc_config_reasserted_known;
    bool tc_config_reasserted;

    /* 2026-09-23: docs/PICO_AUTO_UPDATE.md:64's named gap -- the Pico's
     * own view of which bootloader A/B slot it is currently running,
     * surfaced via the same V3 status frame flags2 byte
     * (SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN/_ACTIVE_SLOT_B). Same
     * "known" gate discipline as tc_config_reasserted_known above. */
    bool pico_active_slot_known;
    bool pico_active_slot_is_b;

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

    /* The operator-entered scale trim (safety_cfg_store_get_ct_cal_trim()).
     * NOT gated on ct_cal_has_value, unlike a_fs/zero_mv above: the trim is
     * always defined -- identity (0.0 A / 1.0x) on a fresh board and after the
     * v1->v2 blob migration -- so build_commissioning_json() emits it
     * unconditionally and the entry field always has a real stored value to
     * round-trip against. */
    float ct_cal_trim_offset_a[SAFETY_CT_CAL_CHANNELS];
    float ct_cal_trim_gain[SAFETY_CT_CAL_CHANNELS];

    /* S8 rate-guard write provenance (docs/audits/s8_auto_calc_design_2026-
     * 09-09.md "Part 3") -- ESP-local, same "always known" reasoning as
     * relay_type/ct_cal above. rate_guard_has_provenance false means "never
     * recorded" (a board that predates this feature, or was just bench-
     * preset) -- the page must render the raw 0x0204 value from params[]
     * with no source label rather than guess. */
    bool rate_guard_has_provenance;
    safety_rate_guard_source_t rate_guard_source;
    float rate_guard_value; /* meaningless unless rate_guard_has_provenance */

    /* Field-by-field diff of the most recent refetch (safety_cfg_store.h's
     * safety_cfg_diff_entry_t) -- 2026-09-10, the decoded config-diff tool
     * docs/audits/safety_config_crc_seq_2026-09-10.md recommended, so an
     * operator sees WHICH named field(s) moved on a config_crc change
     * instead of just the fact that one occurred. Populated straight from
     * safety_cfg_store_get_diff() et al, same "snapshot is a plain struct so
     * this file's JSON builder stays host-testable with no live link" pattern
     * every other field above already follows. */
    size_t diff_count;
    safety_cfg_diff_entry_t diff_entries[SAFETY_CFG_STORE_DIFF_MAX];
    bool diff_truncated;
    uint16_t diff_from_crc;
    uint16_t diff_to_crc;

    /* 2026-09-24 fault-edge instrumentation (CLAUDE.md's "S6a mainFault" MCP
     * tool history, ROADMAP.md): this ESP's OWN fault_sources bitmask and its
     * transition ring/counters -- ESP-local state, never fetched from the
     * Pico, same "always known once s_link exists" reasoning as relay_type
     * above. current_fault_sources_known false only when there is no
     * SafetyLinkClass at all (never started this boot), same convention as
     * every other field this file gates on s_link presence. */
    bool current_fault_sources_known;
    uint32_t current_fault_sources;
    safety_fault_edge_snapshot_t fault_edges;
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
#if CONFIG_KILNCTL_DEV_TOOLS
    APPEND(",\"dev_tools_enabled\":true");
#else
    APPEND(",\"dev_tools_enabled\":false");
#endif
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
    APPEND(",\"tc_config_reasserted_known\":%s", s->tc_config_reasserted_known ? "true" : "false");
    if (s->tc_config_reasserted_known) {
        APPEND(",\"tc_config_reasserted\":%s", s->tc_config_reasserted ? "true" : "false");
    }
    /* docs/PICO_AUTO_UPDATE.md:64's named gap -- "unknown" until a V3
     * peer has confirmed the Pico's own active-slot answer at least once. */
    APPEND(",\"pico_active_slot\":\"%s\"",
           (!s->link_up || !s->pico_active_slot_known) ? "unknown" : (s->pico_active_slot_is_b ? "B" : "A"));
    APPEND(",\"relay_type\":\"%s\"", relay_type_name(s->relay_type));
    APPEND(",\"ct_cal\":[");
    for (size_t ch = 0; ch < SAFETY_CT_CAL_CHANNELS; ch++) {
        APPEND("%s{\"has_value\":%s", ch == 0 ? "" : ",", s->ct_cal_has_value[ch] ? "true" : "false");
        if (s->ct_cal_has_value[ch]) {
            APPEND(",\"a_fs\":%.6g,\"zero_mv\":%.6g,\"source\":\"%s\"", (double)s->ct_cal_a_fs[ch],
                   (double)s->ct_cal_zero_mv[ch], ct_cal_source_name(s->ct_cal_source[ch]));
        }
        /* Outside the has_value guard on purpose -- see the snapshot field's
         * own comment. A trim that appeared only once a channel had been
         * commissioned would leave the entry field with nothing to show on a
         * fresh board, which is exactly the "posts but does not read back"
         * failure this pass exists to avoid. */
        APPEND(",\"trim_offset_a\":%.6g,\"trim_gain\":%.6g", (double)s->ct_cal_trim_offset_a[ch],
               (double)s->ct_cal_trim_gain[ch]);
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
    APPEND("]");

    /* "last_diff": decoded field-by-field diff of the most recent refetch,
     * per safety_cfg_http_snapshot_t's own comment. from_crc/to_crc are 0/0
     * ("nothing refetched this boot") until the first refetch; an operator
     * seeing count==0 after a stale->fresh transition can trust that no
     * *decodable* field actually moved (a seq-only bump, say), rather than
     * wondering whether this endpoint just failed to compute it. */
    APPEND(",\"last_diff\":{\"from_crc\":%u,\"to_crc\":%u,\"truncated\":%s,\"fields\":[",
           (unsigned)s->diff_from_crc, (unsigned)s->diff_to_crc, s->diff_truncated ? "true" : "false");
    for (size_t i = 0; i < s->diff_count; i++) {
        const safety_cfg_diff_entry_t *d = &s->diff_entries[i];
        APPEND("%s{\"name\":\"%s\"", i == 0 ? "" : ",", d->name);
        if (d->old_set) {
            switch (d->type) {
            case KILNLINK_PARAM_TYPE_BOOL: APPEND(",\"old\":%s", d->old_value.bool_val ? "true" : "false"); break;
            case KILNLINK_PARAM_TYPE_U8: APPEND(",\"old\":%u", (unsigned)d->old_value.u8_val); break;
            case KILNLINK_PARAM_TYPE_U16: APPEND(",\"old\":%u", (unsigned)d->old_value.u16_val); break;
            case KILNLINK_PARAM_TYPE_F32:
                if (isfinite(d->old_value.f32_val)) {
                    APPEND(",\"old\":%.6g", (double)d->old_value.f32_val);
                }
                break;
            default: break;
            }
        }
        if (d->new_set) {
            switch (d->type) {
            case KILNLINK_PARAM_TYPE_BOOL: APPEND(",\"new\":%s", d->new_value.bool_val ? "true" : "false"); break;
            case KILNLINK_PARAM_TYPE_U8: APPEND(",\"new\":%u", (unsigned)d->new_value.u8_val); break;
            case KILNLINK_PARAM_TYPE_U16: APPEND(",\"new\":%u", (unsigned)d->new_value.u16_val); break;
            case KILNLINK_PARAM_TYPE_F32:
                if (isfinite(d->new_value.f32_val)) {
                    APPEND(",\"new\":%.6g", (double)d->new_value.f32_val);
                }
                break;
            default: break;
            }
        }
        APPEND("}");
    }
    APPEND("]}");

    /* 2026-09-24 fault-edge instrumentation: this ESP's own fault_sources
     * bitmask (live, not the at-trip snapshot dashboard_http.c's
     * trip_fault_sources reports) plus the transition ring and per-source
     * rising-edge counters -- see safety_link.h's safety_fault_edge_t comment
     * for why this exists (an S6a trip that could not be attributed after the
     * fact once the device log ring had rotated). */
    APPEND(",\"current_fault_sources_known\":%s", s->current_fault_sources_known ? "true" : "false");
    if (s->current_fault_sources_known) {
        APPEND(",\"current_fault_sources\":%u", (unsigned)s->current_fault_sources);
    }
    APPEND(",\"fault_source_edges\":[");
    for (uint32_t i = 0; i < s->fault_edges.count; i++) {
        const safety_fault_edge_t *e = &s->fault_edges.entries[i];
        APPEND("%s{\"uptime_ms\":%lu", i == 0 ? "" : ",", (unsigned long)e->uptime_ms);
        if (e->unix_time_s != 0u) {
            APPEND(",\"unix_time_s\":%lu", (unsigned long)e->unix_time_s);
        } else {
            APPEND(",\"unix_time_s\":null");
        }
        APPEND(",\"source_mask_before\":%u,\"source_mask_after\":%u",
               (unsigned)e->source_mask_before, (unsigned)e->source_mask_after);
        if (e->first_set_bit != 0xFFu) {
            char name_buf[24];
            APPEND(",\"first_set_source\":\"%s\"",
                   safety_fault_source_short_name(e->first_set_bit, name_buf, sizeof(name_buf)));
        } else {
            APPEND(",\"first_set_source\":null");
        }
        APPEND("}");
    }
    APPEND("]");
    APPEND(",\"fault_source_edge_total_recorded\":%lu", (unsigned long)s->fault_edges.total_recorded);
    APPEND(",\"fault_source_counts\":{");
    for (uint8_t b = 0; b < SAFETY_LINK_FAULT_SRC_BIT_COUNT; b++) {
        char name_buf[24];
        const char *name = safety_fault_source_short_name(b, name_buf, sizeof(name_buf));
        APPEND("%s\"%s\":{\"rising_count\":%u", b == 0 ? "" : ",", name,
               (unsigned)s->fault_edges.counts.rising_count[b]);
        if (s->fault_edges.counts.last_rising_valid[b]) {
            APPEND(",\"last_rising_uptime_ms\":%lu",
                   (unsigned long)s->fault_edges.counts.last_rising_uptime_ms[b]);
        } else {
            APPEND(",\"last_rising_uptime_ms\":null");
        }
        APPEND("}");
    }
    APPEND("}");
    APPEND("}");

#undef APPEND
    return o;
}

/* Fixed upper bound for build_commissioning_json()'s output: a per-param
 * entry is at most ~80 bytes (id+name up to ~24 chars+type+set+value), times
 * SAFETY_CFG_PARAM_COUNT, plus a small fixed header -- generous headroom
 * over the ~57*80 + 128 ~= 4700 bytes a full response actually needs. */
/* 2026-09-24: +2560u is for fault_source_edges (<=16 entries, <=138 bytes
 * each at worst-case field widths) and fault_source_counts (6 entries, <=76
 * bytes each) -- ~2.8 KB at worst, slightly more than 2560 on its own, but
 * the params/diff terms above are sized well past their own worst case:
 * test_safety_cfg_http.c's test_build_json_worst_case_fits() measures a
 * whole worst-case body at 11822 of 13440 bytes and fails if it stops fitting. */
#define SAFETY_CFG_JSON_MAX \
    (SAFETY_CFG_PARAM_COUNT * 128u + SAFETY_CFG_STORE_DIFF_MAX * 128u + 256u + 2560u)

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
            snap.tc_config_reasserted_known = st.tc_config_reasserted_known;
            snap.tc_config_reasserted = st.tc_config_reasserted;
            snap.pico_active_slot_known = st.pico_active_slot_known;
            snap.pico_active_slot_is_b = st.pico_active_slot_is_b;
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

        /* 2026-09-24: ESP-local, not fetched from the peer -- see the
         * snapshot field's own comment. */
        snap.current_fault_sources = safety_link_get_fault_sources(s_link);
        snap.current_fault_sources_known = true;
        (void)safety_link_get_fault_edges(s_link, &snap.fault_edges);
    }
    snap.relay_type = safety_cfg_store_get_safety_relay_type();
    for (size_t ch = 0; ch < SAFETY_CT_CAL_CHANNELS; ch++) {
        snap.ct_cal_has_value[ch] = safety_cfg_store_get_ct_cal_input(
            ch, &snap.ct_cal_a_fs[ch], &snap.ct_cal_zero_mv[ch], &snap.ct_cal_source[ch]);
        /* Pre-seeded with the identity so a false return -- only possible for
         * an out-of-range channel, which this loop cannot produce -- still
         * emits a meaningful trim rather than a zero gain. */
        snap.ct_cal_trim_offset_a[ch] = 0.0f;
        snap.ct_cal_trim_gain[ch] = 1.0f;
        (void)safety_cfg_store_get_ct_cal_trim(ch, &snap.ct_cal_trim_offset_a[ch],
                                                &snap.ct_cal_trim_gain[ch]);
    }
    (void)safety_cfg_store_get_rate_guard_meta(&snap.rate_guard_source, &snap.rate_guard_value,
                                                &snap.rate_guard_has_provenance);
    snap.diff_count = safety_cfg_store_diff_count();
    if (snap.diff_count > SAFETY_CFG_STORE_DIFF_MAX) {
        snap.diff_count = SAFETY_CFG_STORE_DIFF_MAX; /* defensive only -- cannot happen */
    }
    for (size_t i = 0; i < snap.diff_count; i++) {
        (void)safety_cfg_store_get_diff(i, &snap.diff_entries[i]);
    }
    snap.diff_truncated = safety_cfg_store_diff_truncated();
    safety_cfg_store_get_diff_crc_range(&snap.diff_from_crc, &snap.diff_to_crc);
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


/* Per-id entry-range gate for this otherwise generic endpoint.
 *
 * Today it covers the per-channel front-end gain (0x030B-0x030D), which the
 * commissioning page made operator-editable so a clamp can be trimmed against
 * a reference meter. gain is the SLOPE control in the end-to-end conversion
 * (I = [counts * 3.3/(4096*gain) - zero_mv/1000] * A_fs / sqrt(2)) and sits in
 * the denominator of current_presence_is_flowing()'s presence threshold too,
 * so a zero/negative/absurd entry is not a cosmetic mistake.
 *
 * It must fail LOUD rather than be quietly repaired. cs_counts_to_amps()
 * substitutes CS_DEFAULT_GAIN for a stored value <= 0, and the Pico's own
 * range check is finiteness-only (config_params.c, deliberately -- see
 * SAFETY_CT_CAL_GAIN_MIN's comment), so without this gate a nonsensical entry
 * would be accepted, persisted, and then silently not used: the operator would
 * be looking at a number the board is ignoring. Refusing at entry is the only
 * place that mismatch can be prevented.
 *
 * Returns true and fills `msg` when a pair is bad; nothing is applied. */
static bool commissioning_pair_range_problem(const safety_cfg_post_pair_t *pairs, int n,
                                              char *msg, size_t msg_len)
{
    for (int i = 0; i < n; i++) {
        if (pairs[i].param_id < 0x030B || pairs[i].param_id > 0x030D) {
            continue;
        }
        unsigned ch = (unsigned)(pairs[i].param_id - 0x030B);
        char *end = NULL;
        float gain = strtof(pairs[i].value_text, &end);
        if (end == pairs[i].value_text || *end != '\0' || !isfinite(gain)) {
            snprintf(msg, msg_len, "gain[%u] must be a number", ch);
            return true;
        }
        if (gain < SAFETY_CT_CAL_GAIN_MIN || gain > SAFETY_CT_CAL_GAIN_MAX) {
            snprintf(msg, msg_len,
                     "gain[%u] must be between %g and %g (got %g) -- refused, not defaulted",
                     ch, (double)SAFETY_CT_CAL_GAIN_MIN, (double)SAFETY_CT_CAL_GAIN_MAX,
                     (double)gain);
            return true;
        }
    }
    return false;
}

static esp_err_t commissioning_post_locked(httpd_req_t *req);
static esp_err_t commissioning_post_handler(httpd_req_t *req)
{
    return cfg_writer_guarded(req, commissioning_post_locked);
}

/* Runs holding SAFETY_CFG_WRITER_HTTP_SYNC (see cfg_writer_guarded()). */
static esp_err_t commissioning_post_locked(httpd_req_t *req)
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
     * endpoint parses) previously fell through safety_cfg_write_apply_pairs() with n == 0 and
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

    /* Entry-range refusal, before anything is staged to the Pico: a bad pair
     * must not be half-applied alongside good ones in the same body. */
    char range_msg[120];
    if (commissioning_pair_range_problem(pairs, n, range_msg, sizeof(range_msg))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, range_msg);
        return ESP_OK;
    }

    char reason[160];
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_NONE;
    bool ok = safety_cfg_write_apply_pairs(s_link, pairs, n, commit, reason, sizeof(reason), &refusal_class);
    /* 2026-09-15 review (HIGH 2): record an ARMED refusal so the standing-
     * divergence warning (safety_ceiling_sync.c) can name the actual reason
     * a broadened-field mismatch cannot currently be corrected, instead of
     * reporting a bare, actionless mismatch -- see safety_cfg_http_recent_
     * armed_refusal()'s doc comment. */
    if (!ok && refusal_class == SAFETY_CEILING_REFUSAL_ARMED) {
        safety_cfg_store_note_armed_refusal();
    }

    /* S8 rate-guard write provenance -- this generic endpoint is how an
     * operator hand-enters max_rate_c_per_min (0x0204) today, so a
     * successful COMMITTED write of it here is, by definition, a MANUAL
     * write: tag it so the commissioning page never shows a stale
     * "auto-derived" label for a value the operator just typed over it.
     * Only tagged when `commit` actually happened -- safety_cfg_write_apply_pairs() already
     * guarantees confirm_commit_landed() ran in that case, so this is the
     * same "verify before tagging" discipline the auto-apply endpoint uses.
     * A stage-only (commit=false) submission changes nothing on the Pico
     * yet, so it must not touch this record either. */
    /* 2026-09-15 review, HIGH 2 -- DOCUMENTED, NOT FIXED. The review's
     * complaint is that this gate is `ok && commit`, so a submission that
     * REACHED the Pico but was refused never updates the captured Pico half,
     * leaving a standing divergence the operator cannot clear. That is
     * accurate, and it is deliberate. Recapturing on a refusal is not
     * available to us here. Reason 2 below is the one that does the work;
     * reason 1 as originally written was proved FALSE by a later review and
     * is corrected in place rather than deleted, so nobody re-derives it:
     *
     * 1. CORRECTED 2026-09-16 (docs/audits/review_divergence_recapture_
     *    6d8c7194_2026-09-16.md, HIGH2). This slot used to read "Nothing
     *    landed -- apply_pairs_ex() stages every pair first and only then
     *    commits, so a refusal writes NOTHING". As a general statement that
     *    is FALSE. Two paths in safety_cfg_write.c return ok=false AFTER the
     *    Pico has already committed:
     *      - confirm_commit_landed()'s refetch failure -- "the safety
     *        processor accepted the commit but this board could not read the
     *        config back to confirm it -- treating the write as UNCONFIRMED,
     *        not successful"; and
     *      - estop_verification_clear() failing on a committed
     *        SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL, whose own comment states
     *        "the commit DID land".
     *    Both reach this gate with ok=false and commit=true, so the
     *    recapture is skipped while the Pico genuinely changed. What
     *    survives of the old claim is only the narrower version: a STAGING
     *    failure, and a commit the Pico explicitly REJECTED ("values were
     *    staged but NOT written"), really do write nothing.
     *    "committed-but-unconfirmed" is a THIRD state, and this gate does
     *    not distinguish it.
     *
     *    Why the gate is nevertheless still correct -- i.e. why this was a
     *    comment fix and not a code fix. Reason 2 below is independently
     *    sufficient on its own, and the resulting staleness is DETECTED, not
     *    silent: safety_ceiling_sync.c re-derives the whole ESP/Pico
     *    comparison from scratch on every safety_poll_task tick, comparing
     *    the active slot's captured expected fields against safety_cfg_
     *    store's LIVE cache -- a cache safety_cfg_store_maybe_refetch()
     *    refills whenever the Pico's config_crc changes ("the only way this
     *    function ever talks to the Pico is a CRC mismatch", safety_cfg_
     *    store.h). A commit that actually landed moves that CRC, so the next
     *    tick refetches and the standing-divergence latch raises itself. No
     *    explicit latch-raise is owed at this call site, and the fault stays
     *    visible and actionable, which is precisely what reason 2 demands.
     *    There is consequently no behavioural difference to pin with a test
     *    here: the corrected text describes the same execution this gate
     *    already performs.
     *
     *    CONTAINMENT, independently re-confirmed 2026-09-16: this staleness
     *    can never move abs_max_temp_c, so it cannot breach the standing
     *    "the Pico's abs_max_temp_c must ALWAYS equal the ESP's" invariant.
     *    Two independent exclusions, either one sufficient: kiln_cfg_store_
     *    capture_expected_pico_fields() skips SAFETY_PARAM_ID_ABS_MAX_TEMP_C
     *    outright (pinned by test_kiln_cfg_store.c's test_capture_expected_
     *    pico_fields_excludes_abs_max_and_tc_type), and safety_ceiling_
     *    sync.c skips it a SECOND time when folding those fields in.
     *    abs_max_temp_c is instead field 0 of the divergence comparison,
     *    re-derived every tick from zones_config_get_temp_limits() through
     *    safety_ceiling_policy_target_c() on the ESP side and from the
     *    Pico's own live ceiling report on the other -- neither side reads
     *    the captured Pico half at all.
     *
     * 2. Clearing it anyway would launder the fault. The captured Pico half
     *    is the ESP's record of what the Pico is supposed to hold. Rewriting
     *    it from an intent the Pico rejected would make the mismatch
     *    disappear from every surface (the LCD notice, /api/status, the
     *    readiness item) while the Pico still holds the old value --
     *    converting a visible, actionable fault into a silent one. Project
     *    policy is the opposite: "a config divergence is a fault", alarm and
     *    never silently reconcile.
     *
     * The owner constraint that makes this unavoidable rather than merely
     * preferable: the refusal this most often concerns is the ARMED refusal,
     * and the governing rule is "there should never be a way that the pico is
     * not armed". The remedy is therefore an operator action (disarm, retry),
     * which the standing-divergence warning now names explicitly -- see
     * safety_cfg_store_recent_armed_refusal() and its reader in
     * safety_ceiling_sync.c. Do not "fix" this by widening the gate. */
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

        /* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
         * MEDIUM 3 / HIGH 3 race): a committed-and-CONFIRMED (safety_cfg_write_apply_pairs()
         * above already ran confirm_commit_landed()'s readback) Pico safety
         * param write never touched the active kiln-config slot's captured
         * Pico half, so the slot went stale relative to a legitimate live
         * change -- the standing divergence check (safety_ceiling_sync.c)
         * would then compare the slot's now-stale "expected" value against
         * the Pico's new (correct) live value and report a divergence for a
         * perfectly ordinary commissioning write.
         *
         * The original fix called kiln_cfg_store_autosave_from_live() here,
         * discarding its result with (void) -- and that call is racy: a
         * safety_poll_task tick landing in the gap between safety_cfg_write_apply_pairs()'s
         * own confirm-by-readback and this line can latch the SAME
         * divergence this write is meant to clear, which made the ordinary
         * (divergence-gated) autosave path skip the very recapture that
         * would resolve it, silently, with the discarded return value
         * hiding the skip entirely. kiln_cfg_store_recapture_pico_half_
         * confirmed() is the fix: it is for exactly this caller -- one that
         * has JUST confirmed its own push -- and does not gate on the
         * divergence latch at all, so the race window above cannot suppress
         * it. Checked and logged, never discarded: the Pico write itself
         * already landed and confirmed, so a failed recapture here must not
         * fail this HTTP response, but it must not go unnoticed either. */
        char autosave_reason[128];
        if (!kiln_cfg_store_recapture_pico_half_confirmed(autosave_reason, sizeof(autosave_reason))) {
            ESP_LOGW(TAG,
                     "commissioning_post_handler: Pico-half recapture after confirmed push failed: %s",
                     autosave_reason);
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
 * NOT routed through parse_set_param_body()/safety_cfg_write_apply_pairs() above, since this
 * is not a Pico param at all (see safety_cfg_store_get_safety_relay_type()'s
 * doc comment): no safety_link involved, no commit round trip, just a local
 * NVS write. "ssr" (and anything else unrecognised) is REJECTED with 400 --
 * the safety relay never offers ssr, and safety_cfg_store_set_safety_relay_
 * type() enforces the same rule as its own second line of defense, but the
 * point of checking here too is to give the operator a specific 400 instead
 * of a generic "nothing happened". */
#define SAFETY_RELAY_TYPE_BODY_MAX 64

static esp_err_t relay_type_post_locked(httpd_req_t *req);
static esp_err_t relay_type_post_handler(httpd_req_t *req)
{
    return cfg_writer_guarded(req, relay_type_post_locked);
}

/* Runs holding SAFETY_CFG_WRITER_HTTP_SYNC (see cfg_writer_guarded()). */
static esp_err_t relay_type_post_locked(httpd_req_t *req)
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
 * k_ct_v_per_a[ch]/zero_counts[ch] through the SAME generic safety_cfg_write_apply_pairs()/
 * confirm_commit_landed() path every other field on this page already uses
 * -- no separate write/verify story for these two fields. Body:
 * "ch=<0-2>&a_fs=<v>&zero_mv=<v>&commit=1"; commit is optional the same way
 * the generic endpoint's is (stage-only submissions are legitimate). Always
 * writes with source=MANUAL -- this endpoint IS manual entry by an operator;
 * the sweep and auto-zero paths call safety_cfg_store_set_ct_cal_input()
 * directly with their own source, never through this HTTP surface. */
#define SAFETY_CT_CAL_BODY_MAX 128

static esp_err_t ct_cal_post_locked(httpd_req_t *req);
static esp_err_t ct_cal_post_handler(httpd_req_t *req)
{
    return cfg_writer_guarded(req, ct_cal_post_locked);
}

/* Runs holding SAFETY_CFG_WRITER_HTTP_SYNC (see cfg_writer_guarded()). */
static esp_err_t ct_cal_post_locked(httpd_req_t *req)
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
     * (source=MANUAL) BEFORE safety_cfg_write_apply_pairs() ran. If the Pico commit then
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
    bool ok = safety_cfg_write_apply_pairs(s_link, pairs, 2, commit, reason, sizeof(reason), NULL);

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
/* POST /api/safety/commissioning/ct_trim                                 */
/* ---------------------------------------------------------------------- */

/* docs/CT_ATTRIBUTION_VERIFICATION.md, owner decision 3: the operator
 * enters the clamp ratio (A_fs, the handler above) and, separately, the
 * offset/gain trim that corrects what the board actually reads against a
 * reference meter. Body: "ch=<0-2>&trim_offset_a=<v>&trim_gain=<v>".
 *
 * WHY THIS IS NOT PART OF ct_cal_post_handler(), AND WHY IT STAGES NOTHING TO
 * THE PICO. The trim is a THIRD quantity, distinct from A_fs (the clamp's
 * nameplate claim) and from gain (the board's fixed front-end divider, params
 * 0x030B-0x030D). It is applied ESP-side, by the sweep task's consumers, and
 * has no wire parameter -- so there are no pairs to stage and no commit to
 * confirm. Folding it into ct_cal's body would have made two of the four
 * fields in that request silently skip the Pico round-trip the other two
 * depend on.
 *
 * Range refusal, not clamping, matches safety_cfg_store_set_ct_cal_trim()'s
 * own contract: a trim quietly clamped to a bound the operator did not ask
 * for would read back as a value they never entered. A changed trim also
 * invalidates any stored CT attribution verdict, because ct_verify_fingerprint()
 * already hashes both fields -- that happens on the next read of the verdict,
 * with nothing to do here. */
#define SAFETY_CT_TRIM_BODY_MAX 128

static esp_err_t ct_trim_post_locked(httpd_req_t *req);
static esp_err_t ct_trim_post_handler(httpd_req_t *req)
{
    return cfg_writer_guarded(req, ct_trim_post_locked);
}

/* Runs holding SAFETY_CFG_WRITER_HTTP_SYNC (see cfg_writer_guarded()). */
static esp_err_t ct_trim_post_locked(httpd_req_t *req)
{
    char body[SAFETY_CT_TRIM_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized body");
        return ESP_OK;
    }

    char ch_text[8] = {0}, off_text[24] = {0}, gain_text[24] = {0};
    if (http_form_find_field(body, "ch", ch_text, sizeof(ch_text)) < 0 ||
        http_form_find_field(body, "trim_offset_a", off_text, sizeof(off_text)) < 0 ||
        http_form_find_field(body, "trim_gain", gain_text, sizeof(gain_text)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "expected ch=<0-2>&trim_offset_a=<v>&trim_gain=<v>");
        return ESP_OK;
    }

    char *end = NULL;
    long ch_l = strtol(ch_text, &end, 10);
    if (end == ch_text || *end != '\0' || ch_l < 0 || ch_l >= (long)SAFETY_CT_CAL_CHANNELS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch must be 0, 1, or 2");
        return ESP_OK;
    }
    size_t ch = (size_t)ch_l;

    float trim_offset_a = strtof(off_text, &end);
    if (end == off_text || *end != '\0' || !isfinite(trim_offset_a)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid trim_offset_a value");
        return ESP_OK;
    }
    if (trim_offset_a < SAFETY_CT_CAL_TRIM_OFFSET_A_MIN ||
        trim_offset_a > SAFETY_CT_CAL_TRIM_OFFSET_A_MAX) {
        char msg[96];
        snprintf(msg, sizeof(msg), "trim_offset_a must be between %g and %g A",
                 (double)SAFETY_CT_CAL_TRIM_OFFSET_A_MIN, (double)SAFETY_CT_CAL_TRIM_OFFSET_A_MAX);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    float trim_gain = strtof(gain_text, &end);
    if (end == gain_text || *end != '\0' || !isfinite(trim_gain)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid trim_gain value");
        return ESP_OK;
    }
    if (trim_gain < SAFETY_CT_CAL_TRIM_GAIN_MIN || trim_gain > SAFETY_CT_CAL_TRIM_GAIN_MAX) {
        char msg[96];
        snprintf(msg, sizeof(msg), "trim_gain must be between %g and %g",
                 (double)SAFETY_CT_CAL_TRIM_GAIN_MIN, (double)SAFETY_CT_CAL_TRIM_GAIN_MAX);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    esp_err_t nvs_err = ESP_OK;
    bool ok = safety_cfg_store_set_ct_cal_trim(ch, trim_offset_a, trim_gain, &nvs_err);

    char resp[192];
    int len;
    if (ok && nvs_err == ESP_OK) {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"trim_offset_a\":%.6g,\"trim_gain\":%.6g,\"persisted\":true}",
                        (double)trim_offset_a, (double)trim_gain);
    } else if (ok) {
        /* In RAM now, but it will not survive a reboot -- reported as a
         * failure of PERSISTENCE, never as a plain success. */
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"trim_offset_a\":%.6g,\"trim_gain\":%.6g,\"persisted\":false,"
                        "\"err\":\"%s\"}",
                        (double)trim_offset_a, (double)trim_gain, esp_err_to_name(nvs_err));
    } else {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":false,\"reason\":\"CT trim rejected by the store\"}");
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
 * ct_cal_post_handler() uses (safety_ct_cal_convert() -> safety_cfg_write_apply_pairs() ->
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
 * docs/HTTP_POST_OWNER_MIGRATION.md slice A1 (2026-09-25): the
 * measurement (~10-12s at CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES/
 * SAFTYFW_PERIOD_CURRENT_TASK_MS, current_task.c) and the commit that
 * follows it used to block the ENTIRE esp_http_server task inline -- a
 * single shared worker, not one thread per connection, so every other HTTP
 * request (dashboard poll, another commissioning action, OTA, etc.) stalled
 * for up to ~15s while this handler ran. ct_auto_zero_post_handler() now
 * does only the fast parts (body parse, preconditions) on httpd_worker and
 * hands the slow tail to ct_auto_zero_job() on its own task via
 * http_async_job_try_start() (http_async_job.h) -- see that header's doc
 * comment for the handoff contract. This is a DIFFERENT hazard from the
 * link_task/current_task blocking problem the Pico's own async-BEGIN/poll
 * design (link_frame.h's own comment) exists to avoid -- that one risks the
 * 30ms link watchdog deadline, not just HTTP responsiveness, and is
 * unchanged by this slice. */
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
 * seam, same reasoning safety_cfg_write_apply_pairs()/parse_set_param_body() are tested this
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

/* Everything ct_auto_zero_post_handler() needs after the handoff --
 * heap-allocated (MALLOC_CAP_INTERNAL, same reasoning as every other
 * heap-scoped struct in this handler: the job's commit path reaches flash
 * via safety_cfg_store_set_ct_cal_input(), and internal DRAM for every
 * allocation in a handler with ANY flash-writing path removes the question
 * rather than depending on this particular one's lifetime not overlapping
 * the write). override_manual is NOT carried through -- it is only ever
 * consulted by ct_auto_zero_check_preconditions(), which already ran to
 * completion before the handoff. */
typedef struct {
    uint8_t channel;
    bool confirm;
    bool has_existing;
    safety_ct_cal_source_t existing_source;
    float existing_a_fs;
    float existing_zero_mv;
} ct_auto_zero_job_ctx_t;

/* The slow tail of ct_auto_zero_post_handler() (docs/HTTP_POST_OWNER_
 * MIGRATION_PLAN.md slice A1) -- runs on its own task via
 * http_async_job_try_start(), not on httpd_worker. Every response body/
 * status this function sends is BYTE FOR BYTE what the pre-A1 inline
 * handler sent from this same point onward -- existing callers
 * (PcTools' ct_auto_zero client, test_safety_cfg_http.c) see no
 * difference. Must not call http_auth_*, cookie or client-IP functions
 * (http_async_job.h's doc comment) -- everything below already ran before
 * the handoff, on the original req, on httpd_worker. Must not call
 * httpd_req_async_handler_complete() itself -- http_async_job.c's run_job()
 * does that once this function returns, on every path, including each of
 * this function's own early returns. */
static void ct_auto_zero_job(httpd_req_t *async_req, void *arg)
{
    ct_auto_zero_job_ctx_t *jc = (ct_auto_zero_job_ctx_t *)arg;
    uint8_t channel = jc->channel;
    bool confirm = jc->confirm;
    bool has_existing = jc->has_existing;
    safety_ct_cal_source_t existing_source = jc->existing_source;
    float existing_a_fs = jc->existing_a_fs;
    float existing_zero_mv = jc->existing_zero_mv;
    free(jc);

    bool have_io = s_hw_io != NULL;

    // --- Measure -------------------------------------------------------------
    esp_err_t begin_err = safety_link_send_ct_auto_zero_begin(s_link, channel);
    if (begin_err != ESP_OK) {
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_sendstr(async_req, "{\"ok\":false,\"reason\":\"could not send the auto-zero request\"}");
        return;
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
    // waited_ms is a real elapsed-time measurement (hal_time_now_us() deltas),
    // not a poll-count proxy -- vTaskDelay() only guarantees AT LEAST the
    // requested delay, so summing SAFETY_CT_AUTO_ZERO_POLL_MS per iteration
    // (the pre-2026-09-25 approach) under-counts real elapsed time whenever a
    // poll is delayed by scheduling, and ct_auto_zero_check_postconditions()'s
    // 5000+waited_ms floor is only as honest as this number (2026-09-25
    // fix-then-push review).
    uint64_t start_us = hal_time_now_us();
    uint32_t waited_ms = 0;
    bool done = false;
    bool stale_done = false;
    bool observed_in_progress = false;
    while (waited_ms < SAFETY_CT_AUTO_ZERO_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(SAFETY_CT_AUTO_ZERO_POLL_MS));
        waited_ms = (uint32_t)((hal_time_now_us() - start_us) / 1000);
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
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_sendstr(async_req, "{\"ok\":false,\"reason\":\"measurement did not start -- the Pico "
                                        "still reports a stale result from an earlier request; check the "
                                        "safety link\"}");
        return;
    }
    if (!done) {
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_sendstr(async_req, "{\"ok\":false,\"reason\":\"measurement did not complete in time -- "
                                        "check the Pico link\"}");
        return;
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
            httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return;
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
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_send(async_req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
        return;
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
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_send(async_req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
        return;
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
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_send(async_req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
        return;
    }

    // --- Re-check the CT-cal snapshot itself, not just the operational
    // preconditions above: the ~10-15s measurement window is long enough for
    // another writer (ct_cal_post_handler(), a backup restore, etc) to have
    // stored a NEW cal for this same channel since jc->existing_* was
    // snapshotted before the handoff. http_async_job_busy() checks added
    // elsewhere refuse a NEW writer from starting while this job is running,
    // but they cannot undo one that already committed in the window between
    // this request's own precondition check and this point. Refuse rather
    // than overwrite a value this request never actually observed -- most
    // importantly, never silently clobber a MANUAL cal made in that window
    // with this stale auto-zero snapshot's numbers (2026-09-25 fix-then-push
    // review). ---
    {
        float fresh_a_fs = 0.0f, fresh_zero_mv = 0.0f;
        safety_ct_cal_source_t fresh_source = SAFETY_CT_CAL_SOURCE_MANUAL; /* overwritten below if fresh_has */
        bool fresh_has = safety_cfg_store_get_ct_cal_input(channel, &fresh_a_fs, &fresh_zero_mv, &fresh_source);
        bool changed = (fresh_has != has_existing) ||
                       (fresh_has && (fresh_source != existing_source || fresh_a_fs != existing_a_fs));
        if (changed) {
            httpd_resp_set_type(async_req, "application/json");
            httpd_resp_sendstr(async_req, "{\"ok\":false,\"reason\":\"CT calibration for this channel "
                                            "changed while this measurement was running -- refusing to "
                                            "commit a stale snapshot; re-run auto-zero\"}");
            return;
        }
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
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_sendstr(async_req, "{\"ok\":false,\"reason\":\"measured value rejected by conversion "
                                        "(out of range)\"}");
        return;
    }

    static const uint16_t K_CT_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0308, 0x0309, 0x030A };
    static const uint16_t ZERO_COUNTS_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x0302, 0x0303, 0x0304 };
    safety_cfg_post_pair_t pairs[2];
    pairs[0].param_id = K_CT_IDS[channel];
    snprintf(pairs[0].value_text, sizeof(pairs[0].value_text), "%.9g", (double)k_ct_v_per_a);
    pairs[1].param_id = ZERO_COUNTS_IDS[channel];
    snprintf(pairs[1].value_text, sizeof(pairs[1].value_text), "%u", (unsigned)zero_counts);

    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(s_link, pairs, 2, true /* always commit on confirm */, reason, sizeof(reason), NULL);

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
    httpd_resp_set_type(async_req, "application/json");
    httpd_resp_send(async_req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
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

    // --- Hand off the measurement + commit to their own task -- everything
    // above this point is fast (a handful of quick reads/a link status
    // fetch, no blocking wait), so it stays on httpd_worker; everything
    // ct_auto_zero_job() does from here is the ~10-15s slow tail this slice
    // moves off it. See http_async_job.h's doc comment for the handoff
    // contract and ct_auto_zero_job()'s own comment for why the response
    // bodies/status codes are unchanged. ---
    ct_auto_zero_job_ctx_t *jc = heap_caps_malloc(sizeof(*jc), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!jc) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    jc->channel = channel;
    jc->confirm = confirm;
    jc->has_existing = has_existing;
    jc->existing_source = existing_source;
    jc->existing_a_fs = existing_a_fs;
    jc->existing_zero_mv = existing_zero_mv;

    // 2026-09-25 fix-then-push review: 4096 B undercounted the real depth.
    // Resolved from ct_auto_zero_job down through safety_cfg_write_apply_pairs
    // -> apply_pairs_ex -> safety_cfg_store_refetch_locked ->
    // safety_link_get_config_page (a 1024 B frame) -> uart_protocol_send_
    // broadcast -> frame_and_send: 3280 B. Add http_async_job_task's own
    // 32 B trampoline plus ~300 B of FreeRTOS/toolchain overhead this static
    // walk does not carry (~3612 B), and an ESP_LOG call through the
    // uart_log_vprintf hook anywhere on this path adds roughly another
    // 1344 B on top of that (esp_log_write ~128 B + uart_log_vprintf ~736 B
    // + vsnprintf ~480 B) -- comfortably over the old 4096 B budget. 6144 B
    // restores real headroom; check_all_task_stack_budgets.py's http_async_job
    // row now walks ct_auto_zero_job as an extra_root and grades against a
    // ceiling derived from this real depth, not a borrowed one.
    http_async_job_start_result_t start_result =
        http_async_job_try_start(req, "http_async_job", 10240, ct_auto_zero_job, jc);
    if (start_result == HTTP_ASYNC_JOB_STARTED) {
        return ESP_OK;
    }
    free(jc);
    httpd_resp_set_type(req, "application/json");
    if (start_result == HTTP_ASYNC_JOB_BUSY) {
        // Refused -- another async job (this route, or a future A2/A3/A4
        // user of the same helper) is already running. req is untouched by
        // http_async_job_try_start() in every refusal case, so responding on
        // it synchronously here is safe. This is a NEW refusal reply this
        // route did not send before A1 -- a concurrent second POST to this
        // route now gets it instead of both requests racing inline.
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"another commissioning operation is running\"}");
    }
    // HTTP_ASYNC_JOB_RESOURCE_FAILURE: the async handoff itself failed
    // (httpd_req_async_handler_begin()) or the job task could not be
    // created -- an out-of-memory-shaped failure, not contention. Matches
    // this handler's existing OOM replies elsewhere (e.g. the jc allocation
    // just above) rather than being misreported as "another operation
    // running".
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
}

/* ---------------------------------------------------------------------- */
/* POST /api/safety/commissioning/bench_preset                            */
/* ---------------------------------------------------------------------- */

/* Dev-tools-only: this table and its handler are only ever registered
 * behind CONFIG_KILNCTL_DEV_TOOLS (see the #if around bench_uri's
 * registration below). Guarding the definitions here too, not just the
 * registration, keeps a release build (CONFIG_KILNCTL_DEV_TOOLS unset) from
 * emitting an unused-function/unused-variable warning for code that build
 * can never call. */
#if CONFIG_KILNCTL_DEV_TOOLS

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

/* The slow tail of bench_preset_post_handler() (docs/HTTP_POST_OWNER_
 * MIGRATION_PLAN.md slice A2) -- runs on its own task via
 * http_async_job_try_start(), not on httpd_worker: 32 SET_PARAM calls plus
 * one COMMIT_CONFIG, each SET_PARAM taking xact_lock with a 5000 ms timeout
 * (SAFETY_XACT_LOCK_TIMEOUT_MS), so a slow link's worst case is tens of
 * seconds. Every response body/status this function sends is BYTE FOR BYTE
 * what the pre-A2 inline handler sent from this same point onward -- no
 * caller (the commissioning page's bench-preset button, PcTools) sees a
 * difference. arg is unused (this job takes no request-derived input beyond
 * the fixed SAFETY_CFG_BENCH_PRESET table) -- NULL is passed at the call
 * site. Must not call http_auth_*, cookie or client-IP functions
 * (http_async_job.h's doc comment); must not call
 * httpd_req_async_handler_complete() itself -- http_async_job.c's run_job()
 * does that once this function returns, on every path. */
static void bench_preset_job(httpd_req_t *async_req, void *arg)
{
    (void)arg;
    for (size_t i = 0; i < SAFETY_CFG_BENCH_PRESET_COUNT; i++) {
        esp_err_t err = safety_link_send_set_param(s_link, SAFETY_CFG_BENCH_PRESET[i].id,
                                                    SAFETY_CFG_BENCH_PRESET[i].type,
                                                    SAFETY_CFG_BENCH_PRESET[i].value);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bench_preset: staging id 0x%04X failed: %s",
                     (unsigned)SAFETY_CFG_BENCH_PRESET[i].id, esp_err_to_name(err));
            httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "communication with the safety processor failed mid-preset");
            return;
        }
    }
    uint16_t reject_param_id = 0;
    uint8_t reject_reason = 0;
    bool rejected = false;
    esp_err_t commit_err = safety_link_send_commit_config(s_link, &reject_param_id, &reject_reason, &rejected);
    if (commit_err != ESP_OK) {
        ESP_LOGW(TAG, "bench_preset: commit failed: %s", esp_err_to_name(commit_err));
        httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             "test preset staged but the safety processor did not acknowledge the commit");
        return;
    }
    if (rejected) {
        /* Not expected in practice (the preset's own values are chosen to
         * pass CONFIG_REFERENCE.md's range/contradiction rules -- see this
         * function's own header comment), but report it honestly rather than
         * claiming {"ok":true} for a commit the Pico actually refused. */
        ESP_LOGW(TAG, "bench_preset: commit REJECTED (param_id=0x%04X, reason=%u)", (unsigned)reject_param_id,
                 (unsigned)reject_reason);
        httpd_resp_set_status(async_req, "500 Internal Server Error");
        httpd_resp_set_type(async_req, "application/json");
        httpd_resp_sendstr(async_req, "{\"ok\":false,\"error\":\"test preset staged but the safety "
                                       "processor rejected the commit\"}");
        return;
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
    httpd_resp_sendstr(async_req, "{\"ok\":true}");
}

static esp_err_t bench_preset_post_handler(httpd_req_t *req)
{
    // Refuse while an http_async_job (ct_auto_zero's 10-15s measurement, or
    // this same route's own job) is running: this handler stages 32 params
    // then commits, and a second interleaved SET_PARAM/COMMIT_CONFIG
    // sequence could persist a half-staged preset (2026-09-25 fix-then-push
    // re-review, kept true after A2's own migration onto the same helper --
    // http_async_job_try_start() below already refuses a second admission on
    // its own, but this check keeps the same synchronous, no-allocation
    // refusal path this route already had rather than depending solely on
    // the busy branch of the try_start() result below).
    if (http_async_job_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"another commissioning operation is running\"}");
        return ESP_OK;
    }
    if (!s_link) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "safety link not available on this board");
        return ESP_OK;
    }

    // --- Hand off the 32 SET_PARAM calls + COMMIT_CONFIG to their own task
    // -- everything above this point is fast (a busy check and a NULL check,
    // no blocking wait), so it stays on httpd_worker; bench_preset_job()
    // does the tens-of-seconds-worst-case slow tail this slice moves off it.
    // See http_async_job.h's doc comment for the handoff contract and
    // bench_preset_job()'s own comment for why the response bodies/status
    // codes are unchanged. No request body to carry through (this route
    // takes no parameters), so ctx is NULL. ---
    http_async_job_start_result_t start_result =
        http_async_job_try_start(req, "http_async_job", 10240, bench_preset_job, NULL);
    if (start_result == HTTP_ASYNC_JOB_STARTED) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    if (start_result == HTTP_ASYNC_JOB_BUSY) {
        // Refused -- another async job (ct_auto_zero, or a concurrent second
        // POST to this same route) is already running. req is untouched by
        // http_async_job_try_start() in every refusal case, so responding on
        // it synchronously here is safe. Same reply text this route already
        // sent for the pre-try_start() http_async_job_busy() check above, so
        // a caller sees no difference in which branch produced it.
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"another commissioning operation is running\"}");
    }
    // HTTP_ASYNC_JOB_RESOURCE_FAILURE: the async handoff itself failed
    // (httpd_req_async_handler_begin()) or the job task could not be
    // created -- an out-of-memory-shaped failure, not contention.
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
}

#endif /* CONFIG_KILNCTL_DEV_TOOLS */

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

    /* 2026-09-10 fix (finding C, then hardened by opus review round 2
     * defect B): this used to reimplement zone_coupling_solve.c's
     * coupling_matrix_provenance_ok() rule by hand, and the reimplementation
     * OMITTED the `use_measured_diag_k_dc` gate the real function applies
     * first -- so a board with `s_coupling_use_measured_diag_k_dc` false and
     * measured off-diagonals populated would have had the control path
     * refuse the matrix while this safety estimator accepted it anyway,
     * backwards from the stated principle that a safety threshold must
     * trust the data at least as little as feedforward, never less. Now
     * calls the SAME shared zone_coupling_matrix_provenance_ok() (exposed
     * from zone_coupling_solve.h) with the SAME shared
     * zone_coupling_use_measured_diag_k_dc() flag the control path uses --
     * there is no second copy of this rule left to drift.
     * board_coupling_provenance_ok is computed board-wide (a property of the
     * whole matrix, same as the control path) and applied to every zone
     * below -- see s8_rate_guard_estimate.h's PROVENANCE section. */
    uint8_t members[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < thermo_count; i++) {
        members[i] = i;
    }
    bool board_coupling_provenance_ok =
        zone_coupling_matrix_provenance_ok(members, thermo_count, zone_coupling_use_measured_diag_k_dc());

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
         * failure (uncommissioned coupling) leaves the sum at 0.0f. 2026-
         * 09-10 fix (finding D): this comment used to claim that a zeroed
         * sum reproduced "the previous own-zone-only basis" as a floor. It
         * did not -- the margin also dropped 2.0x -> 1.3x in the same
         * change, so a coupling-less board's threshold silently loosened by
         * 1.54x, which is a strictly worse basis, not the same one. This is
         * now actually true: board_coupling_provenance_ok (computed above
         * this loop) makes s8_rate_guard_estimate() apply
         * S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED (2.0x, matching the
         * pre-coupling value) whenever coupling is genuinely absent OR its
         * provenance is unproven -- see coupling_provenance_ok below and
         * s8_rate_guard_estimate.h's PROVENANCE section. */
        /* 2026-09-10 fix ("also confirmed, smaller"): apply the SAME
         * per-cell filter profile_feasibility.c's effective_k_dc() applies
         * to this identical data (isfinite(row[j]) && row[j] > 0.0f) before
         * summing, rather than trusting the raw row and only checking the
         * SUM's sign afterward. Without this, a single negative cell could
         * silently understate the basis without ever going negative overall
         * (only a NaN cell was previously caught, via s8_rate_guard_
         * estimate.c's isfinite() check on the summed total). */
        float coupling_row[MAX31856_CHANNEL_COUNT] = {0};
        float coupling_sum = 0.0f;
        if (zones_config_get_coupling(i, coupling_row)) {
            for (uint8_t j = 0; j < thermo_count && j < MAX31856_CHANNEL_COUNT; j++) {
                if (j == i) {
                    continue;
                }
                if (!isfinite(coupling_row[j]) || coupling_row[j] <= 0.0f) {
                    continue;
                }
                coupling_sum += coupling_row[j];
            }
        }
        zones[i].coupling_gain_sum_c_per_duty = coupling_sum;
        zones[i].coupling_provenance_ok = board_coupling_provenance_ok;
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
                                     s8_rate_guard_auto_decision_t *out_decision,
                                     s8_rate_guard_estimate_reason_t *out_reason, char *err_out,
                                     size_t err_cap)
{
    s8_rate_guard_estimate_reason_t reason = rate_guard_gather_and_estimate(out_candidate);
    /* OK and both CLAMPED_* variants all produce a usable *out_candidate --
     * see s8_rate_guard_estimate.h's enum comment. Only NO_DATA means no
     * candidate was produced at all. 2026-09-10 fix (finding E): the
     * CLAMPED distinction previously stopped here -- neither JSON handler
     * below actually surfaced it, despite this header's and this file's own
     * comments claiming an operator could tell "from the plant" apart from
     * "from a bound." *out_reason now carries it out to the callers that do. */
    *out_reason = reason;
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

/* "plant" (S8_RATE_GUARD_ESTIMATE_OK), "floor", or "ceiling" -- see finding
 * E: this is the string the GET/POST JSON now actually carries, closing the
 * gap between this module's doc comments and what an operator could
 * previously observe. */
static const char *rate_guard_clamp_label(s8_rate_guard_estimate_reason_t reason)
{
    switch (reason) {
    case S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR:
        return "floor";
    case S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_CEILING:
        return "ceiling";
    default:
        return "plant";
    }
}

/* GET: compute-and-report only, never writes anything -- safe to poll from
 * the commissioning page on every load, same as the rest of this endpoint's
 * GET side. */
static esp_err_t rate_guard_auto_get_handler(httpd_req_t *req)
{
    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision = S8_RATE_GUARD_AUTO_APPLY;
    s8_rate_guard_estimate_reason_t reason = S8_RATE_GUARD_ESTIMATE_NO_DATA;
    char err[160];
    char resp[288];
    int len;
    if (!rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &reason, err, sizeof(err))) {
        len = snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", err);
    } else {
        len = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"candidate_c_per_min\":%.6g,\"current_set\":%s,"
                        "\"current_c_per_min\":%.6g,\"would_loosen\":%s,\"candidate_source\":\"%s\"}",
                        (double)candidate, current_is_set ? "true" : "false", (double)current,
                        decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY ? "true" : "false",
                        rate_guard_clamp_label(reason));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* POST: body is either empty (act on the tighten/apply policy only) or
 * "confirm=1" (operator has reviewed a loosening suggestion from a prior GET
 * and explicitly wants it applied anyway -- the ONLY way a loosening ever
 * gets written by this endpoint). Every write goes through safety_cfg_write_apply_pairs()
 * (commit=true), which forces confirm_commit_landed()'s live read-back --
 * same "never trust SET_PARAM/COMMIT_CONFIG's own ok" discipline as every
 * other write on this page -- before this handler tags the provenance
 * record or reports success. */
static esp_err_t rate_guard_auto_post_locked(httpd_req_t *req);
static esp_err_t rate_guard_auto_post_handler(httpd_req_t *req)
{
    return cfg_writer_guarded(req, rate_guard_auto_post_locked);
}

/* Runs holding SAFETY_CFG_WRITER_HTTP_SYNC (see cfg_writer_guarded()). */
static esp_err_t rate_guard_auto_post_locked(httpd_req_t *req)
{
    bool confirm = false;
    if (req->content_len > 0) {
        char body[32];
        if (!read_body(req, body, sizeof(body))) {
            /* Oversize or unreadable body: never silently treat as confirm=false
             * (the caller meant something we could not read). */
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or read failed");
            return ESP_OK;
        }
        char value[4] = {0};
        confirm = http_form_find_field(body, "confirm", value, sizeof(value)) >= 0 &&
                  strcmp(value, "1") == 0;
    }

    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision = S8_RATE_GUARD_AUTO_APPLY;
    s8_rate_guard_estimate_reason_t clamp_reason = S8_RATE_GUARD_ESTIMATE_NO_DATA;
    char err[160];
    char resp[320];
    int len;
    if (!rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &clamp_reason, err,
                                  sizeof(err))) {
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
    bool ok = safety_cfg_write_apply_pairs(s_link, &pair, 1, true /* always commit -- this endpoint has no stage-only mode */,
                           reason, sizeof(reason), NULL);
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

    /* safety_cfg_write_apply_pairs() already forced a live read-back that confirms 0x0204
     * now reads `candidate` on the Pico -- only NOW is it correct to tag
     * this record AUTO. A failed persist here (out_nvs_err) costs a UI
     * label on the next boot, never the safety value itself, so it does not
     * change this response. */
    esp_err_t nvs_err = ESP_OK;
    safety_cfg_store_set_rate_guard_meta(SAFETY_RATE_GUARD_SOURCE_AUTO, candidate, &nvs_err);

    len = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"applied\":true,\"value_c_per_min\":%.6g,\"was_loosen\":%s,"
                    "\"candidate_source\":\"%s\"}",
                    (double)candidate, (decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY) ? "true" : "false",
                    rate_guard_clamp_label(clamp_reason));
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
#if CONFIG_KILNCTL_DEV_TOOLS
    /* Dev-tools-only: applies a fixed set of test values, meant for a
     * developer bringing up a board, not for an operator commissioning a
     * real kiln. Gated out of release builds entirely -- see
     * CONFIG_KILNCTL_DEV_TOOLS in App/Kconfig.projbuild. */
    static const httpd_uri_t bench_uri = {
        .uri = "/api/safety/commissioning/bench_preset", .method = HTTP_POST,
        .handler = bench_preset_post_handler,
    };
#endif
    static const httpd_uri_t relay_type_uri = {
        .uri = "/api/safety/commissioning/relay_type", .method = HTTP_POST,
        .handler = relay_type_post_handler,
    };
    static const httpd_uri_t ct_cal_uri = {
        .uri = "/api/safety/commissioning/ct_cal", .method = HTTP_POST,
        .handler = ct_cal_post_handler,
    };
    static const httpd_uri_t ct_trim_uri = {
        .uri = "/api/safety/commissioning/ct_trim", .method = HTTP_POST,
        .handler = ct_trim_post_handler,
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

    const httpd_uri_t *uris[] = { &page_uri,       &get_uri,          &post_uri,
#if CONFIG_KILNCTL_DEV_TOOLS
                                   &bench_uri,
#endif
                                   &relay_type_uri, &ct_cal_uri,       &ct_trim_uri,
                                   &ct_auto_zero_uri,
                                   &rate_guard_auto_get_uri, &rate_guard_auto_post_uri };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = kiln_http_register(server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s", uris[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "safety commissioning API up (link_or_null=%p, %u known params)", (void *)link_or_null,
             (unsigned)safety_cfg_store_param_count());
    return ESP_OK;
}
