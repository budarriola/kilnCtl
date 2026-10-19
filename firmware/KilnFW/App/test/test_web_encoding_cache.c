// KH-09: the final Cache-Control of a signed-in page shell is no-store, not the no-cache that
// web_set_asset_cache_headers() used to write over the prehandler's no-store.
//
// Own SEPARATE executable: #includes the REAL web_encoding.c (the code that issued the overwriting
// header) and links the real http_auth_enforce.c predicate. The prehandler itself needs the whole
// session stack, so its half is modelled by fake_prehandler() (same predicate, same flag hand-off)
// and PINNED to the real source by test_prehandler_source_pin().
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "test_common.h"

#include "../drivers/common/web_encoding.c"
#include "../drivers/http/http_auth_enforce.h"

int g_test_failures = 0;
int g_test_count = 0;

/* ---- header-capturing httpd stubs: last write wins, like the real httpd table ---- */
static char s_cc[32];
static int s_cc_writes;
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    if (field && strcmp(field, "Cache-Control") == 0) {
        s_cc_writes++;
        strncpy(s_cc, value ? value : "", sizeof(s_cc) - 1);
        s_cc[sizeof(s_cc) - 1] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, long long n) { (void)r; (void)b; (void)n; return ESP_OK; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *f, char *v, size_t n)
{
    (void)r; (void)f; (void)v; (void)n;
    return ESP_FAIL;
}

/* ---- model of the prehandler's flag hand-off (http_auth_http.c) ---- */
static const httpd_req_t *s_mark;
bool http_auth_req_wants_no_store(const httpd_req_t *req) { return req != NULL && req == s_mark; }

static void fake_prehandler(httpd_req_t *req, bool web_enabled, route_tier_t tier)
{
    s_mark = NULL;
    if (http_auth_page_shell_needs_no_store(req->uri, (httpd_method_t)req->method, web_enabled, tier)) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        s_mark = req;
    }
}

static const char *serve(const char *uri, bool web_enabled, route_tier_t tier)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = HTTP_GET;
    strncpy(req.uri, uri, sizeof(req.uri) - 1);
    s_cc[0] = '\0';
    s_cc_writes = 0;
    fake_prehandler(&req, web_enabled, tier);
    web_set_asset_cache_headers(&req); /* the page-shell handler's call */
    return s_cc;
}

static void test_final_cache_control(void)
{
    TEST_SECTION("KH-09 final Cache-Control");
    static const char *const shells[] = { "/settings", "/ota", "/wifi" };
    for (size_t i = 0; i < sizeof(shells) / sizeof(shells[0]); i++) {
        TEST_CHECK(strcmp(serve(shells[i], true, ROUTE_TIER_ADMIN), "no-store") == 0,
                   "gated page shell with auth on ends no-store");
    }
    TEST_CHECK(strcmp(serve("/", true, ROUTE_TIER_OPEN), "no-cache") == 0, "dashboard keeps no-cache");
    TEST_CHECK(strcmp(serve("/settings", false, ROUTE_TIER_ADMIN), "no-cache") == 0,
               "auth off page shell keeps no-cache");
    TEST_CHECK(strcmp(serve("/app.js", true, ROUTE_TIER_OPEN), "no-cache") == 0, "static asset keeps no-cache");
}

static void test_prehandler_source_pin(void)
{
    static const char *const C[] = { "../drivers/http/http_auth_http.c", "App/drivers/http/http_auth_http.c",
                                     "firmware/KilnFW/App/drivers/http/http_auth_http.c" };
    char *t = test_read_source_anchored(__FILE__, C[0], C, 3);
    TEST_CHECK(t != NULL, "http_auth_http.c readable");
    if (!t) {
        return;
    }
    char *w = t;
    for (const char *r = t; *r; r++) {
        if (*r != '\r') {
            *w++ = *r;
        }
    }
    *w = '\0';
    TEST_CHECK(strstr(t, "if (http_auth_page_shell_needs_no_store(ctx->uri, ctx->method, web_enabled, ctx->tier)) {\n"
                         "                httpd_resp_set_hdr(req, \"Cache-Control\", \"no-store\");\n"
                         "                s_no_store_req = req;\n") != NULL,
               "prehandler ALLOW branch marks the request no-store (KH-09)");
    TEST_CHECK(strstr(t, "static esp_err_t kiln_http_prehandler(httpd_req_t *req) {\n    s_no_store_req = NULL;") != NULL,
               "prehandler clears the mark on every request (KH-09)");
    free(t);
}

int main(void)
{
    test_final_cache_control();
    test_prehandler_source_pin();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
