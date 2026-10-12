// Campaign 8: handler-level matrix for POST /api/profile_exec/{start,stop,pause,resume}
// and the ack_last_run route, compiled from the REAL dashboard_exec_http.c with the
// real system_mode_gate.c. Executor, readiness facts, danger_mode, run_state are fakes
// that COUNT calls, so every refusal is asserted as "executor never invoked" (module
// state), not just by the reply status.
//
// Observations recorded in docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md (K8-xx).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#define __attribute__(x)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void *heap_caps_calloc(size_t c, size_t n, int caps) { (void)caps; return calloc(c, n); }
static void heap_caps_free(void *p) { free(p); }
#include "../drivers/http/dashboard_exec_http.c"

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

static int f_run_calls, f_halt_calls, f_pause_calls, f_resume_calls, f_ack_calls;
static uint8_t f_run_id;
static bool f_run_result = true, f_pause_result = true, f_resume_result = true, f_ack_result = true;
static const char *f_run_err = "";
bool profile_executor_run(uint8_t id, char *err, size_t cap)
{
    f_run_calls++;
    f_run_id = id;
    if (!f_run_result) {
        snprintf(err, cap, "%s", f_run_err);
    }
    return f_run_result;
}
void profile_executor_halt(void) { f_halt_calls++; }
bool profile_executor_pause(void) { f_pause_calls++; return f_pause_result; }
bool profile_executor_resume(void) { f_resume_calls++; return f_resume_result; }
bool run_state_acknowledge(void) { f_ack_calls++; return f_ack_result; }

/* ---- TEST_TRIP link fakes ---- */
static int f_tt_calls;
static esp_err_t f_tt_err;
static safety_link_test_trip_outcome_t f_tt_outcome;
static uint8_t f_tt_seq;
static bool f_tt_recovery;
static bool f_tt_peer_known;
static uint16_t f_tt_peer;
esp_err_t safety_link_send_test_trip(SafetyLinkClass *link, safety_link_test_trip_outcome_t *out_outcome,
                                     uint8_t *out_trip_seq)
{
    (void)link;
    f_tt_calls++;
    *out_outcome = f_tt_outcome;
    *out_trip_seq = f_tt_seq;
    return f_tt_err;
}
static safety_link_status_t f_tt_status;
static esp_err_t f_tt_status_err;
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (f_tt_status_err == ESP_OK) {
        *out = f_tt_status;
    }
    return f_tt_status_err;
}
bool boot_guard_is_recovery_mode(void) { return f_tt_recovery; }
esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *known, bool *a, uint16_t *proto, uint16_t *b)
{
    (void)link; (void)a; (void)b;
    *known = f_tt_peer_known;
    *proto = f_tt_peer;
    return ESP_OK;
}

static bool f_danger;
bool danger_mode_active(void) { return f_danger; }
bool danger_mode_blocks_start(void) { return f_danger; }
bool watchdog_cfg_panic_disabled(void) { return false; }

/* ---- transport ---- */
static int s_status;
static char s_reply[512];
static int s_sends;
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n)
{
    (void)r;
    s_sends++;
    snprintf(s_reply, sizeof(s_reply), "%.*s", (int)n, b);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send(r, s, (ssize_t)strlen(s)); }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m)
{
    (void)r;
    s_sends++;
    s_status = (int)e;
    snprintf(s_reply, sizeof(s_reply), "%s", m ? m : "");
    return ESP_OK;
}

static const char *s_body;
static size_t s_off, s_chunk, s_fail_after;
static int s_fail_ret;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    (void)r;
    size_t blen = s_body ? strlen(s_body) : 0;
    if (s_off >= s_fail_after) {
        return s_fail_ret;
    }
    if (s_off >= blen) {
        return 0;
    }
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
    s_body = body;
    s_off = 0;
    s_chunk = 2;
    s_fail_after = fail_after;
    s_fail_ret = fail_ret;
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
    memset(&f_facts, 0, sizeof(f_facts));
    f_run_calls = f_halt_calls = f_pause_calls = f_resume_calls = f_ack_calls = 0;
    f_run_result = f_pause_result = f_resume_result = f_ack_result = true;
    f_run_err = "";
    f_danger = false;
    f_run_id = 0xEE;
}

static void test_start_gates(void)
{
    TEST_SECTION("start: recovery-mode gate, readiness gate, danger-mode: executor never invoked");
    reset();
    f_facts.recovery_mode = true;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=1") == 409, "recovery mode -> 409");
    TEST_CHECK(strstr(s_reply, "\"ok\":false") != NULL, "refusal is JSON");
    TEST_CHECK(f_run_calls == 0, "recovery refusal: executor never started");
    reset();
    f_facts.crash_have_record = true; /* unacknowledged crash blocks */
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=1") == 409 && f_run_calls == 0,
               "unacknowledged crash -> 409, executor not started");
    reset();
    f_facts.safety_link_up = false;
    f_facts.safety_trip_mask = 0x0020;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=1") == 409 && f_run_calls == 0,
               "safety trip -> 409, executor not started");
    reset();
    f_facts.safety_link_up = true;
    f_facts.estop_verified = true;
    {
        char m[192];
        TEST_CHECK(readiness_gate_evaluate(&f_facts, m, sizeof(m)) == READINESS_GATE_OK, "setup: readiness passes");
    }
    f_danger = true;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=1") == 409 && f_run_calls == 0,
               "danger mode active: 409, executor not started");
    TEST_CHECK(strstr(s_reply, "danger mode") != NULL, "refusal names danger mode");
    f_danger = false;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=1") == 200 && f_run_calls == 1, "control: same facts start when danger off");
}

static void test_start_body(void)
{
    TEST_SECTION("start: body truncation, recv errors, bad ids, duplicate keys");
    reset();
    /* make the readiness gate pass: find a fact set that evaluates OK */
    char msg[192];
    readiness_gate_facts_t ok = f_facts;
    ok.safety_link_up = true;
    ok.estop_verified = true;
    ok.pico_update_blocked = false;
    ok.ct_attribution = (readiness_ct_attribution_fact_t)0;
    bool passes = readiness_gate_evaluate(&ok, msg, sizeof(msg)) == READINESS_GATE_OK;
    if (!passes) {
        ok.safety_link_up = true;
        passes = readiness_gate_evaluate(&ok, msg, sizeof(msg)) == READINESS_GATE_OK;
    }
    TEST_CHECK(passes, "found a readiness fact set that passes the gate (test setup)");
    f_facts = ok;
    const char *bad[] = { "", "id", "id=", "id=abc", "id=-1", "id=256", "id=1.5", "id=99999999999", "x=1", "id=1x", "id=%00" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int st = bad[i][0] ? post_ok(profile_exec_start_post_handler, bad[i])
                           : post(profile_exec_start_post_handler, bad[i], 0, (size_t)-1, 0);
        char m[96];
        snprintf(m, sizeof(m), "body [%s] -> 400, executor not started", bad[i]);
        TEST_CHECK(st == 400 && f_run_calls == 0, m);
    }
    TEST_CHECK(post(profile_exec_start_post_handler, "id=1", 33, (size_t)-1, 0) == 400 && f_run_calls == 0,
               "content_len 33 (> cap) -> 400");
    TEST_CHECK(post(profile_exec_start_post_handler, "id=1", -1, (size_t)-1, 0) == 400 && f_run_calls == 0,
               "content_len -1 -> 400");
    TEST_CHECK(post(profile_exec_start_post_handler, "id=1", 12, (size_t)-1, 0) == 400 && f_run_calls == 0,
               "declared longer than delivered (truncated) -> 400");
    TEST_CHECK(post(profile_exec_start_post_handler, "id=1", 4, 2, 0) == 400 && f_run_calls == 0, "peer closes mid-body -> 400");
    TEST_CHECK(post(profile_exec_start_post_handler, "id=1", 4, 2, -1) == 400 && f_run_calls == 0, "recv error -> 400");
    /* short declared length drops the trailing digit: id=1 vs id=12 */
    TEST_CHECK(post(profile_exec_start_post_handler, "id=12", 4, (size_t)-1, 0) == 200 && f_run_calls == 1 && f_run_id == 1,
               "declared length bounds the parse: only 'id=1' is read");
    reset();
    f_facts = ok;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=3&id=4") == 200 && f_run_calls == 1 && f_run_id == 3,
               "duplicate id: first occurrence wins, started once");
    reset();
    f_facts = ok;
    f_run_result = false;
    f_run_err = "no such profile";
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=200") == 400 && f_run_calls == 1, "executor refusal -> 400");
    TEST_CHECK(strstr(s_reply, "no such profile") != NULL, "executor's reason is relayed");
    reset();
    f_facts = ok;
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=0") == 200 && f_run_id == 0, "id 0 accepted");
    TEST_CHECK(post_ok(profile_exec_start_post_handler, "id=255") == 200 && f_run_id == 255, "id 255 accepted");
}

static void test_stop_pause_resume(void)
{
    TEST_SECTION("stop / pause / resume / ack");
    reset();
    f_pause_result = false;
    TEST_CHECK(post_ok(profile_exec_pause_post_handler, "") == 400 && f_pause_calls == 1, "pause with nothing running -> 400");
    f_resume_result = false;
    TEST_CHECK(post_ok(profile_exec_resume_post_handler, "") == 400 && f_resume_calls == 1, "resume with nothing paused -> 400");
    f_pause_result = true;
    TEST_CHECK(post_ok(profile_exec_pause_post_handler, "") == 200 && f_pause_calls == 2, "pause ok -> 200");
    TEST_CHECK(post_ok(profile_exec_stop_post_handler, "") == 200 && f_halt_calls == 1, "stop halts exactly once");
    TEST_CHECK(f_run_calls == 0, "none of these started a run");
    f_ack_result = false;
    TEST_CHECK(post_ok(profile_exec_ack_last_run_post_handler, "") == 400 && f_ack_calls == 1, "ack with nothing -> 400");
    /* stop must work in recovery mode and with gates failing: it is the safe direction */
    f_facts.recovery_mode = true;
    f_danger = true;
    TEST_CHECK(post_ok(profile_exec_stop_post_handler, "") == 200 && f_halt_calls == 2, "stop is never gated");
}

static int tt_post(const char *body, esp_err_t err, safety_link_test_trip_outcome_t outcome, uint8_t seq)
{
    static int dummy_link;
    s_dash.safety = (SafetyLinkClass *)&dummy_link;
    f_tt_calls = 0;
    f_tt_err = err;
    f_tt_outcome = outcome;
    f_tt_seq = seq;
    return post_ok(safety_test_trip_post_handler, body);
}

static void test_safety_test_trip(void)
{
    TEST_SECTION("POST /api/safety/test_trip: confirm gate, recovery refusal, outcome-to-status mapping");
    reset();
    f_tt_recovery = false;
    f_tt_peer_known = true;
    f_tt_peer = 17u;
    TEST_CHECK(tt_post("", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 400 && f_tt_calls == 0,
               "no confirm -> 400, link never asked");
    TEST_CHECK(tt_post("confirm=0", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 400 && f_tt_calls == 0,
               "confirm=0 -> 400, link never asked");
    f_tt_recovery = true;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 409 && f_tt_calls == 0 &&
                   strstr(s_reply, "recovery_mode") != NULL, "recovery mode -> 409, link never asked");
    f_tt_recovery = false;
    /* WP3 review LOW-2: the 200 reply reports the live DIAG read-back, never constants posing as observed. */
    memset(&f_tt_status, 0, sizeof(f_tt_status));
    f_tt_status_err = ESP_OK;
    f_tt_status.link_up = true;
    f_tt_status.diag_ever_received = true;
    f_tt_status.diag_state = SAFETY_LINK_DIAG_STATE_TRIPPED;
    f_tt_status.diag_trip_reason = 4u;
    f_tt_status.diag_trip_mask = 8u;
    f_tt_status.diag_since_reboot = true;
    f_tt_status.diag_trip_seq_known = true;
    f_tt_status.diag_trip_seq = 3u;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 && f_tt_calls == 1 &&
                   strstr(s_reply, "\"trip_seq\":3") != NULL && strstr(s_reply, "\"readback\":true") != NULL &&
                   strstr(s_reply, "\"trip_reason\":4") != NULL && strstr(s_reply, "\"trip_mask\":8") != NULL &&
                   strstr(s_reply, "\"is_test\":true") != NULL, "accepted + DIAG read-back -> reason 4, mask 8, is_test");
    f_tt_status.diag_trip_reason = 2u; /* a real guard won the same tick */
    f_tt_status.diag_trip_mask = 2u;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"trip_reason\":2") != NULL && strstr(s_reply, "\"trip_mask\":2") != NULL &&
                   strstr(s_reply, "\"is_test\":false") != NULL && strstr(s_reply, "\"trip_reason\":4") == NULL,
               "real guard won -> reply reports ITS reason/mask and is_test false, never 4/8");
    f_tt_status.diag_trip_reason = 5u; /* an older trip's DIAG (different trip_seq) is not this trip's readback */
    f_tt_status.diag_trip_seq = 2u;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"readback\":false") != NULL, "INFO-A: DIAG with another trip_seq -> readback false");
    f_tt_status.diag_trip_seq = 3u;
    f_tt_status.diag_since_reboot = false;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"readback\":false") != NULL, "LOW-C: previous-boot DIAG -> readback false");
    f_tt_status.diag_since_reboot = true;
    f_tt_status.link_up = false;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"readback\":false") != NULL, "M4: link down -> readback false");
    f_tt_status.link_up = true;
    f_tt_status.diag_state = SAFETY_LINK_DIAG_STATE_ARMED; /* TRIPPED DIAG not seen yet */
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"readback\":false") != NULL &&
                   strstr(s_reply, "\"requested_trip_reason\":4") != NULL &&
                   strstr(s_reply, "\"trip_reason\"") == NULL, "no TRIPPED DIAG yet -> requested_* only, no observed claim");
    f_tt_status_err = ESP_FAIL;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_ACCEPTED, 3) == 200 &&
                   strstr(s_reply, "\"readback\":false") != NULL, "status read fails -> readback false");
    f_tt_status_err = ESP_OK;
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_DUPLICATE, 0) == 409 &&
                   strstr(s_reply, "\"duplicate\"") != NULL && strstr(s_reply, "\"ok\":false") != NULL,
               "duplicate -> 409 ok:false (LOW-4)");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_RATE_LIMIT, 0) == 409 &&
                   strstr(s_reply, "rate_limited") != NULL, "rate limit -> 409");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_ALREADY_TRIPPED, 0) == 409 &&
                   strstr(s_reply, "already_tripped") != NULL, "Pico already tripped -> 409");
    TEST_CHECK(tt_post("confirm=1", ESP_ERR_INVALID_STATE, SAFETY_LINK_TEST_TRIP_LOCAL_ALREADY_TRIPPED, 0) == 409 &&
                   strstr(s_reply, "already_tripped") != NULL, "local already tripped -> 409");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_BOOT_ID, 0) == 409 &&
                   strstr(s_reply, "boot_id_changed") != NULL, "boot id changed -> 409");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_UPDATING, 0) == 409 &&
                   strstr(s_reply, "updating") != NULL, "updating -> 409");
    TEST_CHECK(tt_post("confirm=1", ESP_ERR_NOT_SUPPORTED, SAFETY_LINK_TEST_TRIP_LOCAL_PEER_TOO_OLD, 0) == 501 &&
                   strstr(s_reply, "pico_protocol_too_old") != NULL && strstr(s_reply, "\"pico_protocol\":17") != NULL,
               "old Pico (local) -> 501 naming its protocol");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_PEER_VERSION, 0) == 501,
               "old Pico (Pico refused) -> 501");
    TEST_CHECK(tt_post("confirm=1", ESP_ERR_TIMEOUT, SAFETY_LINK_TEST_TRIP_NO_REPLY, 0) == 503 &&
                   strstr(s_reply, "no_reply") != NULL, "no reply -> 503");
    TEST_CHECK(tt_post("confirm=1", ESP_ERR_INVALID_STATE, SAFETY_LINK_TEST_TRIP_LOCAL_STALE, 0) == 503 &&
                   strstr(s_reply, "link_stale") != NULL, "stale link -> 503");
    TEST_CHECK(tt_post("confirm=1", ESP_OK, SAFETY_LINK_TEST_TRIP_REFUSED_BAD_FRAME, 0) == 502, "bad frame -> 502");
}

int main(void)
{
    test_safety_test_trip();
    test_start_gates();
    test_start_body();
    test_stop_pause_resume();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
