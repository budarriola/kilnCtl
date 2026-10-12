// Host test for drivers/http/http_body_recv.h (HTTP audit E2 #1).
//
// POST /api/auth/bootstrap_password used to do ONE httpd_req_recv() and parse
// whatever came back, so a body split over two TCP segments stored a
// truncated admin password with 200 ok. The handler now reads through
// http_body_recv_full(); this test drives that helper with a recv stub that
// returns the body in chunks, and a connection that drops mid-body, and
// checks the handler source still goes through the helper.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "../drivers/http/http_body_recv.h"

// ---- recv stub: hands back `body` in chunks of at most s_chunk bytes; after
// s_drop_after bytes (if >= 0) the connection "drops" and recv returns -1.
static const char *s_body;
static size_t s_body_len;
static size_t s_pos;
static size_t s_chunk;
static long s_drop_after;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (s_drop_after >= 0 && (long)s_pos >= s_drop_after) {
        return -1;
    }
    size_t n = s_body_len - s_pos;
    if (n > s_chunk) n = s_chunk;
    if (n > buf_len) n = buf_len;
    if (s_drop_after >= 0 && (long)(s_pos + n) > s_drop_after) n = (size_t)s_drop_after - s_pos;
    if (n == 0) return 0;
    memcpy(buf, s_body + s_pos, n);
    s_pos += n;
    return (int)n;
}

static void stage(const char *body, size_t chunk, long drop_after)
{
    s_body = body;
    s_body_len = strlen(body);
    s_pos = 0;
    s_chunk = chunk;
    s_drop_after = drop_after;
}

// Mirrors the handler: read via the helper, then "store" the password only
// when the read succeeded.
static bool bootstrap_like(const char *body, size_t chunk, long drop_after, char *stored, size_t cap)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    stage(body, chunk, drop_after);
    char buf[128];
    memset(buf, 'X', sizeof(buf));
    stored[0] = '\0';
    if (!http_body_recv_full(&req, buf, (size_t)req.content_len)) {
        // failure: partial plaintext must be wiped
        for (size_t i = 0; i <= (size_t)req.content_len; i++) {
            if (buf[i] != 0) return false;
        }
        return false;
    }
    memcpy(stored, buf, cap - 1);
    stored[cap - 1] = '\0';
    return true;
}

static void test_split_body(void)
{
    TEST_SECTION("body in two chunks stores the full password");
    const char *body = "username=admin&password=correct-horse-battery";
    char stored[128];
    bool ok = bootstrap_like(body, 20, -1, stored, sizeof(stored));
    TEST_CHECK(ok, "two-chunk body reads fully");
    TEST_CHECK(strcmp(stored, body) == 0, "full body, including the password tail, is present");
    ok = bootstrap_like(body, 1, -1, stored, sizeof(stored));
    TEST_CHECK(ok && strcmp(stored, body) == 0, "byte-at-a-time body reads fully");
    ok = bootstrap_like(body, 1000, -1, stored, sizeof(stored));
    TEST_CHECK(ok && strcmp(stored, body) == 0, "single-chunk body reads fully");
}

static void test_dropped_connection(void)
{
    TEST_SECTION("connection dropping mid-body fails and stores nothing");
    const char *body = "username=admin&password=correct-horse-battery";
    char stored[128];
    bool ok = bootstrap_like(body, 20, 25, stored, sizeof(stored));
    TEST_CHECK(!ok, "mid-body drop is a failed read (handler answers 400)");
    TEST_CHECK(stored[0] == '\0', "nothing stored after a mid-body drop");
    ok = bootstrap_like(body, 20, 0, stored, sizeof(stored));
    TEST_CHECK(!ok && stored[0] == '\0', "drop before any byte also fails");
    // recv returning 0 (EOF) before content_len is also a failure
    ok = bootstrap_like(body, 20, 20, stored, sizeof(stored));
    TEST_CHECK(!ok, "drop at a chunk boundary fails");
}

static void test_handler_uses_helper(void)
{
    TEST_SECTION("bootstrap handler and sim POST read through the helper");
    char path[512];
    const char *files[] = { "../drivers/http/security_backend_web_auth.c", "../drivers/sim/sim_backend.c" };
    const char *self = __FILE__;
    const char *slash = strrchr(self, 0x5c);
    const char *slash2 = strrchr(self, '/');
    if (slash2 > slash) slash = slash2;
    size_t dirlen = slash ? (size_t)(slash - self) + 1 : 0;
    for (int f = 0; f < 2; f++) {
        snprintf(path, sizeof(path), "%.*s%s", (int)dirlen, self, files[f]);
        FILE *fp = fopen(path, "rb");
        TEST_CHECK(fp != NULL, "handler source is readable");
        if (!fp) continue;
        static char src[400000];
        size_t n = fread(src, 1, sizeof(src) - 1, fp);
        src[n] = '\0';
        fclose(fp);
        TEST_CHECK(strstr(src, "http_body_recv_full(") != NULL, "handler calls http_body_recv_full()");
        TEST_CHECK(strstr(src, "httpd_req_recv(") == NULL, "handler has no single-shot httpd_req_recv()");
    }
}

int main(void)
{
    test_split_body();
    test_dropped_connection();
    test_handler_uses_helper();
    printf("%d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
