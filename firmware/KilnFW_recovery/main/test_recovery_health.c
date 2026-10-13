// test_recovery_health.c -- host test for recovery_health_policy.h plus
// source scans of recovery_wifi.c / recovery_http.c / recovery_main.c: the
// recovery image must not report "up" for a Wi-Fi AP or HTTP server that does
// not exist, and must record distinct error strings for each bring-up failure.
// Built and run by check_recovery_health.ps1 (MSVC) as:
//   test.exe <recovery_wifi.c> <recovery_http.c> <recovery_main.c>
// Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_health_policy.h"

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

static void test_policy(void)
{
    CHECK(!rhealth_ap_stop_should_restart(0), "0 AP_STOP events: no restart");
    CHECK(!rhealth_ap_stop_should_restart(1), "1 AP_STOP event: no restart");
    CHECK(!rhealth_ap_stop_should_restart(RHEALTH_AP_STOP_RESTART_LIMIT - 1), "limit-1: no restart");
    CHECK(rhealth_ap_stop_should_restart(RHEALTH_AP_STOP_RESTART_LIMIT), "limit reached: restart");
    CHECK(rhealth_ap_stop_should_restart(RHEALTH_AP_STOP_RESTART_LIMIT + 1), "past limit: restart");
    CHECK(rhealth_ap_stop_should_restart(0xffffffffu), "huge count: restart");
    CHECK(RHEALTH_AP_STOP_RESTART_LIMIT >= 2 && RHEALTH_AP_STOP_RESTART_LIMIT <= 10, "limit is small");
    CHECK(RHEALTH_HTTP_START_ATTEMPTS >= 2 && RHEALTH_HTTP_START_ATTEMPTS <= 10, "retry count bounded");

    CHECK(rhealth_http_ok(0, 12, 12), "start ok + all routes: http ok");
    CHECK(!rhealth_http_ok(0, 11, 12), "one route missing: not ok");
    CHECK(!rhealth_http_ok(0, 0, 12), "no routes: not ok");
    CHECK(!rhealth_http_ok(0, 13, 12), "more registered than expected: not ok");
    CHECK(!rhealth_http_ok(-1, 12, 12), "httpd_start failed: not ok");
    CHECK(!rhealth_http_ok(0x102, 12, 12), "httpd_start invalid arg: not ok");
    CHECK(!rhealth_http_ok(0, 0, 0), "zero expected routes: not ok");

    CHECK(rhealth_http_restart_allowed(0), "first HTTP-failure restart allowed");
    CHECK(rhealth_http_restart_allowed(RHEALTH_HTTP_MAX_RESTARTS - 1), "last allowed restart");
    CHECK(!rhealth_http_restart_allowed(RHEALTH_HTTP_MAX_RESTARTS), "restart cap reached: stay up");
    CHECK(!rhealth_http_restart_allowed(0xffffffffu), "garbage count: stay up");
    CHECK(RHEALTH_HTTP_MAX_RESTARTS >= 1 && RHEALTH_HTTP_MAX_RESTARTS <= 5, "restart cap is small");
    CHECK(rhealth_wifi_restart_allowed(0), "first Wi-Fi restart allowed");
    CHECK(!rhealth_wifi_restart_allowed(RHEALTH_HTTP_MAX_RESTARTS), "Wi-Fi restart cap reached");
    CHECK(rhealth_ap_stop_action(1, 0) == RHEALTH_AP_RERAISE, "first AP_STOP: re-raise");
    CHECK(rhealth_ap_stop_action(RHEALTH_AP_STOP_RESTART_LIMIT - 1, 9) == RHEALTH_AP_RERAISE,
          "below the stop limit: re-raise even with the cap used");
    CHECK(rhealth_ap_stop_action(RHEALTH_AP_STOP_RESTART_LIMIT, 0) == RHEALTH_AP_RESTART_IMAGE,
          "stop limit with cap left: restart image");
    CHECK(rhealth_ap_stop_action(RHEALTH_AP_STOP_RESTART_LIMIT, RHEALTH_HTTP_MAX_RESTARTS) ==
              RHEALTH_AP_STAY_DOWN, "stop limit with cap used: stay up (AP DOWN)");
    CHECK(rhealth_ap_reraise_failed_action(0) == RHEALTH_AP_RESTART_IMAGE,
          "esp_wifi_start failed, cap left: restart image");
    CHECK(rhealth_ap_reraise_failed_action(RHEALTH_HTTP_MAX_RESTARTS) == RHEALTH_AP_STAY_DOWN,
          "esp_wifi_start failed, cap used: stay up");
    CHECK(rhealth_restart_count_at_boot(true, true, 2) == 0, "power-on reset clears the count");
    CHECK(rhealth_restart_count_at_boot(false, false, 2) == 0, "bad magic clears the count");
    CHECK(rhealth_restart_count_at_boot(false, true, 2) == 2, "software restart keeps the count");

    for (int m = 0; m < 8; m++) {
        bool w = m & 1, l = m & 2, h = m & 4;
        CHECK(rhealth_all_up(w, l, h) == (m == 7), "all_up only when wifi, lcd and http are all ok");
    }
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = 0;
        size_t w = 0; // drop CR so CRLF checkouts scan like LF ones
        for (size_t r = 0; r < got; r++) {
            if (buf[r] != '\r') {
                buf[w++] = buf[r];
            }
        }
        buf[w] = 0;
    }
    fclose(f);
    return buf;
}

// Replaces // and /* */ comments with spaces (string literals respected).
static void strip_comments(char *s)
{
    for (char *p = s; *p;) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p += (*p == '\\' && p[1]) ? 2 : 1) {
            }
            if (*p) {
                p++;
            }
        } else if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                *p++ = ' ';
            }
        } else if (p[0] == '/' && p[1] == '*') {
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                *p++ = ' ';
            }
            if (*p) {
                p[0] = p[1] = ' ';
                p += 2;
            }
        } else {
            p++;
        }
    }
}

static char *load_stripped(const char *path, const char *name)
{
    char *s = slurp(path);
    CHECK(s != NULL, name);
    if (s) {
        strip_comments(s);
    }
    return s;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        n++;
    }
    return n;
}

// Copies the text from `from` up to (not incl.) the first `to` after it.
static char *between(const char *from, const char *to)
{
    if (!from) {
        return NULL;
    }
    const char *e = strstr(from + 1, to);
    size_t n = e ? (size_t)(e - from) : strlen(from);
    char *r = (char *)malloc(n + 1);
    memcpy(r, from, n);
    r[n] = 0;
    return r;
}

static void test_wifi_source(const char *path)
{
    char *src = load_stripped(path, "recovery_wifi.c readable");
    if (!src) {
        return;
    }
    // M1: s_up is set true only in the AP_START handler (plus the explicit
    // no-event-handler fallback) and cleared in AP_STOP.
    CHECK(count_of(src, "s_up = true;") == 2, "s_up = true appears exactly twice (AP_START, fallback)");
    char *start_case = between(strstr(src, "case WIFI_EVENT_AP_START:"), "case WIFI_EVENT_AP_STOP:");
    char *stop_case = between(strstr(src, "case WIFI_EVENT_AP_STOP:"), "case WIFI_EVENT_AP_STACONNECTED:");
    CHECK(start_case && strstr(start_case, "s_up = true;"), "AP_START handler sets s_up");
    CHECK(stop_case && strstr(stop_case, "s_up = false;"), "AP_STOP handler clears s_up");
    CHECK(stop_case && strstr(stop_case, "rhealth_ap_stop_action("),
          "AP_STOP handler consults the restart policy");
    CHECK(stop_case && count_of(stop_case, "restart_image_for_wifi();") == 2 &&
              strstr(stop_case, "rhealth_ap_stop_action("),
          "AP_STOP handler restarts the image through the counted, capped path");
    CHECK(stop_case && strstr(stop_case, "esp_restart()") == NULL, "AP_STOP handler never calls esp_restart directly");
    CHECK(stop_case && strstr(stop_case, "RHEALTH_AP_STAY_DOWN") && strstr(stop_case, "RLCD_AP_FAILED"),
          "cap used up: stay up with AP DOWN on the LCD");
    CHECK(stop_case && strstr(stop_case, "recovery_lcd_set_ap_state(RLCD_AP_RESTARTING);") &&
              strstr(stop_case, "esp_wifi_start()"),
          "AP_STOP below the limit hides the dead AP's credentials and re-raises the AP");
    CHECK(stop_case && strstr(stop_case, "rhealth_ap_reraise_failed_action("),
          "a failed re-raise feeds the capped restart path");
    {
        const char *sd = stop_case ? strstr(stop_case, "if (s_shutting_down)") : NULL;
        const char *act = stop_case ? strstr(stop_case, "rhealth_ap_stop_action(") : NULL;
        CHECK(sd && act && sd < act, "AP_STOP during shutdown is ignored before any action");
    }
    CHECK(count_of(src, "recovery_health_clear_wifi();") == 3,
          "wifi count cleared on stability and on both stay-down paths");
    {
        const char *dec = stop_case ? strstr(stop_case, "rhealth_ap_stop_action(") : NULL;
        const char *clr = stop_case ? strstr(stop_case, "recovery_health_clear_wifi();") : NULL;
        CHECK(dec && clr && dec < clr, "wifi count is cleared only after the restart decision");
        const char *fa = stop_case ? strstr(stop_case, "rhealth_ap_reraise_failed_action(") : NULL;
        const char *clr2 = fa ? strstr(fa, "recovery_health_clear_wifi();") : NULL;
        CHECK(fa && clr2, "failed re-raise stay-down clears after its decision");
    }
    CHECK(strstr(src, "s_up && s_ap_stop_count == s_stops_at_arm") != NULL,
          "stability clear requires AP up with no new stop");
    CHECK(start_case && strstr(start_case, "arm_stable_timer();"), "AP_START arms the stability timer");
    CHECK(strstr(src, "esp_register_shutdown_handler(on_shutdown)") != NULL, "wifi registers a shutdown handler");
    CHECK(strstr(src, "static void on_shutdown(void)") && strstr(src, "s_shutting_down = true;"),
          "shutdown handler sets s_shutting_down");
    CHECK(start_case && strstr(start_case, "recovery_lcd_set_ap_state(RLCD_AP_UP);"),
          "AP_START clears the AP-down screen");
    char *softap = between(strstr(src, "static bool start_softap("), "void recovery_wifi_start(void)");
    CHECK(softap != NULL, "start_softap() found");
    if (softap) {
        const char *ws = strstr(softap, "esp_wifi_start()");
        const char *wait = strstr(softap, "!s_up && waited_ms");
        const char *lcd = strstr(softap, "recovery_lcd_set_ap(");
        CHECK(ws && wait && lcd && ws < wait && wait < lcd,
              "start_softap waits for s_up after esp_wifi_start and before showing the passphrase");
        const char *first_up = strstr(softap, "s_up = true;");
        CHECK(first_up != NULL && strstr(first_up, "s_up = true;") == first_up && ws && first_up > ws,
              "no s_up = true before esp_wifi_start returns");
        char *failwait = between(strstr(softap, "if (!s_up) {"), "ESP_LOGI(TAG, \"SoftAP up");
        CHECK(failwait && strstr(failwait, "return false;") && strstr(failwait, "\"softap_fail\""),
              "no AP_START in time: softap_fail and return false");
        free(failwait);
    }
    // M2: a distinct error string for every bring-up failure path.
    static const char *const errs[] = {"netif_init_fail", "event_loop_fail", "wifi_init_fail",
                                       "softap_fail", "event_register_fail", "wifi_storage_fail"};
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "s_error = \"%s\";", errs[i]);
        CHECK(strstr(src, needle) != NULL, needle);
    }
    // ...recorded in the same branch as the LCD no-network call.
    char *fn = strstr(src, "void recovery_wifi_start(void)");
    const char *lcd_nn = fn ? strstr(fn, "recovery_lcd_set_no_network();") : NULL;
    const char *e1 = fn ? strstr(fn, "s_error = \"netif_init_fail\";") : NULL;
    CHECK(e1 && lcd_nn && e1 < lcd_nn, "netif_init_fail recorded before its LCD call");
    const char *e2 = fn ? strstr(fn, "s_error = \"event_loop_fail\";") : NULL;
    const char *lcd_nn2 = lcd_nn ? strstr(lcd_nn + 1, "recovery_lcd_set_no_network();") : NULL;
    CHECK(e2 && lcd_nn2 && e2 < lcd_nn2, "event_loop_fail recorded before its LCD call");
    const char *e3 = fn ? strstr(fn, "s_error = \"wifi_init_fail\";") : NULL;
    const char *lcd_nn3 = lcd_nn2 ? strstr(lcd_nn2 + 1, "recovery_lcd_set_no_network();") : NULL;
    CHECK(e3 && lcd_nn3 && e3 < lcd_nn3, "wifi_init_fail recorded before its LCD call");
    free(start_case);
    free(stop_case);
    free(softap);
    free(src);
}

static void test_http_source(const char *path)
{
    char *src = load_stripped(path, "recovery_http.c readable");
    if (!src) {
        return;
    }
    CHECK(strstr(src, "esp_err_t recovery_http_start(void)") != NULL, "recovery_http_start returns esp_err_t");
    CHECK(strstr(src, "void recovery_http_start(void)") == NULL, "no void recovery_http_start left");
    char *fn = between(strstr(src, "esp_err_t recovery_http_start(void)"), "\n}\n");
    CHECK(fn != NULL, "recovery_http_start body found");
    if (fn) {
        CHECK(strstr(fn, "esp_err_t start_rc = httpd_start(") != NULL, "httpd_start result is kept");
        CHECK(strstr(fn, "esp_err_t rr = httpd_register_uri_handler(") != NULL,
              "httpd_register_uri_handler result is kept");
        CHECK(strstr(fn, "registered++") != NULL, "registered routes are counted");
        CHECK(strstr(fn, "rhealth_http_ok(") != NULL, "start+route count judged by the policy");
        CHECK(strstr(fn, "recovery_lcd_set_error(") == NULL,
              "per-attempt start never sets the LCD banner (a later retry may succeed)");
        CHECK(strstr(fn, "\"http_start_fail\"") && strstr(fn, "\"route_register_fail\""),
              "failure records a status error string");
        CHECK(strstr(fn, "httpd_stop(server)") != NULL, "partial server is stopped before a retry");
        const char *ok_ret = strstr(fn, "return ESP_OK;");
        const char *started = strstr(fn, "s_started = true;");
        CHECK(started && ok_ret && started < ok_ret, "s_started is set only on the success path");
        CHECK(strstr(fn, "return start_rc != ESP_OK ? start_rc : ESP_FAIL;") != NULL, "failure returns an error");
    }
    char *st = between(strstr(src, "esp_err_t recovery_status_get("), "\n}\n");
    CHECK(st && strstr(st, "http_start_attempts") && strstr(st, "s_http_failed_attempts"),
          "status route reports the failed HTTP start count");
    CHECK(fn && strstr(fn, "s_http_last_error = NULL") == NULL, "a later success does not erase the failure record");
    free(st);
    free(fn);
    free(src);
}

static void test_main_source(const char *path)
{
    char *src = load_stripped(path, "recovery_main.c readable");
    if (!src) {
        return;
    }
    const char *start = strstr(src, "recovery_http_start()");
    CHECK(start != NULL, "recovery_main calls recovery_http_start");
    CHECK(start && strstr(start - 20 > src ? start - 20 : src, "esp_err_t herr =") != NULL,
          "recovery_main keeps recovery_http_start's result");
    CHECK(strstr(src, "RHEALTH_HTTP_START_ATTEMPTS") != NULL, "http start is retried a bounded number of times");
    const char *rs = strstr(src, "recovery_health_restart_for_http();");
    const char *up = strstr(src, "recovery image up");
    CHECK(rs != NULL && start && rs > start, "image restarts when http never comes up");
    CHECK(up && rs && rs < up, "restart (hard failure) precedes the up line");
    const char *gate = strstr(src, "rhealth_all_up(");
    CHECK(gate && up && gate < up, "up line is gated by rhealth_all_up()");
    CHECK(gate && strstr(gate, "recovery_wifi_is_up()") && strstr(gate, "recovery_lcd_is_ok()"),
          "up gate covers wifi and lcd");
    const char *banner = strstr(src, "recovery_lcd_set_error(\"HTTP FAILED\"");
    CHECK(banner != NULL && banner > start, "the final failure shows HTTP FAILED from main");
    CHECK(count_of(src, "recovery_lcd_set_error(") == 2, "main sets the banner only on final failure paths");
    const char *clr = strstr(src, "recovery_lcd_clear_error();");
    CHECK(clr != NULL && up && clr < up, "success path clears the LCD error");
    CHECK(strstr(src, "rhealth_http_restart_allowed(recovery_health_http_restarts())") != NULL, "restart loop is bounded by policy");
    CHECK(strstr(src, "recovery_health_boot_init();") != NULL, "restart counters are initialised at boot");
    CHECK(count_of(src, "recovery_health_clear_http();") == 2,
          "HTTP restart count cleared on success and when staying up after the limit");
    CHECK(strstr(src, "recovery image DEGRADED") != NULL, "a degraded come-up is logged as an error");
    free(src);
}

static void test_health_source(const char *path)
{
    char *src = load_stripped(path, "recovery_health.c readable");
    if (!src) {
        return;
    }
    CHECK(strstr(src, "RTC_NOINIT_ATTR uint32_t s_http_restarts") != NULL, "http count survives esp_restart");
    CHECK(strstr(src, "RTC_NOINIT_ATTR uint32_t s_wifi_restarts") != NULL, "wifi count survives esp_restart");
    CHECK(strstr(src, "esp_reset_reason() == ESP_RST_POWERON") != NULL, "power-on reset detected");
    CHECK(count_of(src, "rhealth_restart_count_at_boot(") == 2, "both counters validated at boot");
    char *sd = between(strstr(src, "static void on_shutdown(void)"), "\n}\n");
    CHECK(sd && strstr(sd, "if (!s_keep)") && strstr(sd, "s_http_restarts = 0;") && strstr(sd, "s_wifi_restarts = 0;"),
          "deliberate restarts zero both counters");
    char *fh = between(strstr(src, "void recovery_health_restart_for_http(void)"), "\n}\n");
    CHECK(fh && strstr(fh, "s_http_restarts++;") && strstr(fh, "s_keep = true;"), "http restart counts and keeps");
    char *fw = between(strstr(src, "void recovery_health_restart_for_wifi(void)"), "\n}\n");
    CHECK(fw && strstr(fw, "s_wifi_restarts++;") && strstr(fw, "s_keep = true;"), "wifi restart counts and keeps");
    char *cw = between(strstr(src, "void recovery_health_clear_wifi(void)"), "\n}\n");
    CHECK(cw && strstr(cw, "s_wifi_restarts = 0;") && !strstr(cw, "s_http_restarts"), "clear_wifi zeroes only the wifi count");
    free(cw);
    free(sd);
    free(fh);
    free(fw);
    free(src);
}

int main(int argc, char **argv)
{
    test_policy();
    if (argc < 5) {
        fprintf(stderr, "usage: %s <recovery_wifi.c> <recovery_http.c> <recovery_main.c> <recovery_health.c>\n", argv[0]);
        return 2;
    }
    test_health_source(argv[4]);
    test_wifi_source(argv[1]);
    test_http_source(argv[2]);
    test_main_source(argv[3]);
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
