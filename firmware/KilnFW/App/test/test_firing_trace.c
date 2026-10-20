// Host tests for App/drivers/persist/firing_trace.c (FD-02): append, rate gate, batched flush,
// rotation over 4 slots, corrupt files, write failure, truncation at 24 h, power-cut loss bound,
// init restore and CSV rows. firing_trace.c is #included; CommonFW's CRC is linked separately.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/firing_trace.c"

/* ---- RAM "cfg_fs" ---------------------------------------------------------- */
typedef struct {
    char path[24];
    uint8_t *data;
    size_t len;
} ramfile_t;
static ramfile_t g_files[8];
static int g_write_calls;
static esp_err_t g_write_fail; /* ESP_OK = succeed */
static void (*g_pending)(void *);
static int g_post_calls;
static int g_run_calls;

static ramfile_t *rf_find(const char *p, int create)
{
    for (int i = 0; i < 8; i++) {
        if (g_files[i].path[0] && strcmp(g_files[i].path, p) == 0) return &g_files[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < 8; i++) {
        if (!g_files[i].path[0]) {
            snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", p);
            return &g_files[i];
        }
    }
    return NULL;
}

static esp_err_t t_write(const char *p, const void *d, size_t n)
{
    g_write_calls++;
    if (g_write_fail != ESP_OK) return g_write_fail;
    ramfile_t *f = rf_find(p, 1);
    free(f->data);
    f->data = malloc(n);
    memcpy(f->data, d, n);
    f->len = n;
    return ESP_OK;
}

static esp_err_t t_read(const char *p, void *buf, size_t cap, size_t *out)
{
    ramfile_t *f = rf_find(p, 0);
    if (!f) return ESP_ERR_NOT_FOUND;
    if (f->len > cap) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, f->data, f->len);
    if (out) *out = f->len;
    return ESP_OK;
}

static esp_err_t t_post(void (*fn)(void *))
{
    g_post_calls++;
    if (g_pending) return ESP_ERR_INVALID_STATE;
    g_pending = fn;
    return ESP_OK;
}

static esp_err_t t_run(void (*fn)(void *), void *a)
{
    g_run_calls++;
    fn(a);
    return ESP_OK;
}

static void run_pending(void)
{
    void (*fn)(void *) = g_pending;
    g_pending = NULL;
    if (fn) fn(NULL);
}

static void *t_alloc(size_t n)
{
    return malloc(n);
}

static const firing_trace_io_t k_io = {.write = t_write, .read = t_read, .post = t_post, .run = t_run, .alloc = t_alloc};

static void wipe_files(void)
{
    for (int i = 0; i < 8; i++) {
        free(g_files[i].data);
        memset(&g_files[i], 0, sizeof(g_files[i]));
    }
}

static void fresh(void)
{
    firing_trace_reset_for_test();
    wipe_files();
    g_write_calls = g_post_calls = g_run_calls = 0;
    g_write_fail = ESP_OK;
    g_pending = NULL;
    firing_trace_init(&k_io);
}

static uint32_t g_up;
static bool note(uint32_t token, float sp, float t0, float d0)
{
    float t[3] = {t0, NAN, 21.5f};
    float d[3] = {d0, NAN, 0.0f};
    g_up += 60;
    bool r = firing_trace_note(token, g_up, 1700000000u + g_up, sp, t, d, 2);
    firing_trace_service();
    run_pending();
    return r;
}

static size_t durable_n(uint32_t id)
{
    static uint8_t scratch[FT_FILE_MAX];
    firing_trace_view_t v;
    if (firing_trace_open(id, scratch, sizeof(scratch), &v) != ESP_OK) return (size_t)-1;
    return v.entry.n;
}

static void run_firing(uint32_t token, int samples)
{
    g_up = 0;
    for (int i = 0; i < samples; i++) note(token, 100.0f, 50.0f + i, 0.5f);
}

static void test_append_and_batch(void)
{
    TEST_SECTION("firing_trace: append, rate gate, batched flush");
    fresh();
    TEST_CHECK(note(7, 100.0f, 50.0f, 0.5f), "first sample stored");
    TEST_CHECK(g_write_calls == 1 && durable_n(1) == 1, "first sample flushes so the file exists");
    float t[3] = {1, 1, 1};
    TEST_CHECK(!firing_trace_note(7, g_up + 30, 0, 1.0f, t, t, 0), "sample < 60 s after the last is refused");
    for (int i = 0; i < 9; i++) note(7, 100.0f, 51.0f + i, 0.5f);
    TEST_CHECK(g_write_calls == 1, "9 more samples buffered, no write yet");
    note(7, 100.0f, 60.0f, 0.5f);
    TEST_CHECK(g_write_calls == 2 && durable_n(1) == 11, "flush when 10 are unflushed");
    firing_trace_entry_t le[FT_SLOTS];
    TEST_CHECK(firing_trace_list(le) == 1, "one firing listed");
}

static void test_end_and_loss(void)
{
    TEST_SECTION("firing_trace: end flushes, power-cut loss bounded by one batch");
    fresh();
    run_firing(9, 25); /* flushes at n=1, 11, 21 */
    size_t durable = durable_n(1);
    TEST_CHECK(durable == 21, "25 samples noted, 21 durable before any end");
    TEST_CHECK(25 - durable <= FT_FLUSH_EVERY, "worst-case power-cut loss <= 10 samples");
    firing_trace_end();
    TEST_CHECK(g_run_calls == 1 && durable_n(1) == 25, "end() flushes the tail synchronously");
    firing_trace_entry_t e[FT_SLOTS];
    firing_trace_list(e);
    TEST_CHECK((e[0].flags & FT_FLAG_CLOSED) != 0 && e[0].n == 25, "entry marked closed");
    TEST_CHECK(!note(9, 1.0f, 1.0f, 0.0f), "no new samples accepted for the ended run token");
    firing_trace_end();
    TEST_CHECK(g_run_calls == 1, "second end() is a no-op");
}

static void test_rotation(void)
{
    TEST_SECTION("firing_trace: rotation keeps the last 4 firings");
    fresh();
    for (uint32_t k = 1; k <= 5; k++) {
        run_firing(100 + k, 3);
        firing_trace_end();
    }
    firing_trace_entry_t e[FT_SLOTS];
    size_t n = firing_trace_list(e);
    TEST_CHECK(n == 4, "4 firings retained");
    TEST_CHECK(e[0].seq == 5 && e[3].seq == 2, "newest first: 5,4,3,2");
    TEST_CHECK(durable_n(1) == (size_t)-1, "firing 1 was overwritten by firing 5");
    TEST_CHECK(durable_n(5) == 3, "firing 5 readable");
    static uint8_t scratch[FT_FILE_MAX];
    firing_trace_view_t v;
    TEST_CHECK(firing_trace_open(1, scratch, sizeof(scratch), &v) == ESP_ERR_NOT_FOUND,
               "opening a rotated-out id reports NOT_FOUND (slot holds another firing)");
}

static void test_corrupt_and_restore(void)
{
    TEST_SECTION("firing_trace: corrupt file ignored, init restores the index");
    fresh();
    for (uint32_t k = 1; k <= 3; k++) {
        run_firing(200 + k, 4);
        firing_trace_end();
    }
    static uint8_t scratch[FT_FILE_MAX];
    firing_trace_view_t v;
    ramfile_t *f2 = rf_find("trace/ft2.bin", 0);
    TEST_CHECK(f2 != NULL, "firing 2 file exists");
    f2->data[FT_HDR_SIZE + 3] ^= 0x40; /* flip a sample bit */
    TEST_CHECK(firing_trace_open(2, scratch, sizeof(scratch), &v) == ESP_ERR_INVALID_CRC, "sample corruption caught");
    f2->data[FT_HDR_SIZE + 3] ^= 0x40;
    f2->data[9] ^= 0x01; /* header seq byte */
    TEST_CHECK(firing_trace_open(2, scratch, sizeof(scratch), &v) == ESP_ERR_INVALID_CRC, "header corruption caught");
    f2->data[9] ^= 0x01;
    ramfile_t *f1 = rf_find("trace/ft1.bin", 0);
    f1->len -= 5; /* torn / truncated file */
    firing_trace_reset_for_test();
    firing_trace_init(&k_io); /* "reboot" */
    firing_trace_entry_t e[FT_SLOTS];
    size_t n = firing_trace_list(e);
    TEST_CHECK(n == 2 && e[0].seq == 3 && e[1].seq == 2, "scan keeps valid files, drops the truncated one");
    run_firing(300, 2);
    firing_trace_end();
    TEST_CHECK(durable_n(4) == 2, "next firing gets seq 4 (max valid + 1) after a reboot");
}

static void test_write_failure(void)
{
    TEST_SECTION("firing_trace: failed write keeps samples in RAM and retries");
    fresh();
    g_write_fail = ESP_FAIL;
    run_firing(5, 12);
    TEST_CHECK(durable_n(1) == (size_t)-1, "nothing durable while writes fail");
    TEST_CHECK(g_write_calls >= 1, "flush was attempted");
    g_write_fail = ESP_OK;
    firing_trace_end();
    TEST_CHECK(durable_n(1) == 12, "end() later writes every buffered sample");
}

static void test_truncation(void)
{
    TEST_SECTION("firing_trace: 24 h cap");
    fresh();
    run_firing(11, FT_MAX_SAMPLES + 5);
    firing_trace_end();
    firing_trace_entry_t e[FT_SLOTS];
    firing_trace_list(e);
    TEST_CHECK(e[0].n == FT_MAX_SAMPLES, "capped at 1440 samples");
    TEST_CHECK((e[0].flags & FT_FLAG_TRUNCATED) != 0, "truncated flag set");
    TEST_CHECK(durable_n(1) == FT_MAX_SAMPLES, "file holds 1440 samples");
    TEST_CHECK(rf_find("trace/ft1.bin", 0)->len == FT_FILE_MAX, "file size is exactly 28,824 B");
}

static void test_post_busy(void)
{
    TEST_SECTION("firing_trace: service() never double-posts");
    fresh();
    g_up = 0;
    float t[3] = {1, 2, 3};
    firing_trace_note(1, 60, 0, 1.0f, t, t, 0);
    firing_trace_service();
    TEST_CHECK(g_post_calls == 1, "one post for a due flush");
    firing_trace_service();
    TEST_CHECK(g_post_calls == 1, "no second post while the first is outstanding");
    run_pending();
    TEST_CHECK(durable_n(1) == 1, "job ran");
    firing_trace_service();
    TEST_CHECK(g_post_calls == 1, "nothing due, no post");
}

static void test_csv(void)
{
    TEST_SECTION("firing_trace: CSV rows");
    fresh();
    g_up = 0;
    float t[3] = {-12.34f, NAN, 1234.5f};
    float d[3] = {0.5f, NAN, 1.0f};
    firing_trace_note(3, 120, 0u /* unsynced */, 999.9f, t, d, 7);
    firing_trace_end();
    static uint8_t scratch[FT_FILE_MAX];
    firing_trace_view_t v;
    TEST_CHECK(firing_trace_open(1, scratch, sizeof(scratch), &v) == ESP_OK, "open");
    char line[FT_CSV_LINE_CAP];
    size_t h = firing_trace_csv_header(line, sizeof(line));
    TEST_CHECK(h > 0 && strncmp(line, "uptime_s,utc_epoch,setpoint_c,", 30) == 0, "header");
    size_t o = firing_trace_csv_row(&v, 0, line, sizeof(line));
    TEST_CHECK(o > 0, "row formatted");
    TEST_CHECK(strcmp(line, "120,0,999.9,-12.3,,1234.5,50.0,,100.0,7\n") == 0, "row: epoch 0, inactive blank, values");
    TEST_CHECK(firing_trace_csv_row(&v, 1, line, sizeof(line)) == 0, "index past n refused");
}

int main(void)
{
    test_append_and_batch();
    test_end_and_loss();
    test_rotation();
    test_corrupt_and_restore();
    test_write_failure();
    test_truncation();
    test_post_busy();
    test_csv();
    firing_trace_reset_for_test();
    wipe_files();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
