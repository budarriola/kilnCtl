// Campaign 8 (autotune part): handler-level matrix for POST /api/autotune/{start,abort,accept},
// compiled from the REAL dashboard_autotune_http.c with the real system_mode_gate.c.
// The engine, readiness facts and danger_mode are fakes that COUNT calls, so every refusal is
// asserted as "engine never invoked" (module state), not just by the reply status.
//
// Findings recorded in docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#define __attribute__(x)
#include "../drivers/http/dashboard_autotune_http.c"

/* ---- fakes ---- */
void json_escape(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    for (; src && *src && o + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') out[o++] = '\\';
        out[o++] = *src;
    }
    out[o] = 0;
}
static readiness_gate_facts_t f_facts;
void readiness_gate_collect(readiness_gate_facts_t *out) { *out = f_facts; }
static bool f_danger;
bool danger_mode_active(void) { return f_danger; }
bool danger_mode_blocks_start(void) { return f_danger; }
bool watchdog_cfg_panic_disabled(void) { return false; }

static int f_run_calls, f_relay_calls, f_abort_calls, f_accept_calls;
static uint8_t f_zone;
static float f_duty, f_sp, f_d, f_h;
static autotune_rule_t f_rule;
static bool f_run_result = true;
static const char *f_run_err = "";
bool autotune_engine_run(uint8_t zone, float duty, autotune_rule_t rule, char *err, size_t cap)
{
    f_run_calls++;
    f_zone = zone; f_duty = duty; f_rule = rule;
    if (!f_run_result) snprintf(err, cap, "%s", f_run_err);
    return f_run_result;
}
bool autotune_engine_run_relay(uint8_t zone, float sp, float d, float h, autotune_rule_t rule, char *err, size_t cap)
{
    f_relay_calls++;
    f_zone = zone; f_sp = sp; f_d = d; f_h = h; f_rule = rule;
    if (!f_run_result) snprintf(err, cap, "%s", f_run_err);
    return f_run_result;
}
void autotune_engine_abort(const char *reason) { (void)reason; f_abort_calls++; }
static bool f_accept_result, f_acc_ack, f_acc_adopt;
static autotune_accept_result_t f_acc_out;
bool autotune_engine_accept(const autotune_accept_opts_t *o, autotune_accept_result_t *out)
{
    f_accept_calls++;
    f_acc_ack = o->ack_unsettled;
    f_acc_adopt = o->adopt_ceiling;
    *out = f_acc_out;
    return f_accept_result;
}
static int f_mode_refusals;

/* ---- transport ---- */
static int s_status;
static char s_reply[512];
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n)
{
    (void)r;
    snprintf(s_reply, sizeof(s_reply), "%.*s", (int)n, b);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send(r, s, (ssize_t)strlen(s)); }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m)
{
    (void)r;
    s_status = (int)e;
    snprintf(s_reply, sizeof(s_reply), "%s", m ? m : "");
    return ESP_OK;
}
esp_err_t system_mode_gate_http_send_refusal(httpd_req_t *req, const char *reason)
{
    f_mode_refusals++;
    s_status = 409;
    return httpd_resp_send(req, reason, (ssize_t)strlen(reason));
}

static const char *s_body;
static size_t s_off, s_chunk, s_fail_after;
static int s_fail_ret;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    (void)r;
    size_t blen = s_body ? strlen(s_body) : 0;
    if (s_off >= s_fail_after) return s_fail_ret;
    if (s_off >= blen) return 0;
    size_t n = blen - s_off;
    if (n > len) n = len;
    if (n > s_chunk) n = s_chunk;
    if (s_off + n > s_fail_after) n = s_fail_after - s_off;
    memcpy(buf, s_body + s_off, n);
    s_off += n;
    return (int)n;
}

static int post(esp_err_t (*h)(httpd_req_t *), const char *body, long long clen, size_t fail_after, int fail_ret)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = clen;
    s_body = body; s_off = 0; s_chunk = 3;
    s_fail_after = fail_after; s_fail_ret = fail_ret;
    s_status = 200;
    s_reply[0] = 0;
    (void)h(&req);
    return s_status;
}
static int post_ok(esp_err_t (*h)(httpd_req_t *), const char *body)
{
    return post(h, body, (long long)strlen(body), (size_t)-1, 0);
}

static void reset(void)
{
    char msg[192];
    memset(&f_facts, 0, sizeof(f_facts));
    f_facts.safety_link_up = true;
    f_facts.estop_verified = true;
    TEST_CHECK(readiness_gate_evaluate(&f_facts, msg, sizeof(msg)) == READINESS_GATE_OK, "setup: readiness gate passes");
    f_run_calls = f_relay_calls = f_abort_calls = f_accept_calls = f_mode_refusals = 0;
    f_run_result = true; f_run_err = "";
    f_danger = false;
    f_zone = 0xEE; f_duty = -1; f_sp = f_d = f_h = -1; f_rule = (autotune_rule_t)99;
    f_accept_result = true; f_acc_ack = f_acc_adopt = false;
    memset(&f_acc_out, 0, sizeof(f_acc_out));
}
#define NO_START() (f_run_calls == 0 && f_relay_calls == 0)

static void test_start_gates(void)
{
    TEST_SECTION("start: recovery / danger / readiness refusals: engine never invoked");
    reset();
    f_facts.recovery_mode = true;
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 409 && NO_START(), "recovery mode -> 409");
    TEST_CHECK(strstr(s_reply, "\"ok\":false") != NULL, "refusal is JSON");
    reset();
    f_facts.crash_have_record = true;
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 409 && NO_START(), "unacknowledged crash -> 409");
    TEST_CHECK(strstr(s_reply, "readiness_item") != NULL, "names readiness item");
    reset();
    f_facts.safety_link_up = true;
    f_facts.safety_trip_mask = 0x0020;
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 409 && NO_START(), "latched safety trip -> 409");
    reset();
    f_danger = true;
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 409 && NO_START(), "danger mode -> 409");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0&method=relay&setpoint_c=500") == 409 && NO_START(),
               "danger mode also refuses relay method");
    f_danger = false;
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 200 && f_run_calls == 1, "control: starts when gates clear");
}

static void test_start_step(void)
{
    TEST_SECTION("start (step): body bounds, field parse, defaults, rules");
    reset();
    TEST_CHECK(post(autotune_start_post_handler, "", 0, (size_t)-1, 0) == 400 && NO_START(), "empty body -> 400");
    {
        char big[194];
        memset(big, 'a', 193);
        big[193] = 0;
        memcpy(big, "zone=0&x=", 9);
        TEST_CHECK(post(autotune_start_post_handler, big, 193, (size_t)-1, 0) == 400 && NO_START(),
                   "193-byte body (valid zone, delivered in full) -> 400, cap is 192");
    }
    TEST_CHECK(post(autotune_start_post_handler, "zone=0", -1, (size_t)-1, 0) == 400 && NO_START(), "content_len -1 -> 400");
    TEST_CHECK(post(autotune_start_post_handler, "zone=0", 20, (size_t)-1, 0) == 400 && NO_START(), "truncated -> 400");
    TEST_CHECK(post(autotune_start_post_handler, "zone=0", 6, 3, 0) == 400 && NO_START(), "peer closes mid-body -> 400");
    TEST_CHECK(post(autotune_start_post_handler, "zone=0", 6, 3, -1) == 400 && NO_START(), "recv error -> 400");
    const char *bad[] = { "x=1", "zone", "zone=", "zone=abc", "zone=-1", "zone=256", "zone=1.5", "zone=1x", "zone=99999999999" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char m[96];
        snprintf(m, sizeof(m), "body [%s] -> 400, engine not started", bad[i]);
        TEST_CHECK(post_ok(autotune_start_post_handler, bad[i]) == 400 && NO_START(), m);
    }
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&step_duty=") == 400 && NO_START(), "empty step_duty -> 400");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&step_duty=abc") == 400 && NO_START(), "non-numeric step_duty -> 400");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=2") == 200 && f_run_calls == 1, "zone only -> 200");
    TEST_CHECK(f_zone == 2 && f_duty == 0.5f && f_rule == AUTOTUNE_RULE_SIMC, "defaults: duty 0.5, SIMC");
    TEST_CHECK(strstr(s_reply, "\"ok\":true") != NULL, "ok reply");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=255&step_duty=0.25&method=step&rule=cohen-coon") == 200 &&
                   f_zone == 255 && f_duty == 0.25f && f_rule == AUTOTUNE_RULE_COHEN_COON,
               "explicit step with cohen-coon, zone 255");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0&rule=zn") == 400 && NO_START(), "zn on step path refused");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0&rule=tl") == 400 && NO_START(), "tl on step path refused");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0&method=banana") == 400 && NO_START(), "unknown method refused");
    TEST_CHECK(strstr(s_reply, "method must be") != NULL, "method refusal reply names method");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0&zone=7") == 200 && f_zone == 0, "duplicate zone: first wins");
    reset();
    f_run_result = false;
    f_run_err = "zone busy";
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=0") == 400 && f_run_calls == 1, "engine refusal -> 400");
    TEST_CHECK(strstr(s_reply, "\"ok\":false") != NULL && strstr(s_reply, "zone busy") != NULL, "engine reason relayed as JSON");
}

static void test_start_relay(void)
{
    TEST_SECTION("start (relay): setpoint required, rule, d/h defaults");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay") == 400 && NO_START(), "relay without setpoint_c -> 400");
    TEST_CHECK(strstr(s_reply, "setpoint_c") != NULL, "reply names setpoint_c");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=") == 400 && NO_START(), "empty setpoint -> 400");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=abc") == 400 && NO_START(), "bad setpoint -> 400");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500&relay_d=x") == 400 && NO_START(), "bad relay_d -> 400");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500&relay_h=") == 400 && NO_START(), "empty relay_h -> 400");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500&rule=simc") == 400 && NO_START(), "simc on relay path refused");
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500&rule=cohen-coon") == 400 && NO_START(), "cohen-coon on relay path refused");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500") == 200 && f_relay_calls == 1 && f_run_calls == 0, "relay ok");
    TEST_CHECK(f_zone == 1 && f_sp == 500.0f && f_d == 0.0f && f_h == 0.0f && f_rule == AUTOTUNE_RULE_TYREUS_LUYBEN,
               "defaults: d/h 0 (engine default), Tyreus-Luyben");
    reset();
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=450.5&relay_d=0.3&relay_h=2&rule=zn") == 200 &&
                   f_sp == 450.5f && f_d == 0.3f && f_h == 2.0f && f_rule == AUTOTUNE_RULE_ZIEGLER_NICHOLS,
               "explicit relay params passed through, zn");
    reset();
    f_run_result = false;
    f_run_err = "too hot";
    TEST_CHECK(post_ok(autotune_start_post_handler, "zone=1&method=relay&setpoint_c=500") == 400 && strstr(s_reply, "too hot") != NULL,
               "relay engine refusal relayed");
}

static void test_abort_accept(void)
{
    TEST_SECTION("abort / accept");
    reset();
    TEST_CHECK(post_ok(autotune_abort_post_handler, "") == 200 && f_abort_calls == 1, "abort -> engine abort once");
    f_facts.recovery_mode = true;
    f_danger = true;
    TEST_CHECK(post_ok(autotune_abort_post_handler, "") == 200 && f_abort_calls == 2, "abort never gated");
    reset();
    f_accept_result = false;
    TEST_CHECK(post(autotune_accept_post_handler, "", 0, (size_t)-1, 0) == 400 && f_accept_calls == 1, "accept refused by engine -> 400");
    TEST_CHECK(!f_acc_ack && !f_acc_adopt, "no body: opts zero");
    f_accept_result = true;
    f_acc_out.adoption = AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED;
    TEST_CHECK(post(autotune_accept_post_handler, "", 0, (size_t)-1, 0) == 200 && strstr(s_reply, "SKIPPED_NOT_REQUESTED") != NULL,
               "accept ok default outcome");
    post_ok(autotune_accept_post_handler, "ack_unsettled=1");
    TEST_CHECK(f_acc_ack && !f_acc_adopt, "ack_unsettled=1");
    post_ok(autotune_accept_post_handler, "ack_unsettled=true&adopt_ceiling=true");
    TEST_CHECK(f_acc_ack && f_acc_adopt, "'true' accepted for both flags");
    post_ok(autotune_accept_post_handler, "ack_unsettled=0&adopt_ceiling=yes");
    TEST_CHECK(!f_acc_ack && !f_acc_adopt, "0 / yes do not opt in");
    post_ok(autotune_accept_post_handler, "adopt_ceiling=1");
    TEST_CHECK(!f_acc_ack && f_acc_adopt, "adopt only");
    {
        char big[120];
        memset(big, 'a', sizeof(big));
        big[119] = 0;
        memcpy(big, "ack_unsettled=1&", 16);
        post(autotune_accept_post_handler, big, 119, (size_t)-1, 0);
        TEST_CHECK(!f_acc_ack, "body >= 96 B not parsed (defaults)");
    }
    post(autotune_accept_post_handler, "ack_unsettled=1", 15, 3, -1);
    TEST_CHECK(!f_acc_ack, "recv failure: defaults, never opt in");
    f_accept_result = false;
    f_acc_out.refused_by_mode_gate = true;
    snprintf(f_acc_out.mode_reason, sizeof(f_acc_out.mode_reason), "firing active");
    f_mode_refusals = 0;
    TEST_CHECK(post_ok(autotune_accept_post_handler, "ack_unsettled=1") == 409 && f_mode_refusals == 1 &&
                   strstr(s_reply, "firing active") != NULL,
               "mode-gate refusal -> 409 with gate text");
    struct { autotune_ceiling_adoption_t a; const char *n; } oc[] = {
        { AUTOTUNE_CEILING_ADOPTED, "ADOPTED" },
        { AUTOTUNE_CEILING_SKIPPED_RELAY_METHOD, "SKIPPED_RELAY_METHOD" },
        { AUTOTUNE_CEILING_SKIPPED_MODEL_NOT_PERSISTED, "SKIPPED_MODEL_NOT_PERSISTED" },
        { AUTOTUNE_CEILING_SKIPPED_ZERO, "SKIPPED_ZERO" },
        { AUTOTUNE_CEILING_SKIPPED_WOULD_TIGHTEN, "SKIPPED_WOULD_TIGHTEN" },
        { AUTOTUNE_CEILING_REJECTED_OUT_OF_RANGE, "REJECTED_OUT_OF_RANGE" },
        { AUTOTUNE_CEILING_SKIPPED_READ_FAILED, "SKIPPED_READ_FAILED" },
        { AUTOTUNE_CEILING_FAILED_TO_PERSIST, "FAILED_TO_PERSIST" },
        { AUTOTUNE_CEILING_REFUSED_NOT_WRITTEN, "REFUSED_NOT_WRITTEN" },
    };
    for (size_t i = 0; i < sizeof(oc) / sizeof(oc[0]); i++) {
        f_accept_result = true;
        memset(&f_acc_out, 0, sizeof(f_acc_out));
        f_acc_out.adoption = oc[i].a;
        f_acc_out.old_ceiling_c_per_hr = 100.0f;
        f_acc_out.new_ceiling_c_per_hr = 1000.0f;
        char m[96];
        post_ok(autotune_accept_post_handler, "adopt_ceiling=1");
        snprintf(m, sizeof(m), "outcome %s named, ceilings printed untruncated", oc[i].n);
        TEST_CHECK(strstr(s_reply, oc[i].n) != NULL && strstr(s_reply, "\"ceiling_new_c_per_hr\":1000.0}") != NULL &&
                       strstr(s_reply, "\"ceiling_old_c_per_hr\":100.0") != NULL,
                   m);
    }
}

int main(void)
{
    test_start_gates();
    test_start_step();
    test_start_relay();
    test_abort_accept();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
