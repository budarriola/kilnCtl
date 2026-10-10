// Host test for App/drivers/http/setup_progress_http.c (round 3, R3-B):
// GET/POST /api/setup/progress. The POST is a persisted-config writer
// (setup wizard progress); the handler owns the cfg-mount refusal, body
// bounds, step/state validation, and the "failed save is never reported as
// ok" mapping. #includes the real .c. The progress store itself
// (setup_wizard_progress.c, covered by test_setup_wizard_progress.c) is
// faked: get_all / set_step are scripted and recorded.
//
// Pins: unmounted cfg refuses 503 before the body is read or the store is
// touched; missing/oversize body 400; step range [0, STEP_COUNT) with strict
// integer parse; unknown state 400; note rejection 400; store failure ->
// 500 (mounted) / 503 (unmounted), never {"ok":true}; success passes exactly
// the parsed step/state/note (empty note -> NULL); GET JSON shape, escaping
// of quotes/backslashes in notes, and malloc-failure -> 500.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "../drivers/persist/setup_wizard_progress.h"

httpd_handle_t wifi_provision_http_get_server(void) { return (httpd_handle_t)1; }
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server; (void)uri;
    return ESP_OK;
}

/* cfg_fs.h seams */
static bool s_cfg_available = true;
static bool s_recovery = false;
bool cfg_fs_is_available(void) { return s_cfg_available; }
bool cfg_fs_skipped_for_recovery(void) { return s_recovery; }

/* progress store fakes */
static setup_wizard_step_t s_steps[SETUP_WIZARD_STEP_COUNT];
static esp_err_t s_set_result = ESP_OK;
static int s_set_calls = 0;
static uint8_t s_set_step = 255;
static setup_wizard_step_state_t s_set_state = SETUP_WIZ_STEP_PENDING;
static bool s_set_note_null = false;
static char s_set_note[SETUP_WIZARD_NOTE_MAX];
static bool s_note_form_ok = true;
static bool s_unmount_on_set = false;
esp_err_t setup_wizard_progress_start(void) { return ESP_OK; }
void setup_wizard_progress_get_all(setup_wizard_step_t out[SETUP_WIZARD_STEP_COUNT])
{
    memcpy(out, s_steps, sizeof(s_steps));
}
bool setup_wizard_progress_note_from_form(const char *body, char *out, size_t out_cap)
{
    if (!s_note_form_ok) return false;
    out[0] = '\0';
    const char *p = strstr(body, "note=");
    if (p) snprintf(out, out_cap, "%s", p + 5);
    return true;
}
esp_err_t setup_wizard_progress_set_step(uint8_t step_index, setup_wizard_step_state_t state, const char *note)
{
    s_set_calls++;
    if (s_unmount_on_set) s_cfg_available = false;
    s_set_step = step_index;
    s_set_state = state;
    s_set_note_null = (note == NULL);
    snprintf(s_set_note, sizeof(s_set_note), "%s", note ? note : "");
    return s_set_result;
}

/* httpd leaf calls */
static int s_status_code = 200;
static char s_body[4096];
static int s_err_calls = 0;
static int s_err_code = 0;
static const char *s_recv_body = "";
static int s_recv_fail = 0;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (s_recv_fail) return -1;
    size_t n = strlen(s_recv_body);
    if (n > buf_len) n = buf_len;
    memcpy(buf, s_recv_body, n);
    return (int)n;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    s_status_code = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *t) { (void)req; (void)t; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    (void)req;
    snprintf(s_body, sizeof(s_body), "%s", s ? s : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req;
    snprintf(s_body, sizeof(s_body), "%.*s", (int)len, buf ? buf : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t e, const char *msg)
{
    (void)req;
    s_err_calls++;
    s_err_code = (int)e;
    snprintf(s_body, sizeof(s_body), "%s", msg ? msg : "");
    return ESP_OK;
}

/* malloc failure injection for the GET handler's scratch */
static int s_fail_malloc = 0;
static void *test_malloc(size_t n) { return s_fail_malloc ? NULL : malloc(n); }
#define malloc(n) test_malloc(n)

#include "../drivers/http/setup_progress_http.c"

#undef malloc

static void reset(void)
{
    s_cfg_available = true;
    s_recovery = false;
    memset(s_steps, 0, sizeof(s_steps));
    s_set_result = ESP_OK;
    s_set_calls = 0;
    s_set_step = 255;
    s_set_state = SETUP_WIZ_STEP_PENDING;
    s_set_note_null = false;
    s_set_note[0] = '\0';
    s_note_form_ok = true;
    s_unmount_on_set = false;
    s_status_code = 200;
    s_body[0] = '\0';
    s_err_calls = 0;
    s_err_code = 0;
    s_recv_body = "";
    s_recv_fail = 0;
    s_fail_malloc = 0;
}

static void post(const char *body)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_recv_body = body;
    req.content_len = (long long)strlen(body);
    TEST_CHECK(api_setup_progress_post_handler(&req) == ESP_OK, "POST always returns ESP_OK");
}

static void test_unmounted(void)
{
    TEST_SECTION("POST -- unmounted cfg refuses 503 before reading the body or touching the store");
    reset();
    s_cfg_available = false;
    post("step=1&state=done");
    TEST_CHECK(s_status_code == 503, "503");
    TEST_CHECK(s_set_calls == 0, "store untouched");
    TEST_CHECK(strstr(s_body, "\"ok\":false") != NULL, "body says ok:false");
}

static void test_body_bounds(void)
{
    TEST_SECTION("POST -- body presence and size bounds");
    reset();
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = 0;
    api_setup_progress_post_handler(&req);
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST && s_set_calls == 0, "empty body: 400");
    reset();
    req.content_len = SETUP_PROGRESS_BODY_MAX + 1;
    api_setup_progress_post_handler(&req);
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST && s_set_calls == 0, "oversize body: 400");
    reset();
    s_recv_fail = 1;
    s_recv_body = "step=1&state=done";
    req.content_len = (long long)strlen(s_recv_body);
    api_setup_progress_post_handler(&req);
    TEST_CHECK(s_err_calls == 1 && s_set_calls == 0, "recv failure: 400, store untouched");
}

static void test_validation(void)
{
    TEST_SECTION("POST -- step/state validation never reaches the store");
    static const char *const bad[] = {
        "state=done",                 /* missing step */
        "step=1",                     /* missing state */
        "step=&state=done",
        "step=12&state=done",         /* == STEP_COUNT */
        "step=-1&state=done",
        "step=1x&state=done",
        "step=%20&state=done",
        "step=1&state=finished",
        "step=1&state=DONE",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset();
        post(bad[i]);
        TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST, "400");
        TEST_CHECK(s_set_calls == 0, "store untouched");
    }
    reset();
    s_note_form_ok = false;
    post("step=1&state=skipped&note=x");
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST && s_set_calls == 0,
               "note rejected by the form helper: 400, store untouched");
}

static void test_success_and_failure(void)
{
    TEST_SECTION("POST -- success passes exact args; store failure is never ok");
    reset();
    post("step=11&state=skipped&note=no%20ct");
    TEST_CHECK(s_set_calls == 1 && s_set_step == 11 && s_set_state == SETUP_WIZ_STEP_SKIPPED, "last step, skipped");
    TEST_CHECK(!s_set_note_null, "note passed through");
    TEST_CHECK(strcmp(s_body, "{\"ok\":true}") == 0 && s_status_code == 200, "ok:true");
    reset();
    post("step=0&state=done");
    TEST_CHECK(s_set_calls == 1 && s_set_step == 0 && s_set_state == SETUP_WIZ_STEP_DONE, "step 0, done");
    TEST_CHECK(s_set_note_null, "absent note -> NULL");
    reset();
    s_set_result = ESP_FAIL;
    post("step=2&state=pending");
    TEST_CHECK(s_set_calls == 1, "store attempted");
    TEST_CHECK(s_status_code == 500, "mounted store failure: 500");
    TEST_CHECK(strstr(s_body, "\"ok\":true") == NULL && strstr(s_body, "\"ok\":false") != NULL,
               "failure never claims ok");
    reset();
    s_set_result = ESP_FAIL;
    s_unmount_on_set = true;
    post("step=2&state=pending");
    TEST_CHECK(s_status_code == 503, "store failure after cfg dropped out: 503, not 500/ok");
    TEST_CHECK(strstr(s_body, "\"ok\":false") != NULL, "still ok:false");
}

static void test_get(void)
{
    TEST_SECTION("GET -- JSON shape, escaping, and allocation failure");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    reset();
    s_steps[0].state = SETUP_WIZ_STEP_DONE;
    s_steps[0].ts = 77;
    s_steps[1].state = SETUP_WIZ_STEP_SKIPPED;
    s_steps[1].ts = 4294967295u;
    snprintf(s_steps[1].note, sizeof(s_steps[1].note), "a\"b\\c");
    TEST_CHECK(api_setup_progress_get_handler(&req) == ESP_OK, "ok");
    static const char kPrefix[] = "{\"version\":1,\"steps\":{\"0\":{\"state\":\"done\",\"ts\":77,\"note\":\"\"},";
    TEST_CHECK(strncmp(s_body, kPrefix, sizeof(kPrefix) - 1) == 0,
               "prefix and step 0");
    TEST_CHECK(strstr(s_body, "\"1\":{\"state\":\"skipped\",\"ts\":4294967295,\"note\":\"a\\\"b\\\\c\"}") != NULL,
               "step 1 with escaped note and max ts");
    TEST_CHECK(strstr(s_body, "\"11\":{\"state\":\"pending\"") != NULL, "last step present");
    size_t len = strlen(s_body);
    TEST_CHECK(len > 2 && strcmp(s_body + len - 2, "}}") == 0, "closed document");
    reset();
    s_fail_malloc = 1;
    api_setup_progress_get_handler(&req);
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failure: 500");
    reset();
    /* worst-case notes on every row must still fit and stay valid */
    for (int i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        s_steps[i].state = SETUP_WIZ_STEP_SKIPPED;
        s_steps[i].ts = 4294967295u;
        memset(s_steps[i].note, '"', SETUP_WIZARD_NOTE_MAX - 1);
        s_steps[i].note[SETUP_WIZARD_NOTE_MAX - 1] = '\0';
    }
    api_setup_progress_get_handler(&req);
    TEST_CHECK(s_err_calls == 0, "worst-case document fits");
    len = strlen(s_body);
    TEST_CHECK(len > 2 && strcmp(s_body + len - 2, "}}") == 0, "worst-case document is complete");
}

int main(void)
{
    test_unmounted();
    test_body_bounds();
    test_validation();
    test_success_and_failure();
    test_get();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
