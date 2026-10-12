// Host-test coverage (c78b): GET /api/autotune, /api/autotune/matrix, /api/autotune/trace.csv
// from the REAL dashboard_autotune_http.c, with the REAL dashboard_json.c formatter. The engine
// (status/matrix/RGA/trace), zone count and the httpd transport are fakes. The route tier
// (auth) is enforced by the route table, not in this module, so it is not asserted here;
// the handlers take no query string, so request extras must not change the reply.
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

/* ---- engine fakes ---- */
static autotune_engine_status_t f_status;
void autotune_engine_get_status(autotune_engine_status_t *out) { *out = f_status; }
static autotune_coupling_matrix_t f_matrix;
void autotune_engine_get_coupling_matrix(autotune_coupling_matrix_t *out) { *out = f_matrix; }
static autotune_rga_t f_rga;
void autotune_engine_compute_rga(const autotune_coupling_matrix_t *m, autotune_rga_t *out) { (void)m; *out = f_rga; }
static uint8_t f_zone_count;
uint8_t zones_config_get_thermo_count(void) { return f_zone_count; }
static size_t f_trace_total;
static int f_trace_calls;
size_t autotune_engine_get_trace(autotune_sample_t *out, size_t start, size_t max)
{
    f_trace_calls++;
    size_t n = 0;
    while (n < max && start + n < f_trace_total) {
        out[n].t_s = (float)(start + n);
        out[n].measurement_c = 20.0f + (float)(start + n) * 0.5f;
        n++;
    }
    return n;
}

/* ---- transport ---- */
static int s_status;
static char s_type[32], s_hdr[96];
static char s_reply[16384];
static size_t s_reply_len;
static int s_chunks, s_chunk_terminators, s_fail_chunk_at;
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; snprintf(s_type, sizeof(s_type), "%s", t); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v) { (void)r; (void)f; snprintf(s_hdr, sizeof(s_hdr), "%s", v); return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n)
{
    (void)r;
    s_reply_len = (size_t)snprintf(s_reply, sizeof(s_reply), "%.*s", (int)n, b);
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
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, size_t n)
{
    (void)r;
    s_chunks++;
    if (s_fail_chunk_at > 0 && s_chunks == s_fail_chunk_at) return ESP_FAIL;
    if (b == NULL || n == 0) { s_chunk_terminators++; return ESP_OK; }
    if (s_reply_len + n < sizeof(s_reply)) {
        memcpy(s_reply + s_reply_len, b, n);
        s_reply_len += n;
        s_reply[s_reply_len] = 0;
    }
    return ESP_OK;
}

static void get(esp_err_t (*h)(httpd_req_t *))
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_status = 200; s_reply[0] = 0; s_reply_len = 0; s_type[0] = 0; s_hdr[0] = 0;
    s_chunks = s_chunk_terminators = 0;
    (void)h(&req);
}
static void reset(void)
{
    memset(&f_status, 0, sizeof(f_status));
    memset(&f_matrix, 0, sizeof(f_matrix));
    memset(&f_rga, 0, sizeof(f_rga));
    f_zone_count = 3; f_trace_total = 0; f_trace_calls = 0; s_fail_chunk_at = 0;
    heap_caps_malloc_test_set_fail(false);
}

static void test_status(void)
{
    TEST_SECTION("GET /api/autotune: idle, active, aborted, OOM");
    reset();
    f_status.state = AUTOTUNE_ENGINE_IDLE;
    get(autotune_status_get_handler);
    TEST_CHECK(s_status == 200 && strstr(s_type, "json") != NULL, "idle: 200 JSON");
    TEST_CHECK(strstr(s_reply, "\"state\":\"idle\"") != NULL, "idle state named");
    TEST_CHECK(strstr(s_reply, "\"refusal\":\"ok\"") != NULL && strstr(s_reply, "\"refusal_reason\":\"\"") != NULL,
               "idle: no stale refusal");
    TEST_CHECK(s_reply[0] == '{' && s_reply[s_reply_len - 1] == '}', "reply is one JSON object, not truncated");
    reset();
    f_status.state = AUTOTUNE_ENGINE_STEPPING;
    f_status.zone_index = 2;
    get(autotune_status_get_handler);
    TEST_CHECK(strstr(s_reply, "\"state\":\"stepping\"") != NULL && strstr(s_reply, "\"zone\":2") != NULL,
               "stepping on zone 2 reported");
    reset();
    f_status.state = AUTOTUNE_ENGINE_ABORTED;
    snprintf(f_status.abort_reason, sizeof(f_status.abort_reason), "sensor \"lost\"");
    get(autotune_status_get_handler);
    TEST_CHECK(strstr(s_reply, "\"state\":\"aborted\"") != NULL, "aborted state");
    TEST_CHECK(strstr(s_reply, "sensor \\\"lost\\\"") != NULL, "abort reason is JSON-escaped");
    reset();
    f_status.state = AUTOTUNE_ENGINE_DONE;
    f_status.model.valid = true;
    f_status.proposed_gains.refusal = AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL;
    get(autotune_status_get_handler);
    TEST_CHECK(strstr(s_reply, "\"state\":\"done\"") != NULL && strstr(s_reply, "\"model_valid\":true") != NULL &&
                   strstr(s_reply, "dead_time_too_small") != NULL,
               "done: model flag and refusal name present");
    reset();
    heap_caps_malloc_test_set_fail(true);
    get(autotune_status_get_handler);
    TEST_CHECK(s_status == 500 && strstr(s_reply, "\"ok\":false") != NULL, "response-buffer OOM -> 500 JSON, no crash");
}

static void test_matrix(void)
{
    TEST_SECTION("GET /api/autotune/matrix");
    reset();
    f_zone_count = 3;
    f_matrix.cell[0][0].valid = true;
    f_matrix.cell[0][0].model.k_gain_c_per_duty = 12.5f;
    f_matrix.cell[0][0].model.tau_s = 300.0f;
    f_matrix.cell[0][0].model.dead_time_s = 20.0f;
    f_rga.valid = false;
    f_rga.status = (autotune_rga_status_t)1;
    snprintf(f_rga.invalid_reason, sizeof(f_rga.invalid_reason), "no 2 zones measured");
    get(autotune_matrix_get_handler);
    TEST_CHECK(s_status == 200 && strstr(s_reply, "\"zone_count\":3") != NULL, "200, zone_count 3");
    TEST_CHECK(strstr(s_reply, "{\"i\":0,\"j\":0,\"valid\":true,\"k\":12.500,\"tau_s\":300.0,\"dead_time_s\":20.0}") != NULL,
               "valid cell printed with model");
    TEST_CHECK(strstr(s_reply, "{\"i\":2,\"j\":2,\"valid\":false}") != NULL, "unmeasured cell valid:false");
    TEST_CHECK(strstr(s_reply, "\"available\":false") != NULL && strstr(s_reply, "no 2 zones measured") != NULL,
               "RGA unavailable with the engine's reason");
    reset();
    f_zone_count = 1;
    get(autotune_matrix_get_handler);
    TEST_CHECK(strstr(s_reply, "\"zone_count\":1") != NULL && strstr(s_reply, "{\"i\":0,\"j\":1") == NULL,
               "one declared zone: only a 1x1 grid, not the hardware channel count");
    TEST_CHECK(strstr(s_reply, "fewer than 2 zones") != NULL, "one zone: RGA reason says fewer than 2 zones");
    reset();
    f_zone_count = 2;
    f_rga.valid = true; f_rga.n = 2; f_rga.determinant = 4.0f;
    f_rga.zone_index[0] = 0; f_rga.zone_index[1] = 1;
    f_rga.lambda[0][0] = 1.25f; f_rga.lambda[0][1] = -0.25f; f_rga.lambda[1][0] = -0.25f; f_rga.lambda[1][1] = 1.25f;
    get(autotune_matrix_get_handler);
    TEST_CHECK(strstr(s_reply, "\"available\":true,\"n\":2") != NULL && strstr(s_reply, "\"zones\":[0,1]") != NULL &&
                   strstr(s_reply, "[1.2500,-0.2500]") != NULL,
               "available RGA: n, zone map and lambda rows");
    TEST_CHECK(s_reply[s_reply_len - 1] == '}', "matrix reply closed");
    reset();
    heap_caps_malloc_test_set_fail(true);
    get(autotune_matrix_get_handler);
    TEST_CHECK(s_status == 500 && strstr(s_reply, "\"ok\":false") != NULL, "matrix OOM -> 500 JSON");
}

static int count_lines(void)
{
    int lines = 0;
    for (const char *p = s_reply; *p; p++) if (*p == '\n') lines++;
    return lines;
}

static void test_trace(void)
{
    TEST_SECTION("GET /api/autotune/trace.csv");
    reset();
    f_trace_total = 0;
    get(autotune_trace_csv_get_handler);
    TEST_CHECK(strstr(s_type, "csv") != NULL && strstr(s_hdr, "autotune_trace.csv") != NULL, "CSV type + filename header");
    TEST_CHECK(strcmp(s_reply, "elapsed_s,measurement_c\n") == 0 && s_chunk_terminators == 1,
               "idle/empty: header only, stream terminated");
    reset();
    f_trace_total = 3;
    get(autotune_trace_csv_get_handler);
    TEST_CHECK(strcmp(s_reply, "elapsed_s,measurement_c\n0.0,20.00\n1.0,20.50\n2.0,21.00\n") == 0, "three rows in order");
    reset();
    f_trace_total = 300; /* 2 full batches of 128 + 44 */
    get(autotune_trace_csv_get_handler);
    TEST_CHECK(count_lines() == 301 && f_trace_calls == 3, "300 samples across batch boundaries: no row lost or repeated");
    TEST_CHECK(strstr(s_reply, "299.0,169.50\n") != NULL, "last row present");
    reset();
    f_trace_total = 128; /* exact batch multiple */
    get(autotune_trace_csv_get_handler);
    TEST_CHECK(count_lines() == 129 && s_chunk_terminators == 1, "exact 128: all rows, single terminator");
    reset();
    f_trace_total = 10;
    s_fail_chunk_at = 3;
    get(autotune_trace_csv_get_handler);
    TEST_CHECK(s_chunk_terminators == 0 && f_trace_calls == 1, "client drop mid-stream: stops, no terminator");
    reset();
    f_trace_total = 1;
    {
        httpd_req_t req;
        memset(&req, 0, sizeof(req));
        req.content_len = 50;
        s_status = 200; s_reply[0] = 0; s_reply_len = 0; s_chunks = s_chunk_terminators = 0;
        (void)autotune_trace_csv_get_handler(&req);
    }
    TEST_CHECK(s_status == 200 && count_lines() == 2, "unexpected request body ignored (no query parsing here)");
}

int main(void)
{
    test_status();
    test_matrix();
    test_trace();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
