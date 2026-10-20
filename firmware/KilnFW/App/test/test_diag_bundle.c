// Host tests for http/diag_bundle.c (FD-01 support bundle framing) plus the
// "never include secrets" source pin on diag_bundle_http.c.
#include <stdlib.h>
#include <string.h>

#include "diag_bundle.h"
#include "test_common.h"

typedef struct {
    char buf[8192];
    size_t len;
    int fail_after; /* fail the Nth sink call (1-based); 0 = never */
    int calls;
} cap_t;

static esp_err_t cap_sink(void *ctx, const void *d, size_t n)
{
    cap_t *c = (cap_t *)ctx;
    c->calls++;
    if (c->fail_after && c->calls >= c->fail_after) {
        return ESP_FAIL;
    }
    if (c->len + n < sizeof(c->buf)) {
        memcpy(c->buf + c->len, d, n);
        c->len += n;
        c->buf[c->len] = 0;
    }
    return ESP_OK;
}

static esp_err_t prod_ok(diag_out_t *o, const char *name)
{
    esp_err_t e = diag_frame_write(o, name, "abc", 3);
    return e == ESP_OK ? diag_frame_write(o, name, "de", 2) : e;
}
static esp_err_t prod_bad(diag_out_t *o, const char *name)
{
    (void)o;
    (void)name;
    return ESP_FAIL;
}

void run_test_diag_bundle(void)
{
    TEST_SECTION("diag_bundle");
    static const diag_section_t secs[] = {{"alpha", prod_ok}, {"beta", prod_bad}, {"gamma.2", prod_ok}};
    cap_t c;
    memset(&c, 0, sizeof(c));
    esp_err_t e = diag_bundle_run(secs, 3, "[\"/api/status\"]", cap_sink, &c);
    TEST_CHECK(e == ESP_OK, "run ok");
    TEST_CHECK(strncmp(c.buf, "KDF1 _manifest ", 15) == 0, "manifest first");
    TEST_CHECK(strstr(c.buf, "\"sections\":[\"alpha\",\"beta\",\"gamma.2\"]") != NULL, "manifest lists sections");
    TEST_CHECK(strstr(c.buf, "\"pc_supplement\":[\"/api/status\"]") != NULL, "manifest pc_supplement");
    TEST_CHECK(strstr(c.buf, "KDF1 alpha 3\nabc\nKDF1 alpha 2\nde\n") != NULL, "multi-frame section");
    TEST_CHECK(strstr(c.buf, "KDF1 gamma.2 3\nabc\n") != NULL, "later section still runs after failure");
    TEST_CHECK(strstr(c.buf, "{\"name\":\"beta\",\"ok\":false") != NULL, "failed producer recorded");
    TEST_CHECK(strstr(c.buf, "{\"name\":\"alpha\",\"ok\":true,\"err\":0,\"bytes\":5}") != NULL, "bytes counted");
    TEST_CHECK(strstr(c.buf, "KDF1 _trailer ") != NULL && strstr(c.buf, "\"complete\":true") != NULL, "trailer present");

    memset(&c, 0, sizeof(c));
    c.fail_after = 5;
    e = diag_bundle_run(secs, 3, NULL, cap_sink, &c);
    TEST_CHECK(e == ESP_FAIL, "sink failure aborts with the sink error");
    TEST_CHECK(strstr(c.buf, "_trailer") == NULL, "no trailer on a truncated run");

    diag_out_t o = {.sink = cap_sink, .sink_ctx = &c};
    memset(&c, 0, sizeof(c));
    TEST_CHECK(diag_frame_write(&o, "Bad Name", "x", 1) == ESP_ERR_INVALID_ARG, "bad name refused");
    TEST_CHECK(diag_frame_write(&o, "", "x", 1) == ESP_ERR_INVALID_ARG, "empty name refused");
    TEST_CHECK(c.len == 0, "refused frame writes nothing");

    /* Secret pin: the producer source must never reach credential stores. */
    char *p = test_resolve_from_here(__FILE__, "../drivers/http/diag_bundle_http.c");
    char *src = p ? test_read_whole_file(p) : NULL;
    TEST_CHECK(src != NULL, "diag_bundle_http.c readable");
    if (src) {
        static const char *banned[] = {"password", "passwd", "psk", "hash", "token", "totp", "secret",
                                       "session", "credential", "kiln_auth", "wifi_config", "salt"};
        for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "producer source has no '%s'", banned[i]);
            /* case-insensitive scan */
            int found = 0;
            size_t bl = strlen(banned[i]);
            for (char *q = src; *q && !found; q++) {
                size_t k = 0;
                while (k < bl && q[k] && ((q[k] | 0x20) == banned[i][k])) {
                    k++;
                }
                found = (k == bl);
            }
            TEST_CHECK(!found, msg);
        }
        free(src);
    }
    free(p);
}
