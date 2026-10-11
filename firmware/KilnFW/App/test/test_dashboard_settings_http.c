// Host test for App/drivers/http/dashboard_settings_http.c (round 3, R3-C):
// POST /api/unit_pref and POST /api/safety/log_level.
// #includes the real .c. Faked: cfg_fs availability, unit_pref_set_ex, the
// safety link send, the relay-level setter, the httpd leaf calls and
// httpd_req_recv. http_form_find_field is the real header implementation.
//
// Pins: unit_pref -- unmounted cfg refuses 503 before the body is read; body
// length bounds; only C/F/celsius/fahrenheit accepted (case-insensitive on
// the word, exact on the letter); a failed save answers 500 and never ok;
// log_level -- unwired safety link 500, level 0..4 only, peer fails closed
// (an overlong or empty peer must NOT fall through to the wire), peer=relay
// is local only (no wire send), a failed wire send answers ok:false.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "dashboard_http_internal.h"

const char *DASH_TAG = "dash";
struct dashboard_board_objects s_dash;

#include "../drivers/http/dashboard_settings_http.c"

/* cfg_fs */
static bool s_cfg_available = true;
bool cfg_fs_is_available(void) { return s_cfg_available; }
bool cfg_fs_skipped_for_recovery(void) { return false; }

/* unit_pref */
static esp_err_t s_set_result = ESP_OK;
static bool s_set_adopted = false;
static int s_set_calls = 0;
static unit_pref_t s_set_last;
esp_err_t unit_pref_set_ex(unit_pref_t pref, bool *out_adopted)
{
    s_set_calls++;
    s_set_last = pref;
    if (out_adopted) *out_adopted = s_set_adopted;
    return s_set_result;
}

/* safety link / relay level */
static esp_err_t s_send_result = ESP_OK;
static int s_send_calls = 0;
static uint8_t s_send_level;
esp_err_t safety_link_send_set_log_level(SafetyLinkClass *link, uint8_t level)
{
    (void)link;
    s_send_calls++;
    s_send_level = level;
    return s_send_result;
}
static int s_relay_calls = 0;
static uint8_t s_relay_level;
void uart_log_bridge_set_safety_relay_level(uint8_t level)
{
    s_relay_calls++;
    s_relay_level = level;
}

/* httpd */
static const char *s_body_in;
static size_t s_body_pos;
static int s_recv_calls;
static int s_status_code;
static char s_out[256];
static int s_err_calls;
static int s_err_code;
int httpd_req_recv(httpd_req_t *req, char *buf, size_t n)
{
    (void)req;
    s_recv_calls++;
    if (!s_body_in) return -1;
    size_t left = strlen(s_body_in) - s_body_pos;
    if (left == 0) return 0;
    if (n > left) n = left;
    memcpy(buf, s_body_in + s_body_pos, n);
    s_body_pos += n;
    return (int)n;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status_code = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    snprintf(s_out, sizeof(s_out), "%s", s ? s : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m)
{
    (void)r;
    s_err_calls++;
    s_err_code = (int)e;
    snprintf(s_out, sizeof(s_out), "%s", m ? m : "");
    return ESP_OK;
}

static void reset(void)
{
    s_cfg_available = true;
    s_set_result = ESP_OK; s_set_adopted = false; s_set_calls = 0;
    s_send_result = ESP_OK; s_send_calls = 0; s_send_level = 0xFF;
    s_relay_calls = 0; s_relay_level = 0xFF;
    s_body_in = NULL; s_body_pos = 0; s_recv_calls = 0;
    s_status_code = 200; s_out[0] = '\0'; s_err_calls = 0; s_err_code = 0;
    memset(&s_dash, 0, sizeof(s_dash));
    s_dash.safety = (SafetyLinkClass *)(uintptr_t)0x1000;
}

static void call(esp_err_t (*h)(httpd_req_t *), const char *body, long len)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_body_in = body;
    s_body_pos = 0;
    req.content_len = (len >= 0) ? (size_t)len : strlen(body);
    TEST_CHECK(h(&req) == ESP_OK, "handler returns ESP_OK");
}

static void unit(const char *body) { call(unit_pref_post_handler, body, -1); }
static void level(const char *body) { call(safety_log_level_post_handler, body, -1); }

static void test_unit_gates(void)
{
    TEST_SECTION("unit_pref -- unmounted cfg and body bounds");
    reset();
    s_cfg_available = false;
    unit("unit=F");
    TEST_CHECK(s_status_code == 503 && s_set_calls == 0 && s_recv_calls == 0, "unmounted: 503, body unread, nothing set");
    reset();
    call(unit_pref_post_handler, "", 0);
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST && s_set_calls == 0, "empty body: 400");
    reset();
    unit("unit=fahrenheit&x"); /* 17 bytes > 16 */
    TEST_CHECK(s_err_calls == 1 && s_set_calls == 0 && s_recv_calls == 0, "oversize body: 400 before read");
    reset();
    call(unit_pref_post_handler, NULL, 6);
    TEST_CHECK(s_err_calls == 1 && s_set_calls == 0, "recv failure: 400");
}

static void test_unit_values(void)
{
    TEST_SECTION("unit_pref -- accepted and refused values");
    static const struct { const char *body; int ok; unit_pref_t pref; } c[] = {
        { "unit=F", 1, UNIT_PREF_FAHRENHEIT },
        { "unit=C", 1, UNIT_PREF_CELSIUS },
        { "unit=fahrenheit", 1, UNIT_PREF_FAHRENHEIT },
        { "unit=FAHRENHEIT", 1, UNIT_PREF_FAHRENHEIT },
        { "unit=Celsius", 1, UNIT_PREF_CELSIUS },
        { "unit=f", 0, UNIT_PREF_CELSIUS },
        { "unit=c", 0, UNIT_PREF_CELSIUS },
        { "unit=K", 0, UNIT_PREF_CELSIUS },
        { "unit=", 0, UNIT_PREF_CELSIUS },
        { "x=F", 0, UNIT_PREF_CELSIUS },
        { "unit=Fx", 0, UNIT_PREF_CELSIUS },
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        reset();
        unit(c[i].body);
        if (c[i].ok) {
            TEST_CHECK(s_set_calls == 1 && s_set_last == c[i].pref && strcmp(s_out, "{\"ok\":true}") == 0, "accepted");
        } else {
            TEST_CHECK(s_set_calls == 0 && s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST, "refused 400, never saved");
        }
    }
}

static void test_unit_save_failure(void)
{
    TEST_SECTION("unit_pref -- save failure never answers ok");
    reset();
    s_set_result = ESP_FAIL;
    unit("unit=F");
    TEST_CHECK(s_status_code == 500 && strstr(s_out, "\"ok\":false") && strstr(s_out, "\"adopted\":false"), "unadopted: 500 adopted false");
    reset();
    s_set_result = ESP_FAIL;
    s_set_adopted = true;
    unit("unit=C");
    TEST_CHECK(s_status_code == 500 && strstr(s_out, "\"adopted\":true") && !strstr(s_out, "{\"ok\":true}"), "adopted: 500 adopted true");
}

static void test_level(void)
{
    TEST_SECTION("log_level -- wiring, bounds, level parsing");
    reset();
    s_dash.safety = NULL;
    level("level=2");
    TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_500_INTERNAL_SERVER_ERROR && s_send_calls == 0, "no link: 500");
    reset();
    call(safety_log_level_post_handler, "", 0);
    TEST_CHECK(s_err_calls == 1 && s_send_calls == 0, "empty body: 400");
    reset();
    level("level=2&peer=safety&pad=xxxxxxxxxxxxxxxxx"); /* > 32 */
    TEST_CHECK(s_err_calls == 1 && s_send_calls == 0 && s_recv_calls == 0, "oversize: 400 before read");
    static const char *const bad[] = { "peer=safety", "level=", "level=5", "level=-1", "level=2x", "level=+2", "level=%202", "level=x", "level=99999999" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset();
        level(bad[i]);
        TEST_CHECK(s_err_calls == 1 && s_send_calls == 0 && s_relay_calls == 0, "bad level refused");
    }
    for (int l = 0; l <= 4; l++) {
        char b[24];
        snprintf(b, sizeof(b), "level=%d", l);
        reset();
        level(b);
        TEST_CHECK(s_send_calls == 1 && s_send_level == l && s_relay_calls == 0 && strcmp(s_out, "{\"ok\":true}") == 0,
                   "levels 0..4 go to the wire by default");
    }
}

static void test_peer(void)
{
    TEST_SECTION("log_level -- peer fails closed; relay is local only");
    reset();
    level("level=3&peer=relay");
    TEST_CHECK(s_relay_calls == 1 && s_relay_level == 3 && s_send_calls == 0, "relay: local setter, no wire");
    TEST_CHECK(strcmp(s_out, "{\"ok\":true}") == 0, "relay: ok");
    reset();
    level("level=1&peer=RELAY");
    TEST_CHECK(s_relay_calls == 1 && s_send_calls == 0, "peer case-insensitive");
    reset();
    level("level=1&peer=safety");
    TEST_CHECK(s_send_calls == 1 && s_relay_calls == 0, "peer=safety: wire");
    static const char *const bad[] = { "level=1&peer=relayyyyyyy", "level=1&peer=", "level=1&peer=pico", "level=1&peer=relayx" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset();
        level(bad[i]);
        TEST_CHECK(s_err_calls == 1 && s_err_code == HTTPD_400_BAD_REQUEST && s_send_calls == 0 && s_relay_calls == 0,
                   "bad peer: 400, nothing sent anywhere");
    }
}

static void test_send_failure(void)
{
    TEST_SECTION("log_level -- wire send failure never claims ok");
    reset();
    s_send_result = ESP_FAIL;
    level("level=2");
    TEST_CHECK(s_status_code == 500 && strstr(s_out, "\"ok\":false") && !strstr(s_out, "\"ok\":true"), "500 ok:false");
}

int main(void)
{
    test_unit_gates();
    test_unit_values();
    test_unit_save_failure();
    test_level();
    test_peer();
    test_send_failure();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
