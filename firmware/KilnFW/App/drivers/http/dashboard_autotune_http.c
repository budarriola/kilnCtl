/* Autotune family -- moved out of dashboard_http.c 2026-09-04 (ROADMAP.md
 * M15, the 1500-line rule): GET /api/autotune, /api/autotune/matrix,
 * /api/autotune/trace.csv; POST /api/autotune/{start,abort,accept}. See
 * dashboard_http_internal.h for the shared s_dash/DASH_TAG seam. */

#include "dashboard_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "autotune_engine.h"
#include "dashboard_json.h"
#include "http_form.h"
#include "profile_executor.h"
#include "readiness_gate.h"
#include "recovery_start_refusal.h"
#include "zones_config_accessors.h"

/* autotune_state_name()/autotune_rule_name()/autotune_refusal_name() and the
 * response body itself moved to dashboard_json.c's
 * dashboard_format_autotune_status_json() (2026-08-31 dashboard-split pass)
 * -- pure formatting with no httpd/hardware dependency, so it can be host-
 * tested the same way append_zone_status_json() already is. Same buffer size
 * (1300, unchanged) and same snprintf-into-stack-buffer shape as before; only
 * where the formatting code is DEFINED changed. */
esp_err_t autotune_status_get_handler(httpd_req_t *req)
{
    autotune_engine_status_t st;
    autotune_engine_get_status(&st);

    char json[1300];
    int n = dashboard_format_autotune_status_json(json, sizeof(json), &st);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* TODO.md 6A.5(b): cross-zone coupling matrix built up across completed
 * autotune runs (one row per zone that's been tested). Cheap enough
 * (MAX31856_CHANNEL_COUNT^2 cells) to send as one JSON object, no pagination
 * needed unlike the trace/history endpoints. */
esp_err_t autotune_matrix_get_handler(httpd_req_t *req)
{
    autotune_coupling_matrix_t m;
    autotune_engine_get_coupling_matrix(&m);

    /* Second term is the cells array, third is the RGA block appended below
     * (n^2 Lambda values plus the zone map, or a refusal reason). HEAP, not
     * stack: this runs on the same httpd_worker task as every handler above
     * (measured at 64 bytes free of 8192 live) -- ~1.2KB of locals here adds
     * to the same high-water mark those handlers do. Freed on every return
     * path. */
    const size_t json_cap = 64 + MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT * 96
                           + 128 + MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT * 16;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(DASH_TAG, "GET /api/autotune_matrix: malloc(%u) failed for the response buffer",
                 (unsigned)json_cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    /* Report the zones this board actually HAS, not the number of MAX31856
     * channels the hardware could carry. These differ whenever an operator
     * has declared fewer thermocouples than are wired (thermo_count=1 on a
     * 3-channel board is the bench's normal state), and every zones_config_*
     * getter already refuses an index >= thermo_count -- so the extra rows
     * and columns were cells that could never become valid, rendered as a
     * 3x3 grid of "not measured yet" on a kiln with one zone. */
    const uint8_t zone_count = zones_config_get_thermo_count();
    size_t o = 0;

    /* Self-clamping append via dashboard_json.c's shared json_append_clamped()
     * -- snprintf returns the WOULD-BE length even when truncated, so an
     * unguarded `o += snprintf(json+o, json_cap-o, ...)` lets `o` walk past
     * `json_cap`; the next call's `json_cap - o` then wraps a size_t and
     * writes out of bounds. This was a stack smash before this handler's
     * buffer moved to the heap (coordinator review, 2026-08-31); it is a
     * HEAP smash now, corrupting some other allocation instead of tripping a
     * stack canary -- worse, not better, if it were ever reachable.
     * Unreachable at MAX31856_CHANNEL_COUNT == 3 (this json_cap comfortably
     * covers the ~400B the RGA block can produce), but json_append_clamped()
     * clamps `o` back to `json_cap - 1` after EVERY call, not just once
     * after the cells loop, so nothing downstream can ever see
     * `o > json_cap - 1` again regardless of channel count -- and the same
     * function is host-tested directly (test_dashboard_json.c) against a
     * long chain of appends into a deliberately undersized buffer, which
     * this handler itself cannot be (dashboard_json.h's own note on why
     * dashboard_http.c doesn't compile on the host). */
#define RGA_APPEND(...) (o = json_append_clamped(json, json_cap, o, __VA_ARGS__))

    RGA_APPEND("{\"zone_count\":%u,\"cells\":[", (unsigned)zone_count);
    bool first = true;
    for (uint8_t i = 0; i < zone_count; i++) {
        for (uint8_t j = 0; j < zone_count; j++) {
            const autotune_coupling_cell_t *c = &m.cell[i][j];
            if (!first) RGA_APPEND(",");
            first = false;
            if (c->valid) {
                RGA_APPEND("{\"i\":%u,\"j\":%u,\"valid\":true,\"k\":%.3f,\"tau_s\":%.1f,\"dead_time_s\":%.1f}", i, j,
                    (double)c->model.k_gain_c_per_duty, (double)c->model.tau_s, (double)c->model.dead_time_s);
            } else {
                RGA_APPEND("{\"i\":%u,\"j\":%u,\"valid\":false}", i, j);
            }
            if (o >= json_cap - 1) break;
        }
    }

    RGA_APPEND("]");

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
        RGA_APPEND(",\"rga\":{\"available\":true,\"n\":%d,\"det\":%.4g,\"zones\":[",
                   rga.n, (double)rga.determinant);
        for (int a = 0; a < rga.n; a++) {
            RGA_APPEND("%s%u", a ? "," : "", (unsigned)rga.zone_index[a]);
        }
        RGA_APPEND("],\"lambda\":[");
        for (int a = 0; a < rga.n; a++) {
            RGA_APPEND("%s[", a ? "," : "");
            for (int b = 0; b < rga.n; b++) {
                RGA_APPEND("%s%.4f", b ? "," : "", (double)rga.lambda[a][b]);
            }
            RGA_APPEND("]");
        }
        RGA_APPEND("]}");
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
        RGA_APPEND(",\"rga\":{\"available\":false,\"code\":%d,\"reason\":\"%s\"}",
                   (int)rga.status, rga_reason);
    }

    RGA_APPEND("}");
#undef RGA_APPEND

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, o < json_cap ? o : json_cap - 1);
    free(json);
    return ret;
}

esp_err_t autotune_start_post_handler(httpd_req_t *req)
{
    /* recovery_start_refusal.h: same explicit, named recovery-mode
     * enforcement as profile_exec_start_post_handler() (dashboard_exec_http.c)
     * -- see that header's doc comment. Checked first, before the body is
     * even read. */
    char recovery_err[192];
    if (recovery_mode_refuses_start(recovery_err, sizeof(recovery_err))) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, recovery_err);
        return ESP_OK;
    }

    /* THE READINESS INTERLOCK (readiness_gate.h), extended to autotune the
     * same day as the firing gate (owner decision 2026-09-09): autotune_begin_
     * run_locked() enforces this for every autotune start path -- this call is
     * a LEGIBILITY duplicate for the HTTP one only, same convention and same
     * reasoning as profile_exec_start_post_handler()'s identical block
     * (dashboard_exec_http.c): it answers 409 Conflict with the blocking item
     * named in full, before the body is even read, instead of the generic 400
     * autotune_begin_run_locked()'s own refusal produces further down.
     * Deliberately reuses recovery_err[] as the OUTPUT buffer for
     * readiness_gate_refuses_start()'s message, rather than adding a second
     * ~200-byte local for that purpose, on the shared 8 KB httpd task stack
     * (check_httpd_task_stack_budget; CLAUDE.md's httpd-stack-blob note).
     * This does NOT mean no further local is added: the json[] buffer a few
     * lines below (sizeof(recovery_err) + 96 = 288 B) IS a genuine second
     * stack local, needed to wrap recovery_err's text in the JSON envelope
     * this endpoint's caller expects -- check_httpd_task_stack_budget.py
     * measures autotune_start_post_handler's reachable depth well under this
     * file's worst path (4304 B of the 4832 B ceiling, cfgfs_status_get_
     * handler) with this buffer included, so it is not the blob this repo's
     * class of bug looks for; it just is not what the sentence above is
     * about.
     *
     * If this call were ever deleted, the autotune start would still be
     * refused -- just with a less specific status/message. If autotune_begin_
     * run_locked()'s check were deleted, this one would NOT cover the
     * benchproto start path. Do not "de-duplicate" by removing the one in
     * autotune_begin_run_locked(). */
    readiness_gate_block_t gate_which = READINESS_GATE_OK;
    if (readiness_gate_refuses_start(recovery_err, sizeof(recovery_err), &gate_which)) {
        const char *item_key = readiness_gate_item_key(gate_which);
        ESP_LOGW(DASH_TAG, "autotune/start refused by the readiness interlock (item %s)",
                 item_key ? item_key : "?");
        /* JSON, not the plain string the recovery refusal above sends -- same
         * reasoning as profile_exec_start_post_handler()'s identical block:
         * the page parses this response as JSON. No json_escape() needed for
         * the same reason as there: readiness_gate_evaluate()'s messages are
         * compile-time constants proven free of '"'/'\\'
         * (test_readiness_gate.c's test_messages_are_json_safe()). */
        char json[sizeof(recovery_err) + 96];
        int n = snprintf(json, sizeof(json),
                         "{\"ok\":false,\"readiness_item\":\"%s\",\"error\":\"%s\"}",
                         item_key ? item_key : "", recovery_err);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

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
        /* rule is optional on the step path too, and SIMC is the default --
         * an omitted field, or an older client (PC tools, MCP autotune_start,
         * test harnesses) that has never heard of this parameter, must get
         * exactly today's behavior. Only "simc" and "cohen-coon" are valid
         * here: ZN and Tyreus-Luyben are relay-only and are refused at this
         * door rather than let through to autotune_engine_run(), which would
         * refuse them anyway but only after the caller thinks the request was
         * accepted -- see PID_EXPANSION_PLAN.md Phase 1 and §2a for why
         * Cohen-Coon must stay opt-in, never the default, on a kiln. */
        int step_rule_len = http_form_find_field(body, "rule", rule_val, sizeof(rule_val));
        autotune_rule_t step_rule = AUTOTUNE_RULE_SIMC;
        if (step_rule_len > 0 && strcmp(rule_val, "cohen-coon") == 0) {
            step_rule = AUTOTUNE_RULE_COHEN_COON;
        } else if (step_rule_len > 0 && strcmp(rule_val, "simc") != 0) {
            params_ok = false;
            snprintf(err_msg, sizeof(err_msg), "rule must be \"simc\" or \"cohen-coon\" on the step-test path "
                                                "(zn/tl are relay-only)");
        }
        if (params_ok) {
            started = autotune_engine_run((uint8_t)zone, step_duty, step_rule, err_msg, sizeof(err_msg));
        }
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

esp_err_t autotune_abort_post_handler(httpd_req_t *req)
{
    autotune_engine_abort("aborted from web UI");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t autotune_accept_post_handler(httpd_req_t *req)
{
    /* ack_unsettled is optional and defaults false -- an omitted body, or an
     * older client that has never heard of this field, gets exactly the
     * refuse-a-low-confidence-fit behavior autotune_engine_accept()'s own
     * comment documents; only an explicit "1" opts in to persisting a fit
     * that never genuinely settled. Same http_form_find_field() body-parse
     * pattern autotune_start_post_handler() above already uses, not a new
     * one. A body is optional here (the common case, accepting a genuinely
     * settled fit, needs none), so a missing/empty body is not an error. */
    /* adopt_ceiling (TODO.md 6A.4) is the same opt-in shape as ack_unsettled
     * just below: an omitted body, or an older client that has never heard
     * of this field, gets exactly today's behavior (gains/model only,
     * max_ramp_c_per_hr untouched) -- only an explicit "1"/"true" opts in
     * to also adopting the run's predicted ramp ceiling. See autotune_
     * engine_accept()'s own comment (autotune_engine.h) for what
     * "adopt" does and does not overwrite. */
    bool ack_unsettled = false;
    bool adopt_ceiling = false;
    if (req->content_len > 0 && req->content_len < 96) {
        char body[96];
        size_t received = 0;
        bool read_ok = true;
        while (received < (size_t)req->content_len) {
            int ret = httpd_req_recv(req, body + received, req->content_len - received);
            if (ret <= 0) {
                read_ok = false;
                break;
            }
            received += (size_t)ret;
        }
        if (read_ok) {
            body[received] = '\0';
            /* Sized [8], not [4]: http_form_url_decode() (http_form.h)
             * requires o+1 < out_cap for every decoded byte it writes, so a
             * 4-byte cap can NEVER hold "true" (4 chars + NUL = 5 bytes,
             * and the o+1 < out_cap guard actually needs 6). A [4] buffer
             * silently truncated "true" to "tru" (or refused to decode it
             * at all, depending on where url_decode's write loop gave up),
             * so a client sending the literal string "true" (rather than
             * "1") could never opt in to either flag -- found in review. */
            char ack_val[8];
            int ack_len = http_form_find_field(body, "ack_unsettled", ack_val, sizeof(ack_val));
            ack_unsettled = (ack_len > 0) && (strcmp(ack_val, "1") == 0 || strcmp(ack_val, "true") == 0);
            char ceiling_val[8];
            int ceiling_len = http_form_find_field(body, "adopt_ceiling", ceiling_val, sizeof(ceiling_val));
            adopt_ceiling = (ceiling_len > 0) && (strcmp(ceiling_val, "1") == 0 || strcmp(ceiling_val, "true") == 0);
        }
    }

    autotune_accept_opts_t accept_opts = {.ack_unsettled = ack_unsettled, .adopt_ceiling = adopt_ceiling};
    autotune_accept_result_t accept_result = {0};
    if (!autotune_engine_accept(&accept_opts, &accept_result)) {
        /* The specific reason (never settled / extrapolation didn't
         * converge / tau inconsistent with the corrected gain) is in the
         * ESP_LOGW autotune_engine_accept() itself already emitted -- see
         * that function's own comment. This HTTP error stays generic
         * because the page's own /api/autotune poll already shows the
         * operator all three flags distinctly (model_settled/
         * model_extrapolation_converged/model_tau_consistent) BEFORE they
         * click Accept, which is the more useful place for that detail. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "no completed autotune result to accept, or it is not fully trustworthy "
                            "yet (see the page for which condition) and needs ack_unsettled=1 to "
                            "accept anyway");
        return ESP_OK;
    }
    /* Review fix: report the ceiling-adoption outcome to the caller instead
     * of only the ESP_LOGx lines inside autotune_engine_accept() --
     * "adopted" vs "silently skipped" vs "rejected as out of range" were
     * previously indistinguishable from this response alone. */
    const char *outcome_name = "SKIPPED_NOT_REQUESTED";
    switch (accept_result.adoption) {
    case AUTOTUNE_CEILING_ADOPTED: outcome_name = "ADOPTED"; break;
    case AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED: outcome_name = "SKIPPED_NOT_REQUESTED"; break;
    case AUTOTUNE_CEILING_SKIPPED_RELAY_METHOD: outcome_name = "SKIPPED_RELAY_METHOD"; break;
    case AUTOTUNE_CEILING_SKIPPED_MODEL_NOT_PERSISTED: outcome_name = "SKIPPED_MODEL_NOT_PERSISTED"; break;
    case AUTOTUNE_CEILING_SKIPPED_ZERO: outcome_name = "SKIPPED_ZERO"; break;
    case AUTOTUNE_CEILING_SKIPPED_WOULD_TIGHTEN: outcome_name = "SKIPPED_WOULD_TIGHTEN"; break;
    case AUTOTUNE_CEILING_REJECTED_OUT_OF_RANGE: outcome_name = "REJECTED_OUT_OF_RANGE"; break;
    case AUTOTUNE_CEILING_SKIPPED_READ_FAILED: outcome_name = "SKIPPED_READ_FAILED"; break;
    case AUTOTUNE_CEILING_FAILED_TO_PERSIST: outcome_name = "FAILED_TO_PERSIST"; break;
    }
    /* Sized to the longest actual response rather than a round number (this
     * task stack has been within 64 B of overflow before -- see
     * check_stack_margin_baseline.ps1). Longest pieces: the format's fixed
     * text is 81 bytes EXCLUDING the NUL terminator; the longest outcome
     * name is "SKIPPED_MODEL_NOT_PERSISTED" (27 bytes); both ceiling values
     * are validated to [0, ZONE_MAX_RAMP_C_PER_HR_MAX] = [0, 1000.0] before
     * ever reaching here (zones_config_set_max_ramp()'s own range check,
     * and the READ_FAILED/out-of-range paths above never populate a
     * nonzero value), so "%.1f" of either is at most 6 bytes ("1000.0").
     * Worst case both ceilings print at 6 bytes: 81 + 27 + 6 + 6 + 1 (NUL)
     * = 121; rounded up to a clean 128. */
    char json[128];
    int n = snprintf(json, sizeof(json),
                      "{\"ok\":true,\"ceiling_adoption\":\"%s\",\"ceiling_old_c_per_hr\":%.1f,"
                      "\"ceiling_new_c_per_hr\":%.1f}",
                      outcome_name, (double)accept_result.old_ceiling_c_per_hr,
                      (double)accept_result.new_ceiling_c_per_hr);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

/* Streamed the same way history_csv_get_handler() is, for the same reason
 * (see that function's comment) -- a single ~69KB one-shot buffer was tried
 * first here too and is exactly the pattern that turned out to risk
 * ESP_ERR_NO_MEM against this board's actual runtime-free heap. */
#define AUTOTUNE_CSV_BATCH 128u

esp_err_t autotune_trace_csv_get_handler(httpd_req_t *req)
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

