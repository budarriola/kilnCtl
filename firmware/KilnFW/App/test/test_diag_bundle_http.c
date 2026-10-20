// Host test for App/drivers/http/diag_bundle_http.c (FD-01 producers + httpd glue; review DB-09b).
// The handler and producers are static, so this file #includes diag_bundle_http.c directly (same
// convention as test_diagnostics_http.c) and fakes the dependency surface. diag_bundle.c, boot_history.c,
// trip_history.c and fake_kv are linked for real.
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

#include "fake_kv.h"

// ---------------------------------------------------------------------------
// httpd fakes: capture every chunk
// ---------------------------------------------------------------------------
#define BODY_MAX 65536
static char s_body[BODY_MAX];
static size_t s_body_len;
static size_t s_chunk_len[256];
static int s_chunk_calls;
static int s_chunk_fail_at;
static bool s_term_chunk;
static char s_query[128];
static bool s_query_present;
static int s_status;

static void resp_reset(void)
{
    s_body_len = 0;
    s_chunk_calls = 0;
    s_chunk_fail_at = 0;
    s_term_chunk = false;
    s_query_present = false;
    s_query[0] = '\0';
    s_status = 200;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    (void)req;
    (void)msg;
    s_status = (int)error;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) { (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *f, const char *v) { (void)req; (void)f; (void)v; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, size_t len)
{
    (void)req;
    s_chunk_calls++;
    if (s_chunk_fail_at && s_chunk_calls == s_chunk_fail_at) {
        return ESP_FAIL;
    }
    if (buf == NULL || len == 0) {
        s_term_chunk = true;
        return ESP_OK;
    }
    if (s_chunk_calls < 256) {
        s_chunk_len[s_chunk_calls] = (size_t)len;
    }
    if (s_body_len + (size_t)len < BODY_MAX) {
        memcpy(s_body + s_body_len, buf, (size_t)len);
        s_body_len += (size_t)len;
    }
    return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    if (!s_query_present) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(buf, buf_len, "%s", s_query);
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    size_t kl = strlen(key);
    const char *p = qry;
    while (*p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = strcspn(v, "&");
            if (vl >= val_size) {
                return ESP_ERR_HTTPD_RESULT_TRUNC;
            }
            memcpy(val, v, vl);
            val[vl] = '\0';
            return ESP_OK;
        }
        p += strcspn(p, "&");
        if (*p == '&') {
            p++;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// module fakes (real headers first so a signature drift fails to compile)
// ---------------------------------------------------------------------------
#include "boot_guard.h"
#include "crash_report.h"
#include "hal_sysinfo.h"
#include "log_store.h"
#include "ota_record.h"

static bool g_core_present;
static uint32_t g_core_len;
static hal_status_t g_core_read_st;
hal_status_t hal_sysinfo_coredump_get_info(hal_sysinfo_coredump_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->present = g_core_present;
    out->data_len = g_core_len;
    return HAL_OK;
}
hal_status_t hal_sysinfo_coredump_read(uint32_t off, void *buf, uint32_t len)
{
    if (g_core_read_st != HAL_OK) {
        return g_core_read_st;
    }
    for (uint32_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)((off + i) * 7u);
    }
    return HAL_OK;
}
void hal_sysinfo_get_build_info(hal_sysinfo_build_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->version, sizeof(out->version), "v9.9-test");
}
hal_status_t hal_sysinfo_get_running_partition(hal_sysinfo_partition_info_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->label, sizeof(out->label), "ota_0");
    return HAL_OK;
}
bool boot_guard_get_persisted_count(uint32_t *c) { *c = 3; return true; }
uint32_t boot_guard_get_boot_count(void) { return 4; }
bool boot_guard_is_recovery_mode(void) { return false; }
esp_err_t ota_record_load(ota_record_t *r) { (void)r; return ESP_ERR_NOT_FOUND; }

static bool g_cr_present;
static crash_report_record_t g_cr_rec;
bool crash_report_get(crash_report_record_t *out)
{
    if (g_cr_present) {
        *out = g_cr_rec;
    }
    return g_cr_present;
}

/* log_store: N records of g_log_rec_len bytes for the firing log, none for autotune. */
static int g_log_records;
static size_t g_log_rec_len = 32;
struct log_store_reader {
    int left;
};
static struct log_store_reader s_rd;
log_store_reader_t *log_store_reader_open(log_store_kind_t kind)
{
    s_rd.left = (kind == LOG_STORE_KIND_FIRING) ? g_log_records : 0;
    return &s_rd;
}
bool log_store_reader_next(log_store_reader_t *rd, void *out, size_t cap, size_t *out_len)
{
    (void)cap;
    if (rd->left <= 0) {
        return false;
    }
    memset(out, 0x40 + (g_log_records - rd->left) % 16, g_log_rec_len);
    *out_len = g_log_rec_len;
    rd->left--;
    return true;
}
void log_store_reader_close(log_store_reader_t *rd) { (void)rd; }

bool uart_bridge_ext_is_on_flash_worker(void) { return false; }
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *), void *arg)
{
    fn(arg);
    return ESP_OK;
}

#include "../drivers/http/diag_bundle_http.c"

// ---------------------------------------------------------------------------
// frame parser over the captured body
// ---------------------------------------------------------------------------
typedef struct {
    char name[40];
    size_t off, len;
} fr_t;
static fr_t s_fr[128];
static int s_nfr;

static bool parse_frames(void)
{
    s_nfr = 0;
    size_t pos = 0;
    while (pos < s_body_len && s_nfr < 128) {
        if (strncmp(s_body + pos, "KDF1 ", 5) != 0) {
            return false;
        }
        char *nl = memchr(s_body + pos, '\n', s_body_len - pos);
        if (nl == NULL) {
            return false;
        }
        fr_t *f = &s_fr[s_nfr];
        char head[96];
        size_t hl = (size_t)(nl - (s_body + pos));
        if (hl >= sizeof(head)) {
            return false;
        }
        memcpy(head, s_body + pos, hl);
        head[hl] = '\0';
        unsigned long len = 0;
        if (sscanf(head, "KDF1 %39s %lu", f->name, &len) != 2) {
            return false;
        }
        f->off = (size_t)(nl - s_body) + 1;
        f->len = len;
        if (f->off + len + 1 > s_body_len || s_body[f->off + len] != '\n') {
            return false;
        }
        pos = f->off + len + 1;
        s_nfr++;
    }
    return pos == s_body_len;
}

static int count_frames(const char *name)
{
    int n = 0;
    for (int i = 0; i < s_nfr; i++) {
        n += strcmp(s_fr[i].name, name) == 0;
    }
    return n;
}

static int first_frame(const char *name)
{
    for (int i = 0; i < s_nfr; i++) {
        if (strcmp(s_fr[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool frame_contains(const char *name, const char *needle)
{
    for (int i = 0; i < s_nfr; i++) {
        if (strcmp(s_fr[i].name, name) != 0) {
            continue;
        }
        char *tmp = malloc(s_fr[i].len + 1);
        memcpy(tmp, s_body + s_fr[i].off, s_fr[i].len);
        tmp[s_fr[i].len] = '\0';
        bool hit = strstr(tmp, needle) != NULL;
        free(tmp);
        if (hit) {
            return true;
        }
    }
    return false;
}

static esp_err_t run_get(const char *query)
{
    resp_reset();
    if (query) {
        s_query_present = true;
        snprintf(s_query, sizeof(s_query), "%s", query);
    }
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    return diag_bundle_get_handler(&req);
}

static void test_default_bundle(void)
{
    TEST_SECTION("diag_bundle_http: default bundle, coredump opt-in");
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    boot_history_record_boot(4, true, 345, 0xDEADu, true, "x-gfeedbeef", 1700000000u);
    g_core_present = true;
    g_core_len = 5000;
    g_core_read_st = HAL_OK;
    g_cr_present = true;
    memset(&g_cr_rec, 0, sizeof(g_cr_rec));
    g_cr_rec.exc_a0 = 0xA0A0A0A0u;
    g_cr_rec.exc_a1 = 0xB1B1B1B1u;
    snprintf(g_cr_rec.reset_reason, sizeof(g_cr_rec.reset_reason), "PANIC");
    g_log_records = 0;

    TEST_CHECK(run_get(NULL) == ESP_OK, "handler ok");
    TEST_CHECK(s_term_chunk, "terminating chunk sent");
    TEST_CHECK(parse_frames(), "body parses as complete frames");
    TEST_CHECK(s_nfr >= 3 && strcmp(s_fr[0].name, "_manifest") == 0, "manifest first");
    TEST_CHECK(strcmp(s_fr[s_nfr - 1].name, "_trailer") == 0, "trailer last");
    TEST_CHECK(frame_contains("_manifest", "\"coredump_included\":false"), "manifest says coredump not included");
    TEST_CHECK(first_frame("coredump") < 0, "coredump left out by default");
    TEST_CHECK(frame_contains("_manifest", "/api/crash_report"), "pc_supplement lists /api/crash_report");
    TEST_CHECK(frame_contains("_trailer", "\"complete\":true"), "trailer complete");
    TEST_CHECK(first_frame("boot_history") > 0 && first_frame("trip_history") > 0, "history sections present");
    TEST_CHECK(frame_contains("boot_history", "\"dump_id\":57005"), "boot_history entry serialized");
    TEST_CHECK(frame_contains("trip_history", "\"dropped\":0"), "trip_history serialized");
    TEST_CHECK(frame_contains("crash_report", "\"reset_reason\":\"PANIC\""), "crash_report has reset_reason");
    TEST_CHECK(frame_contains("crash_report", "\"exc_a0\":\"0xa0a0a0a0\""), "crash_report has exc_a0");
    TEST_CHECK(frame_contains("crash_report", "\"exc_a1\":\"0xb1b1b1b1\""), "crash_report has exc_a1");
    TEST_CHECK(frame_contains("build", "\"version\":\"v9.9-test\""), "build section");

    /* every chunk but the last is exactly the 1 KB sink size */
    bool sized = s_chunk_calls > 2;
    for (int i = 1; i < s_chunk_calls - 1 && i < 255; i++) {
        sized = sized && (s_chunk_len[i] == DIAG_BUF_BYTES);
    }
    TEST_CHECK(sized || s_chunk_calls <= 2, "http_sink flushes full 1 KB chunks");

    /* opt-in coredump: offsets, chunking, last partial chunk */
    TEST_CHECK(run_get("coredump=1") == ESP_OK && parse_frames(), "coredump=1 ok");
    TEST_CHECK(frame_contains("_manifest", "\"coredump_included\":true"), "manifest records coredump_included");
    TEST_CHECK(frame_contains("_manifest", "Wi-Fi password"), "manifest carries the warning");
    TEST_CHECK(count_frames("coredump") == 3, "5000 B dump is 2048+2048+904");
    int c = first_frame("coredump");
    TEST_CHECK(c >= 0 && s_fr[c].len == 2048 && s_fr[c + 1].len == 2048 && s_fr[c + 2].len == 904, "core chunk sizes");
    bool bytes_ok = c >= 0;
    size_t off = 0;
    for (int k = 0; k < 3 && c >= 0; k++) {
        for (size_t i = 0; i < s_fr[c + k].len; i++, off++) {
            bytes_ok = bytes_ok && ((uint8_t)s_body[s_fr[c + k].off + i] == (uint8_t)(off * 7u));
        }
    }
    TEST_CHECK(bytes_ok && off == 5000, "coredump bytes match their partition offsets");
    TEST_CHECK(frame_contains("_trailer", "\"name\":\"coredump\",\"ok\":true,\"err\":0,\"bytes\":5000"),
               "trailer counts coredump bytes");
    TEST_CHECK(run_get("coredump=0") == ESP_OK && parse_frames() && first_frame("coredump") < 0, "coredump=0 excluded");
    TEST_CHECK(run_get("x=1&coredump=1") == ESP_OK && parse_frames() && first_frame("coredump") >= 0,
               "coredump=1 among other params");

    g_core_read_st = HAL_IO;
    TEST_CHECK(run_get("coredump=1") == ESP_OK && parse_frames(), "a failed core read still ends the bundle");
    TEST_CHECK(frame_contains("_trailer", "\"name\":\"coredump\",\"ok\":false"), "trailer records the producer failure");
    g_core_read_st = HAL_OK;
}

static void test_log_coalescing(void)
{
    TEST_SECTION("diag_bundle_http: log coalescing at the 1 KB boundary");
    g_core_present = false;
    g_log_rec_len = 32;
    g_log_records = 32; /* exactly 1024 B */
    TEST_CHECK(run_get(NULL) == ESP_OK && parse_frames(), "ok (32 records)");
    TEST_CHECK(count_frames("firing_log") == 1 && s_fr[first_frame("firing_log")].len == 1024, "1024 B in one frame");
    g_log_records = 33; /* 1056 B: flush at the boundary */
    TEST_CHECK(run_get(NULL) == ESP_OK && parse_frames(), "ok (33 records)");
    int f = first_frame("firing_log");
    TEST_CHECK(count_frames("firing_log") == 2 && s_fr[f].len == 1024 && s_fr[f + 1].len == 32, "1024 + 32");
    g_log_records = 40;
    g_log_rec_len = 64;
    TEST_CHECK(run_get(NULL) == ESP_OK && parse_frames(), "ok (64 B records)");
    f = first_frame("firing_log");
    TEST_CHECK(count_frames("firing_log") == 3 && s_fr[f].len == 1024 && s_fr[f + 1].len == 1024 &&
                   s_fr[f + 2].len == 512,
               "64 B records coalesce to 1024+1024+512");
    TEST_CHECK(first_frame("autotune_log") < 0, "empty log emits no frames");
    g_log_records = 0;
    g_log_rec_len = 32;
}

static void test_abort_path(void)
{
    TEST_SECTION("diag_bundle_http: transport failure aborts without a terminating chunk");
    g_log_records = 100;
    for (int at = 1; at <= 4; at++) {
        resp_reset();
        s_chunk_fail_at = at;
        httpd_req_t req;
        memset(&req, 0, sizeof(req));
        esp_err_t e = diag_bundle_get_handler(&req);
        char msg[64];
        snprintf(msg, sizeof(msg), "chunk %d failing returns ESP_FAIL", at);
        TEST_CHECK(e == ESP_FAIL, msg);
        TEST_CHECK(!s_term_chunk, "no terminating chunk after a send failure");
    }
    g_log_records = 0;
}

static void test_alloc_failure(void)
{
    TEST_SECTION("diag_bundle_http: out of memory");
    resp_reset();
    heap_caps_malloc_test_set_fail(true);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t e = diag_bundle_get_handler(&req);
    heap_caps_malloc_test_set_fail(false);
    TEST_CHECK(e == ESP_FAIL && s_status == 500, "sink buffer allocation failure answers 500");
}

int main(void)
{
    test_default_bundle();
    test_log_coalescing();
    test_abort_path();
    test_alloc_failure();
    printf("test_diag_bundle_http: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
