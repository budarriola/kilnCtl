/* Profile executor family -- moved out of dashboard_http.c 2026-09-04
 * (ROADMAP.md M15, the 1500-line rule): GET /api/profile_exec,
 * /api/profile_plan, /api/control, /api/firing_history, /api/history.csv;
 * POST /api/profile_exec/{start,stop,pause,resume,ack_last_run}, POST
 * /api/safety/clear_trip. See dashboard_http_internal.h for the shared
 * s_dash/DASH_TAG seam. dashboard_plan_exec_fields() stays public API
 * (declared in dashboard_http.h) and is unchanged by this split. */

#include "dashboard_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "autotune_engine.h"
#include "danger_mode.h"
#include "dashboard_json.h"
#include "http_form.h"
#include "kiln_io_owner.h"
#include "ota_http.h"
#include "profile_executor.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "readiness_gate.h"
#include "relay_authority.h"
#include "run_state.h"
#include "safety_trip_words.h"
#include "sim_backend.h"
#include "system_mode_gate.h"
#include "unit_pref.h"
#include "watchdog_cfg.h"

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
 * plan-only estimate, every time, not just when a lag is currently visible.
 *
 * Exported (2026-08-30, TODO.md 10.1a shared-backend rule) so ui_page_home.c's
 * LCD progress bar reads the exact same numbers /api/profile_exec's
 * total_planned_s/elapsed_s/remaining_s/remaining_is_estimate report on the
 * web dashboard, instead of a second copy of this switch drifting apart from
 * it. Declared in dashboard_http.h. */
void dashboard_plan_exec_fields(const profile_exec_status_t *st, int64_t *out_total_planned_s,
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

esp_err_t profile_exec_status_get_handler(httpd_req_t *req)
{
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    profile_executor_get_status(st);

    char name_escaped[sizeof(st->profile_name) * 2 + 1];
    json_escape(st->profile_name, name_escaped, sizeof(name_escaped));
    char reason_escaped[sizeof(st->fault_reason) * 2 + 1];
    json_escape(st->fault_reason, reason_escaped, sizeof(reason_escaped));

    int64_t total_planned_s, remaining_s;
    uint32_t elapsed_s;
    bool remaining_is_estimate;
    dashboard_plan_exec_fields(st, &total_planned_s, &elapsed_s, &remaining_s, &remaining_is_estimate);
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

    /* Sized against the real worst case rather than an estimate (Opus
     * review round 3, blocker 1's own re-audit: the PRIOR "320, not 224"
     * pass here computed 320 from ~130 bytes of fixed keys + a 190-byte
     * fully-escaped fault_reason, but that arithmetic undercounted its own
     * fixed-keys estimate -- the real fixed-key total (heat_blocked/
     * heat_blocked_sources included) plus a 190-byte escaped fault_reason
     * is 401B, already over the 320 it shipped at, before this fix's own
     * three new fields (ff_hold_used_matrix/ff_hold_infeasible/
     * ff_membership_change_count, another ~95B) pushed it to 496B worst
     * case. 512 leaves real headroom. PID_EXPANSION_PLAN.md sec 7.2 added
     * two more (ramp_stretch_segment_s/ramp_stretch_total_s, 2x "%.2f" up
     * to 8B each = 16B of values + ~55B of keys = ~71B more), 567B worst
     * case now -- still real headroom against the 960-byte fixed part
     * below. The 960-byte fixed part covers the
     * run-level line (its own escaped reason, plus the four duration-model
     * fields added for the profile-plan contract -- at most ~48 bytes more)
     * plus the "last_run" object at ITS worst case. The httpd task runs on
     * an 8192-byte stack (wifi_provision_http.c) -- but httpd_worker was
     * measured at 64 bytes free of 8192 (0.8% headroom) after a live
     * 30-minute profile run with this buffer ON the stack, right next to
     * every other handler's own locals sharing that same task/stack across
     * calls (autotune_matrix_get_handler's ~1.2KB, zones_get_handler's
     * ~5.6KB, etc. -- see this file's own history for the running audit).
     * HEAP now, not stack, same fix and same reasoning as GET /api/status's
     * json above: freed on every return path, diagnosable 500 on malloc
     * failure instead of a near-miss stack overflow that would corrupt
     * memory on a board that commands kiln heaters. See
     * test_dashboard_json.c's own worst-case render for both this and
     * /api/control's buffer, so the next field added to either JSON shape
     * gets caught here instead of shipping silently truncated again.
     *
     * PID_EXPANSION_PLAN.md sec 7.3, DEFECT 4 fix: added one more run-level
     * field to the fixed part above, "ramp_dwell_credit_applied_s" -- the
     * one number saying how many seconds THIS run's dwell(s) were shortened
     * by ramp-assist dwell credit (s_exec_state_t.dwell_credit_applied_s,
     * profile_executor_internal.h; mirrored onto profile_exec_status_t.
     * ramp_dwell_credit_applied_s at profile_executor_status.c:257, which
     * this handler was reading into `st` but never serializing -- Opus
     * review of commit 5312e14, "the commit message claims it is reported
     * run-wide on /api/profile_exec; it is not"). Key+punctuation
     * (`"ramp_dwell_credit_applied_s":` then a trailing comma) is 32B
     * (28-char key + 2 quotes + colon + comma); value is "%.2f" at up to 8B
     * ("-1234.56", same worst-case width convention as every other float in
     * this format string) = 40B more, bringing the fixed part's worst case
     * to 567+40 = 607B, plus the "last_run" object at ITS worst case, per the
     * paragraph above.
     *
     * docs/audits/profile_executor_panic_2026-09-24.md: added two more run-
     * level fields, "mode_state_fault_latched" (bool, ~35B worst case) and
     * "mode_state_violation_count" (uint32, ~45B worst case at
     * "4294967295") -- ~80B more, 687B worst case now, still real headroom
     * against this 960-byte fixed allowance.
     *
     * 2026-09-25 (HP-02 latch leak): added "zone_blocked_mask" (uint8 as
     * %u, ~23B worst case: 17-char key + quotes/colon/comma + "255") --
     * relay_authority's own latched per-zone block mask, read-only, so a
     * stale latch (relay_authority.h's relay_authority_zone_latched_
     * blocked() doc comment) is observable from this route without needing
     * a new one (route count already 163/170). Not the first place this
     * value is reported -- dashboard_status_http.c:654 already serializes
     * the same value under the same key on GET /api/status; this is a
     * convenience copy for a caller already polling profile_exec. ~710B
     * worst case now, still real headroom. */
    char *json = heap_caps_malloc(DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/profile_exec: malloc(%u) failed for the response buffer",
                 (unsigned)DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        free(st);
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    int n = snprintf(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE,
        "{\"state\":\"%s\",\"profile_id\":%u,\"profile_name\":\"%s\",\"zone_mask\":%u,"
        "\"segment_index\":%u,\"segment_count\":%u,\"dwelling\":%s,\"target_c\":%.2f,"
        "\"segment_elapsed_s\":%lu,\"dwell_remaining_s\":%lu,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,\"ramp_stretch_segment_s\":%.2f,\"ramp_stretch_total_s\":%.2f,"
        "\"ramp_dwell_credit_applied_s\":%.2f,"
        "\"fault_reason\":\"%s\",\"fault_guard\":%u,\"mode_state_fault_latched\":%s,"
        "\"mode_state_violation_count\":%lu,\"zone_blocked_mask\":%u,"
        "\"total_planned_s\":%s,\"elapsed_s\":%lu,\"remaining_s\":%s,\"remaining_is_estimate\":%s,",
        exec_state_name(st->state), st->profile_id, name_escaped, st->zone_mask, st->segment_index,
        st->segment_count, st->dwelling ? "true" : "false", (double)st->target_c,
        (unsigned long)st->segment_elapsed_s, (unsigned long)st->dwell_remaining_s,
        st->ramp_lock_held ? "true" : "false", st->ramp_lock_lagging_mask,
        (double)st->ramp_stretch_segment_s, (double)st->ramp_stretch_total_s,
        (double)st->ramp_dwell_credit_applied_s, reason_escaped, st->fault_guard,
        st->mode_state_fault_latched ? "true" : "false", (unsigned long)st->mode_state_violation_count,
        (unsigned)st->zone_blocked_mask,
        total_planned_buf, (unsigned long)elapsed_s, remaining_buf, remaining_is_estimate ? "true" : "false");
    size_t o = (n < 0 || (size_t)n >= DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE)
                   ? DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE - 1
                   : (size_t)n;
    /* last_run BEFORE the zones array on purpose: both appenders stop rather
     * than overflow, and the zones array is the unbounded-ish one (up to
     * MAX31856_CHANNEL_COUNT escaped fault reasons). Emitting the fixed-size
     * breadcrumb first means an unusually verbose fault can never be what
     * silently drops it from the response. */
    o = append_last_run_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o);
    o = append_zone_status_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, st, false);
    if (o + 1 < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE) json[o++] = '}';
    json[o < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE ? o : DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, o);
    free(json);
    free(st);
    return ret;
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
esp_err_t profile_plan_get_handler(httpd_req_t *req)
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

    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    profile_executor_get_status(st);
    float start_c = PROFILE_PLAN_PREVIEW_AMBIENT_C;
    if (st->state != PROFILE_EXEC_IDLE && st->profile_id == (uint8_t)id) {
        start_c = st->run_start_c;
    }

    profile_plan_point_t points[PLAN_MAX_POINTS];
    size_t point_count = 0;
    int64_t total_s = profile_feasibility_plan_curve(p.segments, p.segment_count, start_c, points,
                                                     PLAN_MAX_POINTS, &point_count);

    char name_escaped[sizeof(p.name) * 2 + 1];
    json_escape(p.name, name_escaped, sizeof(name_escaped));

    /* Fixed part plus up to PLAN_MAX_POINTS (25 for PROFILE_MAX_SEGMENTS ==
     * 12) points at ~40 bytes each worst case ("{"t":123456.00,"c":-999.99},").
     * HEAP, not stack: this handler runs on the same httpd_worker task as
     * /api/profile_exec and /api/control (dashboard_http.c's own audit,
     * 64 bytes free of 8192 measured live) -- every large transient buffer
     * on this task's request path adds to the same high-water mark, so a
     * "comfortably inside 8192" argument made handler-by-handler was the
     * bug, not a fix. Freed on every return path. */
    const size_t json_cap = 192 + PLAN_MAX_POINTS * 48;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/profile_plan: malloc(%u) failed for the response buffer",
                 (unsigned)json_cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        free(st);
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    size_t o = 0;
    int n = snprintf(json, json_cap, "{\"profile_id\":%ld,\"name\":\"%s\",\"total_planned_s\":", id,
                     name_escaped);
    o = (n < 0 || (size_t)n >= json_cap) ? json_cap - 1 : (size_t)n;
    if (total_s < 0) {
        n = snprintf(json + o, json_cap - o, "null,\"points\":[");
    } else {
        n = snprintf(json + o, json_cap - o, "%lld,\"points\":[", (long long)total_s);
    }
    if (n > 0 && (size_t)n < json_cap - o) o += (size_t)n;
    for (size_t i = 0; i < point_count; i++) {
        n = snprintf(json + o, json_cap - o, "%s{\"t\":%.0f,\"c\":%.2f}", i == 0 ? "" : ",",
                    (double)points[i].t, (double)points[i].c);
        if (n < 0 || (size_t)n >= json_cap - o) break;
        o += (size_t)n;
    }
    if (o + 2 < json_cap) {
        json[o++] = ']';
        json[o++] = '}';
    }
    json[o < json_cap ? o : json_cap - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, o);
    free(json);
    free(st);
    return ret;
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
esp_err_t control_status_get_handler(httpd_req_t *req)
{
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    profile_executor_get_status(st);

    /* Sized against the real worst case, not the previous estimate (Opus
     * review: 224/zone was already wrong before ff_hold_used_matrix/
     * ff_hold_infeasible existed -- the control_fields=true zone object was
     * 259B worst-case against a 224B budget, so append_zone_status_json()'s
     * own truncation guard (`if (n<0 || n>=cap-o) return o;`) was already
     * silently bailing on a 3-zone firing and this handler's caller still
     * appended the closing `}` on top of that partial buffer -- an
     * unterminated `"zones":[{...},{...` followed by `}`, invalid JSON,
     * with no error anywhere. Adding ff_hold_used_matrix/ff_hold_infeasible
     * made it 313B/zone (worst case, every field at its widest: 255 for an
     * ID/mask byte, -1234.56 for a plausible-worst actual_c, 4294967295 for
     * the sources mask) and pushed 3 zones to 1054B against 800 -- still
     * wrong, just more visibly so. round 3, item 3's ff_membership_change_
     * count field added another ~34B/zone worst case (381B/zone total).
     * 448/zone left real slack over that 381B measured worst case. ROADMAP
     * M15 B4 then added zone_duty_breakdown_t's twelve bd_* keys (~371B/zone
     * worst case) and raised the budget to 900/zone -- ~728B/zone measured
     * against 900, ~172B headroom; dashboard_json.h's
     * DASHBOARD_JSON_CONTROL_BUF_SIZE carries the per-key arithmetic. 256
     * fixed covers the state/zone_mask/target_c/ramp_lock header (~100B
     * worst case) with matching headroom. See
     * test_control_status_json_is_complete_and_well_formed_at_3_zones() --
     * this is the field that test exists to catch the next time someone
     * adds a key here without re-checking this budget.
     *
     * HEAP, not stack (same fix, same reasoning as /api/status and
     * /api/profile_exec above): httpd_worker was measured at 64 bytes free
     * of 8192 after a live run with this and /api/profile_exec's buffer
     * both on the stack. Freed on every return path; a malloc failure gets
     * a diagnosable 500 instead of a near-miss stack overflow. */
    char *json = heap_caps_malloc(DASHBOARD_JSON_CONTROL_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/control: malloc(%u) failed for the response buffer",
                 (unsigned)DASHBOARD_JSON_CONTROL_BUF_SIZE);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        free(st);
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    int n = snprintf(json, DASHBOARD_JSON_CONTROL_BUF_SIZE,
        "{\"state\":\"%s\",\"zone_mask\":%u,\"target_c\":%.2f,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,",
        exec_state_name(st->state), st->zone_mask, (double)st->target_c, st->ramp_lock_held ? "true" : "false",
        st->ramp_lock_lagging_mask);
    size_t o = (n < 0 || (size_t)n >= DASHBOARD_JSON_CONTROL_BUF_SIZE)
                   ? DASHBOARD_JSON_CONTROL_BUF_SIZE - 1
                   : (size_t)n;
    o = append_zone_status_json(json, DASHBOARD_JSON_CONTROL_BUF_SIZE, o, st, true);
    if (o + 1 < DASHBOARD_JSON_CONTROL_BUF_SIZE) json[o++] = '}';
    json[o < DASHBOARD_JSON_CONTROL_BUF_SIZE ? o : DASHBOARD_JSON_CONTROL_BUF_SIZE - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, o);
    free(json);
    free(st);
    return ret;
}

/* GET /api/firing_history?profile_id=N -- PID_EXPANSION_PLAN.md Phase 7a-2/
 * 7a-3's last-PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH persisted runs for one
 * profile, newest-first, each with per-zone firing_stats + the PID gains in
 * force at completion. profile_id follows /api/profile_plan?id=N's own
 * parsing convention (dashboard_http.c's profile_plan_get_handler() above).
 * An unknown/never-run profile_id is NOT a 404 -- unlike /api/profile_plan,
 * this has nothing to look up in profiles_nvs to validate against (a
 * profile can be deleted and its history is still worth showing), so it
 * always answers 200 with an empty "records" array rather than guessing
 * whether the id ever existed. */
esp_err_t firing_history_get_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile_id missing");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(query, "profile_id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile_id missing");
        return ESP_OK;
    }
    char *end = NULL;
    long id = strtol(id_str, &end, 10);
    if (end == id_str || id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad profile_id");
        return ESP_OK;
    }

    /* PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH (5) records, each up to
     * MAX31856_CHANNEL_COUNT active zones -- pulled off the stack via
     * profile_executor_get_firing_history() into a heap buffer (same
     * "large transient off the httpd_worker stack" discipline as every
     * other handler on this task, 64B free of 8192 measured live). Freed
     * before the records array too, once the JSON is rendered from it. */
    profile_firing_run_record_t *records = heap_caps_malloc(
        sizeof(profile_firing_run_record_t) * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (records == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/firing_history: malloc(%u) failed for the records buffer",
                 (unsigned)(sizeof(profile_firing_run_record_t) * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    size_t record_count = profile_executor_get_firing_history((uint8_t)id, records,
                                                               PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH);

    /* Fixed part per record (escaped name at worst-case, run_started_unix_s/
     * duration_s/zone_mask) ~150B, plus up to MAX31856_CHANNEL_COUNT zone
     * objects at ~600B worst case each (kp/ki/kd plus the same firing_stats
     * shape append_zone_status_json() renders, see dashboard_json.h's own
     * 472B-per-zone sizing note -- this adds ~100B more for kp/ki/ki/zone
     * index/braces, rounded up to 600 for headroom), times
     * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH records, plus a small wrapper. */
    const size_t json_cap = 64 + PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH *
                             (160 + MAX31856_CHANNEL_COUNT * 600);
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/firing_history: malloc(%u) failed for the response buffer",
                 (unsigned)json_cap);
        free(records);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    size_t o = dashboard_format_firing_history_json(json, json_cap, (uint8_t)id, records, record_count);
    free(records);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, o);
    free(json);
    return ret;
}

/* TODO.md section 0 / 6A.9: history ring buffer + CSV export, and the data
 * source for the dashboard graph. One row per sample; guard is
 * thermal_guard_trip_t (0 = none) so the graph can mark trips on the
 * timeline without a second request.
 *
 * 2026-09-01 (owner: "the duty cycle and all of the zones are not always
 * visible on the web graph"): the row used to carry ONE representative
 * zone's actual/duty -- every other zone only ever existed client-side in
 * the dashboard JS, gone on reload. profile_history_entry_t now carries
 * every zone's actual_c/duty/guard (profile_executor.h), so this widens to
 * one z<N>_actual_c,z<N>_duty,z<N>_guard triple per MAX31856_CHANNEL_COUNT
 * zone; desired_c stays a single column since it's one setpoint ramp shared
 * across a run's zones (TODO.md 6A.5), not per-zone data. A zone not active
 * in the run that produced a given row reads as an empty actual_c/duty field
 * (NAN) and guard 0 -- see profile_history_entry_t's own doc comment.
 * log_analysis.py (tools/PcTools) was updated for this column layout in the
 * same change.
 *
 * 2026-09-02 (opus review of 3f9b1a9/2a8ff7e): a trailing zone_mask column
 * was added -- THIS ROW's own zone_mask (captured at sample time,
 * profile_history_entry_t/history_slot_t), not the run currently active
 * when the request was served. Without it, a client averaging duty across
 * a row had nothing but the executor's CURRENT-run zone_mask to mask/divide
 * with, which is wrong whenever the executor is idle (mask 0, divide-by-
 * zero -> every historical duty point reads NaN) or the ring holds rows
 * from an earlier run with a different mask than the one now active.
 *
 * This is a display buffer covering roughly the last HISTORY_MAX_SAMPLES *
 * HISTORY_SAMPLE_PERIOD_S (currently 640 * 30s = ~5h20m), NOT a durable
 * record of a firing -- see HISTORY_MAX_SAMPLES's own doc comment
 * (profile_executor.h) for the sizing rationale. A firing longer than that
 * has its early history fall off the ring by design; this endpoint (and the
 * "Download CSV" affordance in the dashboard that hits it) exports whatever
 * the ring currently holds, not the whole firing. Durable, whole-firing
 * records are event_log.h's flash events (state transitions, faults) plus
 * the live debug-UART temperature feed -- this ring exists only to draw the
 * web/LCD graphs.
 *
 * Streamed in small batches via httpd_resp_send_chunk() rather than built
 * into one big buffer first: an earlier version allocated a full
 * HISTORY_MAX_SAMPLES-sized entries array (now ~13KB at the 2026-09-02 640-sample display-only sizing, previously ~58KB at 2880) *and* a full CSV text
 * buffer (~115KB) at once, and hit ESP_ERR_NO_MEM in practice against the
 * heap this board actually has free at runtime (Wi-Fi/lwIP/httpd already
 * hold a good chunk of it) -- the earlier size-report check only looked at
 * static DIRAM headroom, not the separate runtime heap this comes out of.
 * This version's peak allocation is one HISTORY_CSV_BATCH-sized entries
 * array plus one small text buffer, independent of how many samples exist. */
#define HISTORY_CSV_BATCH 128u
/* Wide enough for "elapsed,desired," plus 3 zone columns of up to
 * MAX31856_CHANNEL_COUNT zones ("-3276.7,100,255," worst case per zone,
 * ~16 bytes) with headroom -- computed off the zone count rather than
 * hand-picked so a future channel-count change can't silently truncate a
 * row. */
#define HISTORY_CSV_LINE_CAP (64u + 32u * MAX31856_CHANNEL_COUNT)

esp_err_t history_csv_get_handler(httpd_req_t *req)
{
    profile_history_entry_t *batch = malloc(sizeof(profile_history_entry_t) * HISTORY_CSV_BATCH);
    char *line = malloc(HISTORY_CSV_LINE_CAP);
    if (!batch || !line) {
        free(batch);
        free(line);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"kiln_history.csv\"");

    size_t o = (size_t)snprintf(line, HISTORY_CSV_LINE_CAP, "elapsed_s,desired_c");
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && o < HISTORY_CSV_LINE_CAP; zi++) {
        o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, ",z%u_actual_c,z%u_duty,z%u_guard", zi, zi, zi);
    }
    /* zone_mask trails the per-zone triples (appended, not interleaved) so
     * an older parser keyed on column NAME (log_analysis.py's csv.
     * DictReader) keeps working unmodified -- see profile_history_entry_t's
     * own doc comment (profile_executor.h) for why this is THIS row's mask,
     * not the executor's current-run one. */
    if (o < HISTORY_CSV_LINE_CAP) o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, ",zone_mask");
    if (o < HISTORY_CSV_LINE_CAP) o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, "\n");
    esp_err_t err = httpd_resp_send_chunk(req, line, o < HISTORY_CSV_LINE_CAP ? o : HISTORY_CSV_LINE_CAP - 1);

    size_t start = 0;
    while (err == ESP_OK) {
        size_t got = profile_executor_get_history(batch, start, HISTORY_CSV_BATCH);
        if (got == 0) break;
        for (size_t i = 0; i < got && err == ESP_OK; i++) {
            o = (size_t)snprintf(line, HISTORY_CSV_LINE_CAP, "%lu,%.2f", (unsigned long)batch[i].elapsed_s,
                                 (double)batch[i].desired_c);
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && o < HISTORY_CSV_LINE_CAP; zi++) {
                o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, ",%.2f,%.3f,%u",
                                      (double)batch[i].actual_c[zi], (double)batch[i].duty[zi], batch[i].guard[zi]);
            }
            if (o < HISTORY_CSV_LINE_CAP) o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, ",%u", batch[i].zone_mask);
            if (o < HISTORY_CSV_LINE_CAP) o += (size_t)snprintf(line + o, HISTORY_CSV_LINE_CAP - o, "\n");
            err = httpd_resp_send_chunk(req, line, o < HISTORY_CSV_LINE_CAP ? o : HISTORY_CSV_LINE_CAP - 1);
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

esp_err_t profile_exec_start_post_handler(httpd_req_t *req)
{
    /* system_mode_gate (slice 2): checked first, before even reading the
     * body -- retires recovery_start_refusal.h's separate, HTTP-only wording
     * (ui_aggregate_review_2026-09-08's finding that the recovery banner's
     * "Firing is NOT available" claim was never enforced by any route). Uses
     * the same recovery_mode fact readiness_gate_facts_t carries, collected
     * once and shared with the readiness call below. JSON, not plain text --
     * main_page.html's proceedToStart() calls r.json() on every response, so
     * a plain-text body throws in the parse and lands in .catch(), leaving an
     * operator who pressed Start in recovery mode looking at nothing (the bug
     * recovery_start_refusal.h's plain httpd_resp_sendstr() had). */
    char recovery_err[192];
    readiness_gate_facts_t facts;
    readiness_gate_collect(&facts);

    sys_mode_snapshot_t mode_snap;
    memset(&mode_snap, 0, sizeof(mode_snap));
    mode_snap.recovery_mode = facts.recovery_mode;
    if (system_mode_gate_check(SYS_ACTION_START_PROFILE, &mode_snap, recovery_err, sizeof(recovery_err))) {
        ESP_LOGW(DASH_TAG, "profile_exec/start refused by the system mode gate (recovery mode)");
        char json[sizeof(recovery_err) + 96];
        int n = snprintf(json, sizeof(json),
                         "{\"ok\":false,\"readiness_item\":\"%s\",\"error\":\"%s\"}",
                         READINESS_GATE_KEY_RECOVERY_MODE, recovery_err);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    /* THE READINESS INTERLOCK (readiness_gate.h). profile_executor_run()
     * enforces this for every start path -- this call is a LEGIBILITY
     * duplicate for the HTTP one only: it answers 409 Conflict with the
     * blocking item named in full, before the body is even read, instead of
     * the generic 400 the run() refusal path below produces. Deliberately
     * reuses recovery_err[] rather than adding a second ~200-byte local to a
     * frame that runs on the shared 8 KB httpd task stack
     * (check_httpd_task_stack_budget; see CLAUDE.md's httpd-stack-blob note),
     * and reuses the facts collected above rather than re-collecting them --
     * the recovery check has already returned by the time this writes.
     *
     * If this call were ever deleted, the firing would still be refused --
     * just with a less specific status and message. If profile_executor_run()'s
     * check were deleted, this one would NOT cover the LCD or benchproto start
     * paths. Do not "de-duplicate" by removing the one in run(). */
    readiness_gate_block_t gate_which = readiness_gate_evaluate(&facts, recovery_err, sizeof(recovery_err));
    if (gate_which != READINESS_GATE_OK) {
        const char *item_key = readiness_gate_item_key(gate_which);
        ESP_LOGW(DASH_TAG, "profile_exec/start refused by the readiness interlock (item %s)",
                 item_key ? item_key : "?");
        /* JSON, same envelope as the recovery refusal above: this
         * page's start handler (main_page.html's proceedToStart()) parses the
         * response with r.json() and shows result.error, so a plain-text body
         * would throw in the parse and land in its .catch() -- an operator
         * pressing Start would see NOTHING happen. "readiness_item" carries
         * the /api/readiness key so the page can name and link the one item
         * that blocked, instead of dropping the operator on a checklist of
         * eighteen to find it themselves.
         *
         * No json_escape() here, deliberately: readiness_gate_evaluate()'s
         * messages are compile-time constants guaranteed free of '"' and '\\'
         * (see its own comment, enforced by test_readiness_gate.c's
         * test_messages_are_json_safe()), so escaping would only buy a
         * doubled ~400-byte scratch buffer on this shared stack -- the exact
         * httpd-stack-blob class CLAUDE.md warns about -- to protect against
         * an input that cannot occur. */
        char json[sizeof(recovery_err) + 96];
        int n = snprintf(json, sizeof(json),
                         "{\"ok\":false,\"readiness_item\":\"%s\",\"error\":\"%s\"}",
                         item_key ? item_key : "", recovery_err);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

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

    /* danger_mode.h's own refusal is one-directional (refuses to OPEN the
     * window during a firing) -- this is the other half: refuse to START a
     * firing while the window is already open. Unlike watchdog_cfg's
     * log-only bypass just below, this is a hard refusal, not a warning:
     * danger_mode_active() means kiln_io_owner.c's relay_on_blocked() is
     * skipping every safety-fault/OTA-update gate on these same four
     * relays, and the window can auto-expire-and-REBOOT mid-firing with no
     * warning to whatever profile_executor.c was doing at the time. A
     * firing must never start into that state; see danger_mode_request_
     * start()'s own PROFILE_EXEC_RUNNING/PAUSED refusal for the symmetric
     * check in the other direction. */
    if (danger_mode_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "refused -- danger mode is active (diagnostics page); stop it first");
        return ESP_OK;
    }

    /* watchdog_cfg.h: a UI-only warning (main_page.html/ui_page_home.c's
     * extra confirmation dialogs) is bypassable with curl straight to this
     * endpoint. Do NOT refuse the start -- the owner wants this usable during
     * development -- but log it loudly server-side so it is never a silent
     * fact about a running firing. */
    if (watchdog_cfg_panic_disabled()) {
        ESP_LOGW(DASH_TAG, "profile_exec/start: id=%ld starting with the task-watchdog PANIC DISABLED -- "
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

esp_err_t profile_exec_stop_post_handler(httpd_req_t *req)
{
    profile_executor_halt();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t profile_exec_pause_post_handler(httpd_req_t *req)
{
    bool ok = profile_executor_pause();
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing running to pause");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t profile_exec_resume_post_handler(httpd_req_t *req)
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
esp_err_t profile_exec_ack_last_run_post_handler(httpd_req_t *req)
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
esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)
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

