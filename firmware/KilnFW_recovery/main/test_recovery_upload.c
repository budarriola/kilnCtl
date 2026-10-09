// test_recovery_upload.c -- host test for recovery_upload.c's streaming loop:
// length gate, first-chunk gate, short body, recv timeouts, allocation floor,
// sink begin/write/finish failures, and the ESP sink's esp_ota_* usage (an
// esp_ota_end failure must leave the boot target alone). The ESP-IDF calls are
// stubbed (host_stubs/host_esp_stub.h) and scripted from here. Built and run
// by check_recovery_upload.ps1 (MSVC). Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_upload.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

// ---- stub state ------------------------------------------------------------
const host_recv_step_t *g_recv_script;
size_t g_recv_script_len;
const uint8_t *g_body;
size_t g_body_len;
size_t g_body_pos;
bool g_spiram_fail;
size_t g_internal_free;
int g_ota_begin_calls, g_ota_write_calls, g_ota_end_calls, g_ota_abort_calls;
int g_ota_set_boot_calls;
int g_ota_write_fail_at;
bool g_ota_begin_fail, g_ota_end_fail;
size_t g_ota_bytes_written;
int g_status_set_calls;
char g_last_status[64];
char g_last_hdr_field[32], g_last_hdr_value[32];
int g_send_calls;
static size_t s_script_pos;
int64_t g_now_us;           // esp_timer_get_time() stub
int64_t g_recv_advance_us;  // clock advance per httpd_req_recv() call (slow-drip simulation)
int64_t esp_timer_get_time(void) { return g_now_us; }

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    (void)r;
    size_t cap = len;
    g_now_us += g_recv_advance_us;
    if (g_recv_script) {
        if (s_script_pos >= g_recv_script_len) {
            return -1; // script over: connection closed
        }
        host_recv_step_t st = g_recv_script[s_script_pos++];
        if (st.kind == 1) {
            return HTTPD_SOCK_ERR_TIMEOUT;
        }
        if (st.kind == 2) {
            return -1;
        }
        if (st.max_bytes && cap > st.max_bytes) {
            cap = st.max_bytes;
        }
    }
    size_t left = g_body_len - g_body_pos;
    if (left == 0) {
        return 0; // peer closed
    }
    if (cap > left) {
        cap = left;
    }
    memcpy(buf, g_body + g_body_pos, cap);
    g_body_pos += cap;
    return (int)cap;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    g_status_set_calls++;
    strncpy(g_last_status, status, sizeof(g_last_status) - 1);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v)
{
    (void)r;
    strncpy(g_last_hdr_field, f, sizeof(g_last_hdr_field) - 1);
    strncpy(g_last_hdr_value, v, sizeof(g_last_hdr_value) - 1);
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, int l)
{
    (void)r;
    (void)b;
    (void)l;
    g_send_calls++;
    return ESP_OK;
}

esp_err_t esp_ota_begin(const esp_partition_t *p, size_t sz, esp_ota_handle_t *h)
{
    (void)p;
    (void)sz;
    g_ota_begin_calls++;
    if (g_ota_begin_fail) {
        return ESP_FAIL;
    }
    *h = 77;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n)
{
    (void)h;
    (void)d;
    g_ota_write_calls++;
    if (g_ota_write_fail_at && g_ota_write_calls == g_ota_write_fail_at) {
        return ESP_FAIL;
    }
    g_ota_bytes_written += n;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h)
{
    (void)h;
    g_ota_end_calls++;
    return g_ota_end_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h)
{
    (void)h;
    g_ota_abort_calls++;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    (void)p;
    g_ota_set_boot_calls++;
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if ((caps & MALLOC_CAP_SPIRAM) && g_spiram_fail) {
        return NULL;
    }
    return malloc(size);
}
size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return g_internal_free;
}
esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}

// ---- fake validator and sink -----------------------------------------------
static int v_calls, v_calls_nonnull;
static size_t v_first_len, v_total, v_max;
static ric_result_t v_result_gate, v_result_first;

static ric_result_t fake_validate(const uint8_t *first, size_t len, size_t content_len, size_t max_len,
                                  void *vctx)
{
    (void)vctx;
    v_calls++;
    v_total = content_len;
    v_max = max_len;
    if (!first) {
        return v_result_gate;
    }
    v_calls_nonnull++;
    v_first_len = len;
    return v_result_first;
}

static int k_begin, k_write, k_finish, k_abort;
static bool k_fail_begin, k_fail_finish;
static int k_fail_write_at;
static uint8_t k_data[65536];
static size_t k_len;

static bool k_begin_fn(void *c, size_t n)
{
    (void)c;
    (void)n;
    k_begin++;
    return !k_fail_begin;
}
static bool k_write_fn(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    k_write++;
    if (k_fail_write_at && k_write == k_fail_write_at) {
        return false;
    }
    memcpy(k_data + k_len, d, n);
    k_len += n;
    return true;
}
static bool k_finish_fn(void *c)
{
    (void)c;
    k_finish++;
    return !k_fail_finish;
}
static void k_abort_fn(void *c)
{
    (void)c;
    k_abort++;
}

static uint8_t s_body[65536];

static void reset_all(size_t body_len)
{
    g_recv_script = NULL;
    g_recv_script_len = 0;
    s_script_pos = 0;
    g_now_us = 1000000;
    g_recv_advance_us = 0;
    for (size_t i = 0; i < sizeof(s_body); i++) {
        s_body[i] = (uint8_t)(i * 31u + 7u);
    }
    g_body = s_body;
    g_body_len = body_len;
    g_body_pos = 0;
    g_spiram_fail = false;
    g_internal_free = 1u << 20;
    g_ota_begin_calls = g_ota_write_calls = g_ota_end_calls = g_ota_abort_calls = 0;
    g_ota_set_boot_calls = 0;
    g_ota_write_fail_at = 0;
    g_ota_begin_fail = g_ota_end_fail = false;
    g_ota_bytes_written = 0;
    g_status_set_calls = g_send_calls = 0;
    g_last_status[0] = g_last_hdr_field[0] = g_last_hdr_value[0] = 0;
    v_calls = v_calls_nonnull = 0;
    v_first_len = v_total = v_max = 0;
    v_result_gate = RIC_NO_LENGTH;
    v_result_first = RIC_OK;
    k_begin = k_write = k_finish = k_abort = 0;
    k_fail_begin = k_fail_finish = false;
    k_fail_write_at = 0;
    k_len = 0;
}

static const recovery_sink_t k_sink = {k_begin_fn, k_write_fn, k_finish_fn, k_abort_fn, NULL};
static const recovery_upload_cfg_t k_cfg = {20000u, fake_validate, NULL};

static recovery_upload_result_t run(size_t content_len, int *status, const char **msg)
{
    httpd_req_t req;
    req.content_len = content_len;
    *status = 0;
    *msg = NULL;
    return recovery_upload_stream(&req, &k_cfg, &k_sink, status, msg);
}

// ---- tests -----------------------------------------------------------------
static void test_happy_paths(void)
{
    int st;
    const char *msg;
    reset_all(10000);
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_OK, "10000-byte body uploads");
    CHECK(k_begin == 1 && k_finish == 1 && k_abort == 0, "begin once, finish once, no abort");
    CHECK(k_len == 10000 && memcmp(k_data, s_body, 10000) == 0, "sink received the exact body, in order");
    CHECK(k_write == 3, "10000 bytes = 4096 + 4096 + 1808 chunks");
    CHECK(v_calls_nonnull == 1 && v_first_len == RECOVERY_UPLOAD_CHUNK && v_total == 10000 && v_max == 20000u,
          "validator saw exactly the first chunk, the length and the limit");

    reset_all(100);
    CHECK(run(100, &st, &msg) == RECOVERY_UPLOAD_OK && k_len == 100 && k_write == 1 && v_first_len == 100,
          "a body smaller than one chunk is a single write");

    reset_all(8192);
    CHECK(run(8192, &st, &msg) == RECOVERY_UPLOAD_OK && k_write == 2 && k_len == 8192,
          "an exact multiple of the chunk size ends cleanly");
}

static void test_gates(void)
{
    int st;
    const char *msg;
    reset_all(10000);
    CHECK(run(0, &st, &msg) == RECOVERY_UPLOAD_REJECTED, "zero content length rejected");
    CHECK(st == 400 && k_begin == 0 && g_body_pos == 0 && v_calls_nonnull == 0,
          "zero length: 400, nothing read, nothing written");

    reset_all(30000);
    v_result_gate = RIC_OVERSIZE;
    CHECK(run(30000, &st, &msg) == RECOVERY_UPLOAD_REJECTED && st == 413, "oversize: 413");
    CHECK(k_begin == 0 && g_body_pos == 0, "oversize: nothing read, nothing written");

    reset_all(10000);
    v_result_first = RIC_WRONG_PROJECT;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_REJECTED && st == 400, "bad first chunk rejected");
    CHECK(k_begin == 0 && k_write == 0 && k_finish == 0, "bad first chunk: sink never started");
}

static void test_alloc(void)
{
    int st;
    const char *msg;
    reset_all(100);
    g_spiram_fail = true;
    g_internal_free = RECOVERY_UPLOAD_CHUNK + RECOVERY_INTERNAL_FLOOR_BYTES - 1;
    CHECK(run(100, &st, &msg) == RECOVERY_UPLOAD_NO_MEMORY && st == 503, "internal RAM below floor: 503");
    CHECK(k_begin == 0 && g_body_pos == 0, "no memory: nothing read or written");
    reset_all(100);
    g_spiram_fail = true;
    g_internal_free = RECOVERY_UPLOAD_CHUNK + RECOVERY_INTERNAL_FLOOR_BYTES;
    CHECK(run(100, &st, &msg) == RECOVERY_UPLOAD_OK, "internal RAM exactly at the floor: allowed");
}

static void test_sink_failures(void)
{
    int st;
    const char *msg;
    reset_all(10000);
    k_fail_begin = true;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_SINK_ERROR && st == 500, "begin failure: 500");
    CHECK(k_write == 0 && k_abort == 0 && k_finish == 0, "begin failure: no write, no abort, no finish");

    reset_all(10000);
    k_fail_write_at = 2;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_SINK_ERROR && st == 500, "write failure: 500");
    CHECK(k_abort == 1 && k_finish == 0, "write failure: aborted once, never finished");

    reset_all(10000);
    k_fail_finish = true;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_VERIFY_ERROR && st == 422, "finish failure: 422");
    CHECK(k_abort == 0, "finish failure: no abort (finish already released the sink)");
}

static void test_short_body_and_timeouts(void)
{
    int st;
    const char *msg;
    // Short body: declared 10000, peer closes after 6000.
    reset_all(6000);
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && st == 400, "short body: 400");
    CHECK(k_abort == 1 && k_finish == 0, "short body mid-image: aborted once, never finished");

    // Short inside the first chunk: nothing may reach the validator or sink.
    reset_all(2000);
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && st == 400, "short first chunk: 400");
    CHECK(v_calls_nonnull == 0 && k_begin == 0, "short first chunk: never validated, never written");

    // Three consecutive timeouts are tolerated.
    static const host_recv_step_t t3[] = {{1, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}};
    reset_all(6000);
    g_recv_script = t3;
    g_recv_script_len = 5;
    CHECK(run(6000, &st, &msg) == RECOVERY_UPLOAD_OK && k_len == 6000, "3 consecutive timeouts tolerated");

    // A fourth consecutive timeout abandons the upload.
    static const host_recv_step_t t4[] = {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}};
    reset_all(6000);
    g_recv_script = t4;
    g_recv_script_len = 6;
    CHECK(run(6000, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && st == 400, "4 consecutive timeouts abandon");
    CHECK(k_begin == 0, "timeout before the first chunk: nothing written");

    // The timeout count resets on every successful read.
    static const host_recv_step_t treset[] = {{1, 0}, {1, 0}, {1, 0}, {0, 1000}, {1, 0}, {1, 0},
                                              {1, 0}, {0, 1000}, {1, 0}, {1, 0}, {1, 0}, {0, 1000}};
    reset_all(3000);
    g_recv_script = treset;
    g_recv_script_len = 12;
    CHECK(run(3000, &st, &msg) == RECOVERY_UPLOAD_OK && k_len == 3000,
          "timeouts separated by data never accumulate");

    // Timeout storm after the first chunk: abort.
    static const host_recv_step_t tmid[] = {{0, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {0, 0}};
    reset_all(8192);
    g_recv_script = tmid;
    g_recv_script_len = 6;
    CHECK(run(8192, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && st == 400, "mid-image timeout storm: 400");
    CHECK(k_abort == 1 && k_finish == 0 && k_write == 1, "mid-image timeout storm: one chunk written, aborted");

    // A hard recv error is not retried.
    static const host_recv_step_t herr[] = {{0, 0}, {2, 0}, {0, 0}};
    reset_all(8192);
    g_recv_script = herr;
    g_recv_script_len = 3;
    CHECK(run(8192, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && k_abort == 1, "hard recv error aborts");
}

static void test_overall_deadline(void)
{
    int st;
    const char *msg;
    // Budget for a 10000-byte upload is 60 s + 10000/2048 s ~= 64.9 s. A
    // client that drips 1 byte per call with 10 s of clock per call (each
    // call "succeeds", so the timeout-retry budget never trips) must be cut off.
    static host_recv_step_t drip[64];
    for (size_t i = 0; i < 64; i++) {
        drip[i].kind = 0;
        drip[i].max_bytes = 1;
    }
    reset_all(10000);
    g_recv_script = drip;
    g_recv_script_len = 64;
    g_recv_advance_us = 10000000LL;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR && st == 400, "slow-drip first chunk: cut off by the overall deadline");
    CHECK(k_begin == 0, "slow-drip first chunk: nothing written");
    CHECK(s_script_pos <= 8, "slow-drip first chunk: stopped reading once the deadline passed (not by script end)");

    // Fast client with a large clock budget is unaffected.
    reset_all(10000);
    g_recv_advance_us = 1000;
    CHECK(run(10000, &st, &msg) == RECOVERY_UPLOAD_OK, "normal-speed upload is not cut off");
    CHECK(recovery_upload_budget_us(0) == 60000000LL, "budget base is 60 s");
    CHECK(recovery_upload_budget_us(2048) == 61000000LL, "2 KB adds 1 s");
}

static void test_esp_sink(void)
{
    int st;
    const char *msg;
    esp_partition_t part = {1};
    recovery_sink_t sink;
    recovery_esp_sink_state_t es;
    httpd_req_t req;

    // Happy path.
    reset_all(10000);
    recovery_upload_esp_sink_init(&sink, &es, &part);
    req.content_len = 10000;
    CHECK(recovery_upload_stream(&req, &k_cfg, &sink, &st, &msg) == RECOVERY_UPLOAD_OK, "esp sink: uploads");
    CHECK(g_ota_begin_calls == 1 && g_ota_end_calls == 1 && g_ota_abort_calls == 0, "esp sink: begin/end once");
    CHECK(g_ota_bytes_written == 10000, "esp sink: every byte reached esp_ota_write");
    CHECK(g_ota_set_boot_calls == 0, "upload never sets the boot partition itself");

    // esp_ota_end fails: the boot target must stay untouched and the handle must
    // not be aborted a second time.
    reset_all(10000);
    g_ota_end_fail = true;
    recovery_upload_esp_sink_init(&sink, &es, &part);
    CHECK(recovery_upload_stream(&req, &k_cfg, &sink, &st, &msg) == RECOVERY_UPLOAD_VERIFY_ERROR && st == 422,
          "esp_ota_end failure: 422");
    CHECK(g_ota_set_boot_calls == 0, "esp_ota_end failure: boot target unchanged");
    CHECK(!es.begun, "esp_ota_end failure: handle marked released");
    sink.abort(sink.ctx);
    CHECK(g_ota_abort_calls == 0, "esp_ota_end failure: a later abort does not double-release the handle");

    // esp_ota_write fails mid-image: abort exactly once, never end.
    reset_all(10000);
    g_ota_write_fail_at = 2;
    recovery_upload_esp_sink_init(&sink, &es, &part);
    CHECK(recovery_upload_stream(&req, &k_cfg, &sink, &st, &msg) == RECOVERY_UPLOAD_SINK_ERROR && st == 500,
          "esp_ota_write failure: 500");
    CHECK(g_ota_abort_calls == 1 && g_ota_end_calls == 0 && g_ota_set_boot_calls == 0,
          "esp_ota_write failure: aborted, not ended, boot target unchanged");
    sink.abort(sink.ctx);
    CHECK(g_ota_abort_calls == 1, "abort is idempotent");

    // esp_ota_begin fails.
    reset_all(10000);
    g_ota_begin_fail = true;
    recovery_upload_esp_sink_init(&sink, &es, &part);
    CHECK(recovery_upload_stream(&req, &k_cfg, &sink, &st, &msg) == RECOVERY_UPLOAD_SINK_ERROR, "esp_ota_begin failure");
    CHECK(!es.begun && g_ota_write_calls == 0 && g_ota_abort_calls == 0, "begin failure: nothing to release");

    // Connection lost mid-image: abort, boot target unchanged.
    reset_all(6000);
    recovery_upload_esp_sink_init(&sink, &es, &part);
    req.content_len = 10000;
    CHECK(recovery_upload_stream(&req, &k_cfg, &sink, &st, &msg) == RECOVERY_UPLOAD_READ_ERROR, "esp: short body");
    CHECK(g_ota_abort_calls == 1 && g_ota_end_calls == 0 && g_ota_set_boot_calls == 0,
          "esp: short body aborts the OTA, boot target unchanged");
}

static void test_send_error(void)
{
    httpd_req_t req;
    req.content_len = 0;
    struct {
        int code;
        const char *want;
    } t[] = {{400, "400 Bad Request"},
             {413, "413 Payload Too Large"},
             {422, "422 Unprocessable Entity"},
             {503, "503 Service Unavailable"},
             {500, "500 Internal Server Error"},
             {418, "500 Internal Server Error"}};
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        reset_all(0);
        esp_err_t e = recovery_upload_send_error(&req, t[i].code, "x");
        CHECK(strcmp(g_last_status, t[i].want) == 0, t[i].want);
        CHECK(e == ESP_FAIL && g_send_calls == 1, "send_error returns ESP_FAIL after sending once");
        CHECK(strcmp(g_last_hdr_field, "Connection") == 0 && strcmp(g_last_hdr_value, "close") == 0,
              "send_error asks for Connection: close");
    }
}

int main(void)
{
    test_happy_paths();
    test_gates();
    test_alloc();
    test_sink_failures();
    test_short_body_and_timeouts();
    test_overall_deadline();
    test_esp_sink();
    test_send_error();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
