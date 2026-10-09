// POST /api/zones whole-page-submit handler, split off zones_http_handlers.c
// (2026-09-04, ROADMAP.md M15's 1500-line item) -- see zones_http_internal.h
// for the full split map. MOVE-ONLY: zones_post_handler(), unchanged, still
// declared non-static (as it already was) in zones_http_internal.h. Its
// per-zone field parser now lives in zones_http_post_parse.c as
// zones_http_parse_zone_fields() -- see that file's own header comment.

#include "zones_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "persist_scratch.h" /* persist_scratch_alloc() -- zone free/restore copies */
#include "http_form.h"
#include "ota_http.h" /* ota_http_check_interlocks() -- the shared "not while firing" gate */
#include "ota_interlock.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() -- see the system_mode_gate check below */
#include "safety_cfg_writer_guard.h" /* HTTP_SYNC claim around the ceiling-raise body */
#include "safety_ceiling_sync.h" /* owner request 2026-09-10 -- Pico abs_max_temp_c tracks the zone max */
#include "system_mode_gate.h" /* SYS_ACTION_WRITE_ZONES_CONFIG -- owner decision Q2, 2026-09-25 */
#include "system_mode_gate_http.h" /* system_mode_gate_http_send_refusal() -- 409, shared sender */
#include "zone_settings_source_chain.h"
#include "cfg_fs_refusal_http.h"

static esp_err_t zones_post_body(httpd_req_t *req);
/* Portable noinline, same guard as autotune_engine.c: cl.exe (the host tests) rejects the GCC syntax. */
#if defined(_MSC_VER)
#define ZONES_POST_NOINLINE
#else
#define ZONES_POST_NOINLINE __attribute__((noinline))
#endif
static ZONES_POST_NOINLINE esp_err_t zones_post_apply(httpd_req_t *req, char *body);

/* Tracks the Pico ceiling DOWN to `live_max_temp_c` (the zone maxima now live in RAM, i.e. the
 * persisted ones), best-effort. Two callers: the post-commit LOWER below, and the lost-update 409,
 * which must undo the raise it already confirmed (HOST_TEST_GAP_AUDIT_2026-10-09.md gap 3: the
 * divergence check needs an exact match, so a raise left behind forces relays off until the next
 * save, and link-up reconcile only ever raises). apply_lower() targets exactly the maximum of the
 * array and only ever lowers, so the Pico never ends up tighter than the live zone maxima.
 * NOINLINE: keeps the reason buffer off zones_post_apply()'s frame (httpd stack blob class). */
static ZONES_POST_NOINLINE void zones_post_track_ceiling_lower(const float *live_max_temp_c, const char *why)
{
    safety_ceiling_sync_result_t ceiling_result = SAFETY_CEILING_SYNC_NONE;
    char ceiling_reason[192];
    ceiling_reason[0] = '\0';
    safety_ceiling_sync_apply_lower(s_hw_safety, live_max_temp_c, MAX31856_CHANNEL_COUNT, &ceiling_result,
                                     ceiling_reason, sizeof(ceiling_reason));
    if (ceiling_result == SAFETY_CEILING_SYNC_LOWER_FAILED) {
        ESP_LOGW(ZONES_HTTP_TAG,
                 "%s: safety processor ceiling not lowered to track the live zone max -- %s -- "
                 "Pico ceiling stays wider than the live max, which is safe, just not tight",
                 why, ceiling_reason);
    } else if (ceiling_result == SAFETY_CEILING_SYNC_LOWERED) {
        ESP_LOGI(ZONES_HTTP_TAG, "%s: safety processor ceiling lowered to track the live zone max", why);
    }
}

/* ---- move_zone_to_aux hook and the zone free/restore it drives ---- */
static zones_move_to_aux_handler_t s_move_to_aux_handler;
static zones_move_to_aux_is_request_t s_move_to_aux_is_request;
static zone_cfg_t *s_aux_saved_zone; /* heap, valid between free_for_aux and restore/discard */

void zones_http_set_move_to_aux_handler(zones_move_to_aux_is_request_t is_request, zones_move_to_aux_handler_t h)
{
    s_move_to_aux_is_request = is_request;
    s_move_to_aux_handler = h;
}

static void aux_saved_zone_drop(void)
{
    free(s_aux_saved_zone);
    s_aux_saved_zone = NULL;
}

int zones_http_zone_free_for_aux(uint8_t zone)
{
    aux_saved_zone_drop();
    if (zone >= s_zones.cfg.thermo_count || zone >= MAX31856_CHANNEL_COUNT ||
        s_zones.cfg.zones[zone].zone_type != (uint8_t)ZONE_TYPE_ON_OFF) {
        return ZONES_AUX_FREE_NOTHING_CHANGED;
    }
    zones_cfg_t *tmp = persist_scratch_alloc(sizeof(*tmp));
    zone_cfg_t *saved = persist_scratch_alloc(sizeof(*saved));
    if (tmp == NULL || saved == NULL) {
        free(tmp);
        free(saved);
        return ZONES_AUX_FREE_NOTHING_CHANGED;
    }
    *tmp = s_zones.cfg;
    zone_cfg_t *z = &tmp->zones[zone];
    z->zone_type = (uint8_t)ZONE_TYPE_HEATER;
    z->relay_mask = 0;
    z->failsafe_state = 0;
    z->hyst_c = 0.0f;
    z->min_on_s = 0;
    z->min_off_s = 0;
    const char *why = NULL;
    if (!zones_config_json_validate(tmp, &why)) {
        ESP_LOGW(ZONES_HTTP_TAG, "zone free for aux refused by validate: %s", why ? why : "?");
        free(tmp);
        free(saved);
        return ZONES_AUX_FREE_NOTHING_CHANGED;
    }
    *saved = s_zones.cfg.zones[zone];
    zones_cfg_lock();
    s_zones.cfg = *tmp;
    s_config_generation++;
    zones_cfg_unlock();
    free(tmp);
    zones_config_push_all_relay_types();
    /* The cfg file is the only save target (NVS dual-write closed): a failed write here is a
     * refusal, because the caller goes on to rewrite profiles against this state. */
    if (nvs_save() != ESP_OK) {
        zones_cfg_lock();
        s_zones.cfg.zones[zone] = *saved;
        s_config_generation++;
        zones_cfg_unlock();
        zones_config_push_all_relay_types();
        /* Report the put-back's own result: a failed second save may leave the cfg file holding
         * the freed zone. */
        bool restored = nvs_save() == ESP_OK;
        free(saved);
        return restored ? ZONES_AUX_FREE_NOTHING_CHANGED : ZONES_AUX_FREE_UNCERTAIN;
    }
    s_aux_saved_zone = saved;
    return ZONES_AUX_FREE_OK;
}

bool zones_http_zone_restore_after_aux(uint8_t zone)
{
    if (s_aux_saved_zone == NULL || zone >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    /* The saved zone is a struct copy from before the conversion; by now the aux store may own its
     * relay. Putting it back unchecked would break the one-owner-per-relay invariant, so check the
     * candidate with the same conflict test the POST path uses and refuse on any overlap. */
    zones_cfg_t *cand = persist_scratch_alloc(sizeof(*cand));
    if (cand == NULL) {
        aux_saved_zone_drop();
        return false;
    }
    *cand = s_zones.cfg;
    cand->zones[zone] = *s_aux_saved_zone;
    if (zones_config_json_aux_conflict_mask(cand) != 0) {
        ESP_LOGE(ZONES_HTTP_TAG, "zone restore refused: relay is owned by an enabled aux output");
        free(cand);
        aux_saved_zone_drop();
        return false;
    }
    free(cand);
    zones_cfg_lock();
    s_zones.cfg.zones[zone] = *s_aux_saved_zone;
    s_config_generation++;
    zones_cfg_unlock();
    zones_config_push_all_relay_types();
    bool ok = nvs_save() == ESP_OK;
    aux_saved_zone_drop();
    return ok;
}

void zones_http_zone_discard_saved_for_aux(void)
{
    aux_saved_zone_drop();
}

esp_err_t zones_post_handler(httpd_req_t *req)
{
    /* Refuse to rewrite zone/relay/guard configuration while a firing is running, the same
     * gate kiln_cfg_http.c's apply and backup_http.c's restore already put in
     * front of the very same zones_cfg_t. Without it, changing relay_mask
     * mid-run moved the firing onto a different physical relay and left the
     * old one wherever it was last commanded, with nobody driving it off --
     * a contact that stays closed because the code that owned it stopped
     * looking at it. See the ota_http_check_interlocks() call below for why
     * that shared gate, not a private profile-is-RUNNING check, is used. */
    /* Owner decision Q2 (docs/SYSTEM_MODE_GATE.md, 2026-09-25,
     * gate-slices-2/4/5 spec): refuse ALL zone/relay/guard config writes --
     * not scoped to which field changed -- while a firing or autotune run is
     * active, PAUSED included, distinct 409 from the OTA interlock's own
     * 428/409 below. relay_authority_heat_run_active() is the same leaf,
     * no-module-lock-held snapshot read kiln_io_owner.c's relay gate already
     * uses (CLAUDE.md's "cache a snapshot outside the lock" note) -- taken
     * here with nothing else held.
     *
     * Review fix, 2026-09-25: this must run BEFORE ota_http_check_interlocks()
     * below, not after -- the interlock already refuses (409 "a profile is
     * running"/"autotune is running", or 428 on link-down) for the exact same
     * facts this gate checks, so with the gate second its own refusal body
     * was dead code: the interlock always answered first and the mode-gate's
     * 409 never reached a caller. Same ordering factory_reset.c already used
     * (its own test asserts !g_probe_interlock_called). */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(ZONES_HTTP_TAG, "POST /api/zones refused by system mode gate: %s", mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    /* ota_http_check_interlocks() is that shared gate rather than a private
     * profile-is-RUNNING check, deliberately: it also covers a hot zone and
     * a commanded heater, and reads the kiln's ACTUAL current state rather
     * than profile_executor's own view (see its doc comment for why that
     * distinction matters). ota_http_req_ack_no_safety() carries the same
     * per-request operator acknowledgement every other caller passes, so a
     * board with no safety processor can still be configured -- saving this
     * page streams nothing over the link, exactly as backup_http's restore
     * argues for itself. */
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    interlock_reason[0] = '\0';
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                            interlock_reason, sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "POST /api/zones refused by interlock: %s", interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    /* docs/HTTP_POST_OWNER_MIGRATION.md A2 review fix (Opus, 2026-09-25) and
     * S7 residual: the rest of this handler raises the Pico ceiling via
     * safety_ceiling_sync_guard_raise() (-> pico_ceiling_writer() ->
     * safety_cfg_write_set_and_confirm_f32()), staging abs_max_temp_c and
     * sending a COMMIT_CONFIG -- the same staged transaction an http_async_job,
     * a current sweep, a kiln config swap or a reconcile tick drives, so it
     * claims the single-flight guard (HTTP_SYNC) for the whole body, released
     * on every path by this wrapper. A refused claim covers an in-flight async
     * job too (it owns ASYNC_JOB), so the old http_async_job_busy() check is
     * subsumed. Ordered after the mode gate and ota_http_check_interlocks():
     * both answer a different question and must still run first. Claimed with
     * no module lock held; the body takes its own locks beneath the leaf flag. */
    if (!safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_HTTP_SYNC)) {
        ESP_LOGW(ZONES_HTTP_TAG, "POST /api/zones refused: another commissioning operation is running");
        return system_mode_gate_http_send_refusal(req, "another commissioning operation is running");
    }
    esp_err_t err = zones_post_body(req);
    (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_HTTP_SYNC);
    return err;
}

/* Everything after the refusal gates, split out so the caller can pair the
 * HTTP_SYNC claim with exactly one release however this returns. */
static esp_err_t zones_post_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > ZONES_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack -- ZONES_BODY_MAX+1 (4097B) was the second-
     * largest transient buffer on the whole httpd_worker task's request
     * path (coordinator review, 2026-08-31), and unlike profile_post_
     * handler's `body` this one genuinely IS read throughout the entire
     * function (http_form_find_field() calls scattered across the whole
     * parse), so it cannot be freed early the way that one's could -- it is
     * freed on EVERY return path below instead, mirroring every other
     * heap-converted handler in this pass. */
    char *body = heap_caps_malloc(ZONES_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(ZONES_HTTP_TAG, "POST /api/zones: malloc(%u) failed for the request body buffer",
                 (unsigned)(ZONES_BODY_MAX + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(ZONES_HTTP_TAG, "zones body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* One-shot "move an ON_OFF zone to an aux output" (docs/SPARE_RELAY_ONOFF_PLAN.md section 10).
     * Not a normal whole-page submit: it is a separate action carried on this route so no new URI
     * slot is needed, dispatched through a hook the aux http module installs. Runs inside this
     * handler's gates and HTTP_SYNC claim. */
    if (s_move_to_aux_handler != NULL && s_move_to_aux_is_request != NULL && s_move_to_aux_is_request(body)) {
        esp_err_t herr = s_move_to_aux_handler(req, body);
        free(body);
        return herr;
    }
    return zones_post_apply(req, body);
}

/* The ordinary whole-page submit, split out of zones_post_body() so the move-to-aux path above never
 * carries this frame (zones_cfg_t tmp alone is ~1.1 KB) on top of its own: noinline keeps the compiler
 * from folding it back, and takes ownership of `body` (frees it on every return). */
static ZONES_POST_NOINLINE esp_err_t zones_post_apply(httpd_req_t *req, char *body)
{
    zones_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    /* Lost-update guard: tmp is assembled from s_zones.cfg (preserved fields) and committed whole at
     * the commit point, after blocking work (Pico ceiling confirm). Any other writer in between bumps
     * s_config_generation; the commit re-checks it and refuses rather than overwrite that write. */
    const uint32_t gen_at_snapshot = s_config_generation;

    if (!zones_config_json_parse_u8_field(body, "thermo_count", 0, MAX31856_CHANNEL_COUNT, &tmp.thermo_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "thermo_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    if (!zones_config_json_parse_u8_field(body, "relay_count", 0, KILN_IO_RELAY_COUNT, &tmp.relay_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    /* Optional -- unlike thermo_count/relay_count, a missing
     * max_simultaneous_relays means "not set" (0, unlimited), not a
     * rejected request, so existing/older callers that don't send it keep
     * working unchanged. Present-but-out-of-range is still rejected. */
    {
        char val[8];
        int len = http_form_find_field(body, "max_simultaneous_relays", val, sizeof(val));
        if (len > 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* *end != '\0' rejects trailing garbage after a valid numeric
             * prefix (e.g. "2X"), same gap as zones_config_json_parse_u8_field()/
             * zones_config_json_parse_float_field() above -- end == val alone lets it through. */
            if (end == val || *end != '\0' || v < 0 || v > KILN_IO_RELAY_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "max_simultaneous_relays out of range");
                free(body);
                return ESP_OK;
            }
            tmp.max_simultaneous_relays = (uint8_t)v;
        }
    }
    /* Optional, same "missing means keep the safe default" convention as
     * max_simultaneous_relays above -- tmp is zeroed, so a caller that
     * never sends this field gets continue_on_zone_trip=0 (abort the whole
     * firing), TODO.md 6A.3's stated default. Only "0" or "1" accepted. */
    {
        char val[4];
        int len = http_form_find_field(body, "continue_on_zone_trip", val, sizeof(val));
        if (len > 0) {
            if (strcmp(val, "1") == 0) {
                tmp.continue_on_zone_trip = 1;
            } else if (strcmp(val, "0") == 0) {
                tmp.continue_on_zone_trip = 0;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "continue_on_zone_trip must be 0 or 1");
                free(body);
                return ESP_OK;
            }
        }
    }

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "make it so that i
     * can have diffrent Safety timings and assign the zones to them"): the
     * named timing profiles this submission defines, parsed BEFORE the
     * per-zone loop below -- each zone's z%u_timingprofile must be bounded
     * against however many profiles THIS submission actually carries, and
     * that count is only known once this loop finishes. A profile slot is
     * "in the submission" iff tp<N>_name is present, checked contiguously
     * from N=0: GET /api/zones always emits a dense 0..count-1
     * timing_profiles array (see zones_get_handler()), so the page always
     * reposts one too, and the first missing tp<N>_name is the end of the
     * list, not a gap to skip past. At least one profile (tp0_name) is
     * required -- an empty timing_profiles[] would leave every zone's
     * z%u_timingprofile with nothing valid to reference, and
     * zones_cfg_t::timing_profile_count must never be 0 in a config this
     * handler commits (see its own comment). */
    for (uint8_t p = 0; p < MAX31856_CHANNEL_COUNT; p++) {
        char probe_key[16];
        char probe[TIMING_PROFILE_NAME_MAX_LEN + 1];
        snprintf(probe_key, sizeof(probe_key), "tp%u_name", p);
        int probe_len = http_form_find_field(body, probe_key, probe, sizeof(probe));
        if (probe_len == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timing profile name too long");
            free(body);
            return ESP_OK;
        }
        if (probe_len < 0) {
            break; /* no tp<p>_name -- this and every following slot is absent from this submission */
        }
        const char *err_reason = "invalid timing profile field";
        if (!zones_config_json_parse_timing_profile_fields(body, p, &tmp.timing_profiles[p], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
        tmp.timing_profile_count = (uint8_t)(p + 1);
    }
    if (tmp.timing_profile_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "at least one timing profile (tp0_name) is required");
        free(body);
        return ESP_OK;
    }

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const char *err_reason = "invalid zone field";
        /* &s_zones.cfg.zones[i]: the LIVE value, for z%u_tctype's
         * omit-means-preserve fallback (see zones_http_parse_zone_fields()'s comment) --
         * tmp itself is zeroed, so tmp.zones[i] can't supply "what this
         * channel is already set to." */
        if (!zones_http_parse_zone_fields(body, i, tmp.thermo_count, tmp.relay_count, tmp.timing_profile_count,
                               &s_zones.cfg.zones[i], &tmp.zones[i], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
    }
    /* zones_http_parse_zone_fields()'s own per-zone chain-walk (above, inside that
     * function) only ever sees ONE zone's link changing at a time -- it
     * checks the new link for zone i against every OTHER zone's LIVE stored
     * value, which is correct for that single write but blind to a
     * whole-page POST that changes SEVERAL zones' links in the same
     * request, none of which is a cycle by itself against the old live
     * config, but which together close one (e.g. z0_settings_source=1 and
     * z1_settings_source=0 in the same POST when neither zone pointed
     * anywhere before). tmp.zones[] now holds every zone's fully-assembled
     * NEW link, so re-walk every zone's chain against THAT -- before the
     * commit point below, so a rejection here still leaves s_zones
     * untouched, same "never partially apply" discipline the rest of this
     * handler follows. Bounded by tmp.thermo_count, same "unused trailing
     * slot" discipline zones_config_json_settings_source_chain_has_cycle()'s own comment
     * explains -- a slot past this submission's own thermo_count was never
     * rendered and never posted to, so it is excluded from this walk
     * entirely rather than treated as a real link. (It does NOT read back at
     * a zero-initialized default: zones_http_parse_zone_fields()'s early return for
     * such a slot does `*z = *current_z`, so tmp.zones[i] carries whatever
     * settings_source is already LIVE and stored for that zone, not 0 --
     * still bounded out of this walk on principle, since that live value was
     * not part of this submission either, but the "reads back as 0" premise
     * would be wrong if repeated as a reason.) */
    for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
        for (uint8_t i = 0; i < tmp.thermo_count && i < MAX31856_CHANNEL_COUNT; i++) {
            if (zones_config_json_settings_source_chain_has_cycle(tmp.zones, group, i, tmp.thermo_count)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "zone settings_source would create an inheritance cycle");
                free(body);
                return ESP_OK;
            }
        }
    }

    /* DEPRECATED as a write path, 2026-09-15 (owner decision, Opus review F3,
     * "the commissioning page owns the type"): this used to accept an
     * operator-submitted safety_tc_type and push it to the Pico (see
     * zones_cfg_t::safety_tc_type's own comment). The Pico's own
     * commissioning page is now the sole writer of its tc_type; this ESP
     * field is read-only from the web form's point of view -- ALWAYS keep
     * the current live value (last value read back from the Pico),
     * regardless of what a submission carries, rather than accepting an
     * operator-typed value that would never actually reach the Pico and
     * would silently desync the displayed value from the real one. */
    tmp.safety_tc_type = s_zones.cfg.safety_tc_type;

    /* The one global v8 override. OPTIONAL, and on omit it keeps the CURRENT
     * live value rather than resetting to 0 -- the same reasoning as
     * safety_tc_type above: this is a whole-page submit, and an older client
     * that predates the field must not silently reset how long a firing
     * tolerates a dead PC link just by saving the zones page. */
    {
        char val[16];
        int len = http_form_find_field(body, "pc_link_abort_silence_ms", val, sizeof(val));
        if (len > 0) {
            if (!zones_config_json_parse_float_field(body, "pc_link_abort_silence_ms", 0.0f,
                                   ZONE_PC_LINK_SILENCE_MS_MAX, &tmp.pc_link_abort_silence_ms)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "pc_link_abort_silence_ms out of range (0 = firmware default)");
                free(body);
                return ESP_OK;
            }
        } else {
            tmp.pc_link_abort_silence_ms = s_zones.cfg.pc_link_abort_silence_ms;
        }
    }

    /* ZONES_CFG_VERSION 16->17: ease_off_window_mult moved per-zone -- see
     * zone_cfg_t::ease_off_window_mult's own comment. Parsed per zone as
     * z%u_easeoffmult inside zones_http_parse_zone_fields()
     * (zones_http_post_parse.c), not here alongside the whole-board globals
     * any more. */

    /* 2026-08-27+1 (owner request: name relays not assigned to any zone).
     * relay<N>_name (N = 1..KILN_IO_RELAY_COUNT, matching relay_mask's wire
     * numbering) is GLOBAL, relay-indexed, not per-zone -- so it follows the
     * same "omitted means keep the current live value" convention this
     * handler already uses for safety_tc_type/pc_link_abort_silence_ms
     * above, NOT the per-zone fields' "omitted means zero" convention
     * (tmp.zones[] is zero-initialized; this isn't). That matters here even
     * more than it does for those: zones_page.html and safety_config_page.html
     * BOTH POST to this same endpoint, and only zones_page.html renders a
     * relay-name input at all (see its renderRelayNames()) -- a save
     * triggered from /settings/safety must not wipe every relay name just
     * because that page has no editable field for them. safety_config_page.html
     * still echoes them anyway (defense in depth, matching its existing
     * safety_tc_type/pc_link_abort_silence_ms echo style), but this
     * omit-preserves convention is what actually GUARANTEES neither page can
     * clobber the other's relay names, independent of whether that echo is
     * ever forgotten in a future edit to either page.
     *
     * Storage is unconditional regardless of the relay's CURRENT zone
     * ownership -- see this file's relay-names section header comment for
     * why a name is kept, not cleared, when its relay becomes zone-owned.
     * Present-but-overlong is still rejected (a real mistake, not a
     * deliberate omission), same as z%u_name's -2 handling in
     * zones_http_parse_zone_fields(). Parsed into a scratch copy of the CURRENT live
     * relay names, not applied to s_relay_names.cfg directly, so a request
     * that gets rejected later in this handler (impossible past this point
     * today, but kept for the same "never partially apply" discipline every
     * other section of this handler follows) has touched nothing. */
    relay_names_cfg_t tmp_relay_names = s_relay_names.cfg;
    for (uint8_t r = 1; r <= KILN_IO_RELAY_COUNT; r++) {
        char rkey[16];
        snprintf(rkey, sizeof(rkey), "relay%u_name", r);
        char rval[RELAY_NAME_MAX_LEN + 1];
        int rlen = http_form_find_field(body, rkey, rval, sizeof(rval));
        if (rlen == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay name too long");
            free(body);
            return ESP_OK;
        }
        if (rlen >= 0) {
            strncpy(tmp_relay_names.names[r - 1], rval, RELAY_NAME_MAX_LEN);
            tmp_relay_names.names[r - 1][RELAY_NAME_MAX_LEN] = '\0';
        }
        /* rlen < 0 (omitted): tmp_relay_names.names[r-1] already holds the
         * current live value, copied above -- left untouched. */

        /* docs/ZONE_GRAPHIC_PLAN.md stage 2: relay<N>_type, the sibling of
         * relay<N>_name, carrying relay_device_type_t as a small integer.
         * Same omitted-means-keep convention and the same scratch-copy
         * discipline as the name above, for the same reason (safety_config_
         * page.html POSTs to this endpoint and renders no such control).
         *
         * An out-of-range or non-numeric value is REFUSED with a 400, never
         * coerced to UNSET or to any other default: UNSET is a meaningful
         * operator-visible state ("nobody has said what this relay drives"),
         * so silently landing there would turn a client bug into a plausible
         * stored answer. 0 (UNSET) submitted deliberately IS accepted. */
        char tkey[16];
        snprintf(tkey, sizeof(tkey), "relay%u_type", r);
        char tval[8];
        int tlen = http_form_find_field(body, tkey, tval, sizeof(tval));
        if (tlen == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay device type out of range");
            free(body);
            return ESP_OK;
        }
        if (tlen > 0) {
            char *tend = NULL;
            long tv = strtol(tval, &tend, 10);
            if (tend == tval || *tend != '\0' || tv < 0 || tv >= (long)RELAY_DEVICE_TYPE_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay device type out of range");
                free(body);
                return ESP_OK;
            }
            tmp_relay_names.types[r - 1] = (uint8_t)tv;
        }
    }

    /* Spare-relay aux conflict (docs/SPARE_RELAY_ONOFF_PLAN.md WP-2): this handler
     * commits `s_zones.cfg = tmp` below WITHOUT calling zones_config_json_validate(),
     * so the aux hook that validate carries never runs here -- the check must be
     * explicit. A zone may not claim a relay an ENABLED aux output owns. 409, the
     * same "refused, nothing changed" status the mode gate uses; tmp is a scratch
     * copy so nothing has been touched yet. Runs against the fully assembled
     * `tmp` (preserved slots included), before any Pico ceiling write. */
    {
        uint8_t aux_conflict = zones_config_json_aux_conflict_mask(&tmp);
        if (aux_conflict != 0) {
            ESP_LOGW(ZONES_HTTP_TAG, "POST /api/zones refused: relay mask 0x%02X is owned by an aux output",
                     (unsigned)aux_conflict);
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "a zone relay_mask claims a relay an aux (spare-relay) output already owns -- "
                                    "disable that aux output first");
            free(body);
            return ESP_OK;
        }
    }

    /* cfg is the only save target: refuse before any Pico write or RAM commit
     * when it is not mounted. */
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        free(body);
        return ESP_OK;
    }

    /* Owner request 2026-09-10 ("if i change the max temp in the web gui it
     * should change it in the pico too."): the Pico's own independent
     * abs_max_temp_c ceiling must never end up TIGHTER than the highest
     * configured zone max_temp_c -- see safety_ceiling_policy.h's header
     * comment for the full invariant and why RAISING requires the Pico to
     * be written and CONFIRMED first, strictly before this handler's own
     * commit point below. `tmp.zones[i].max_temp_c` is the PROPOSED new
     * config -- this must run against `tmp`, not the still-live
     * `s_zones.cfg`, and it must run BEFORE the commit point so a refused
     * Pico raise leaves s_zones completely untouched, same "never partially
     * apply" discipline the rest of this handler already follows.
     *
     * A refusal here is EXPECTED, not exotic, whenever the Pico is in its
     * ordinary standing ARMED state (config_store_decide_write() refuses
     * every config write unconditionally while ARMED, with no per-field
     * carve-out -- see safety_ceiling_policy.h's top comment) -- so this
     * reports a clear, specific, operator-facing reason via HTTP 409
     * rather than a generic 400/500, and never silently drops the raise or
     * requires an undocumented safety-processor reset. */
    {
        float new_max_temp_c[MAX31856_CHANNEL_COUNT];
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            new_max_temp_c[i] = (i < tmp.thermo_count) ? tmp.zones[i].max_temp_c : 0.0f;
        }
        /* 2026-09-10 opus review, httpd stack blob class: ceiling_reason[192] +
         * escaped[224] + resp[300] used to all live on THIS handler's own
         * frame, which already also carries the full `tmp` zone-config struct
         * -- 716 B live simultaneously on the shared 8 KB httpd stack this
         * repo has already panicked from twice. Heap-allocate the trio,
         * following diagnostics_http.c's cfgfs_status_get_handler() pattern.
         * ceiling_reason is the guard's own out-param, needed on the call
         * below regardless of outcome, so the allocation happens before the
         * call, not just in the failure arm; a malloc failure here fails
         * SAFE by refusing the raise (reason_out=NULL is tolerated by
         * safety_ceiling_policy_guard_raise()) rather than risking an
         * unconfirmed raise. */
        typedef struct {
            char ceiling_reason[192];
            char escaped[224];
            char resp[300];
        } ceiling_scratch_t;
        ceiling_scratch_t *cs = persist_scratch_alloc(sizeof(*cs));
        safety_ceiling_sync_result_t ceiling_result;
        bool raise_ok;
        if (!cs) {
            ESP_LOGE(ZONES_HTTP_TAG, "zones_post_handler: malloc(%u) failed -- refusing the ceiling raise",
                     (unsigned)sizeof(*cs));
            raise_ok = safety_ceiling_sync_guard_raise(s_hw_safety, new_max_temp_c, MAX31856_CHANNEL_COUNT,
                                                       &ceiling_result, NULL, 0, NULL);
        } else {
            raise_ok = safety_ceiling_sync_guard_raise(s_hw_safety, new_max_temp_c, MAX31856_CHANNEL_COUNT,
                                                       &ceiling_result, cs->ceiling_reason,
                                                       sizeof(cs->ceiling_reason), NULL);
        }
        if (!raise_ok) {
            ESP_LOGW(ZONES_HTTP_TAG,
                     "POST /api/zones refused: raising the safety processor's ceiling failed/could not be "
                     "confirmed -- zone config left UNCHANGED: %s",
                     cs ? cs->ceiling_reason : "(out of memory -- no detail available)");
            if (cs) {
                size_t o = 0;
                for (const char *c = cs->ceiling_reason; *c && o + 2 < sizeof(cs->escaped); c++) {
                    if (*c == '"' || *c == '\\') {
                        cs->escaped[o++] = '\\';
                    }
                    cs->escaped[o++] = *c;
                }
                cs->escaped[o] = '\0';
                int len = snprintf(cs->resp, sizeof(cs->resp),
                                    "{\"ok\":false,\"error\":\"safety_ceiling_raise_failed\",\"reason\":\"%s\"}",
                                    cs->escaped);
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, cs->resp, len > 0 && (size_t)len < sizeof(cs->resp) ? (size_t)len : strlen(cs->resp));
                free(cs);
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            }
            free(body);
            return ESP_OK;
        }
        free(cs);
        /* ceiling_result is RAISED or NONE here -- either way the Pico's
         * ceiling is now confirmed to be >= the proposed new max, so this
         * commit is safe to apply. A LOWER (if any) is deliberately NOT
         * attempted here -- see this handler's post-commit block below for
         * why that direction goes AFTER, not before. */
    }

    /* Check-and-commit is ONE critical section against every other writer's mutate-and-bump
     * (zones_cfg_lock(), review L2): the compare, the struct copies and the generation bump below
     * cannot interleave with a concurrent setter. The lock is dropped before the 409 reply and held
     * across no producer call -- everything slow (ceiling write, parsing) happened above on `tmp`. */
    zones_cfg_lock();
    if (s_config_generation != gen_at_snapshot) {
        /* The raise above may already have widened the Pico for `tmp`, which is now never
         * committed: snapshot the live maxima under the lock, and lower back to them below. */
        float live_max_temp_c[MAX31856_CHANNEL_COUNT];
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            live_max_temp_c[i] = (i < s_zones.cfg.thermo_count) ? s_zones.cfg.zones[i].max_temp_c : 0.0f;
        }
        zones_cfg_unlock();
        zones_post_track_ceiling_lower(live_max_temp_c, "POST /api/zones lost-update 409");
        ESP_LOGW(ZONES_HTTP_TAG, "POST /api/zones refused: zones config changed concurrently (generation %u -> %u)",
                 (unsigned)gen_at_snapshot, (unsigned)s_config_generation);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"zones_config_changed_concurrently\",\"reason\":\"another writer changed the zones config while this submit was being processed; reload and retry\"}");
        free(body);
        return ESP_OK;
    }
    /* Commit point: every rejection above returned before touching s_zones,
     * so this is the first and only line at which the submission becomes the
     * live config -- and therefore the only place in this handler the
     * generation may advance. A 400'd submission changed nothing and must
     * not make a running profile re-read identical settings (TODO.md
     * 6A.7). */
    s_zones.cfg = tmp;
    s_relay_names.cfg = tmp_relay_names;
    /* A validated, freshly-submitted config is trustworthy the moment it's
     * live in RAM, regardless of whether the NVS write below succeeds --
     * same "applied now either way" convention nvs_save()'s failure handling
     * already uses below. This is the other half of s_zones_config_valid's
     * contract: true after either a real successful load OR a fresh valid
     * save. */
    s_zones_config_valid = true;
    s_config_generation++;
    zones_cfg_unlock();
    /* RELAY_LIFE_BUDGET.md, "on every successful save": this
     * whole-page submit just validated cleanly and is now live in
     * s_zones.cfg (the "successful" part -- a rejected submission returned
     * long before this line and never reaches here), so push every zone's
     * relay_type out to relay_cycles.c now, same "applied now either way"
     * convention as nvs_save()'s own failure handling just below -- an
     * operator-visible relay type change takes effect immediately whether
     * or not the NVS write that would make it survive a reboot succeeds. */
    zones_config_push_all_relay_types();
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "nvs_save failed: %s -- config applied live but will not survive a reboot",
                 esp_err_to_name(err));
        /* Still applied above, but the save failed: reported as an error
         * after the ceiling sync below, never as "ok". */
    }
    esp_err_t names_err = relay_names_save();
    if (names_err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "relay_names_save failed: %s -- names applied live but will not survive a reboot",
                 esp_err_to_name(names_err));
        /* Applied live; the failed save is reported after the ceiling sync. */
    }

    /* LOWERING direction, deliberately AFTER the commit above -- see
     * safety_ceiling_policy.h's top comment for why: applying the ESP's own
     * (now-lower) zone config FIRST can never put the Pico's ceiling below
     * the live max (the Pico's ceiling only ever gets tightened here, never
     * loosened), whereas tightening the Pico BEFORE the ESP commit could
     * transiently -- or, if the ESP write then somehow failed, permanently
     * -- leave the Pico's ceiling BELOW the ESP's still-live higher max.
     * Best-effort only: a failure here (most likely the Pico's ordinary
     * ARMED state, same as the raise path) is logged, never treated as this
     * request's own failure -- the invariant stays satisfied either way
     * (Pico ceiling merely stays wider than strictly necessary until the
     * next opportunity, e.g. after a safety-processor reset). */
    {
        float new_max_temp_c[MAX31856_CHANNEL_COUNT];
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            new_max_temp_c[i] = (i < s_zones.cfg.thermo_count) ? s_zones.cfg.zones[i].max_temp_c : 0.0f;
        }
        zones_post_track_ceiling_lower(new_max_temp_c, "POST /api/zones");
    }

    free(body);
    if (err != ESP_OK || names_err != ESP_OK) {
        return cfg_fs_http_persist_failed(req);
    }
    return httpd_resp_sendstr(req, "ok");
}
