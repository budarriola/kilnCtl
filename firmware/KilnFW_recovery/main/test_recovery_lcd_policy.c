// test_recovery_lcd_policy.c -- host test for recovery_lcd_policy.h plus
// source scans of recovery_lcd.c and recovery_http.c. Built and run by
// check_recovery_lcd_policy.ps1 (MSVC) as:
//   test.exe <recovery_lcd.c> <recovery_http.c>
// Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_lcd_policy.h"

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

static void test_schedule(void)
{
    CHECK(RLCD_BURST_ATTEMPTS == 3, "burst is 3 attempts");
    CHECK(!rlcd_reset_expander_before(0), "first attempt does not re-reset the expander");
    CHECK(rlcd_reset_expander_before(1), "second attempt re-resets the expander");
    CHECK(rlcd_reset_expander_before(2), "third attempt re-resets the expander");

    // Simulate a burst against a panel that never comes up: exactly 3 attempts.
    int attempts = 0, resets = 0;
    for (int a = 0;; a++) {
        attempts++;
        if (rlcd_reset_expander_before(a)) {
            resets++;
        }
        if (!rlcd_burst_continue(a + 1, false)) {
            break;
        }
    }
    CHECK(attempts == 3, "failing burst makes exactly 3 attempts");
    CHECK(resets == 2, "failing burst re-resets the expander between attempts (2x)");

    // Succeeds on the 2nd attempt: stops there.
    attempts = 0;
    for (int a = 0;; a++) {
        attempts++;
        if (!rlcd_burst_continue(a + 1, a == 1)) {
            break;
        }
    }
    CHECK(attempts == 2, "burst stops at the first success");
    CHECK(!rlcd_burst_continue(1, true), "success on attempt 1 ends the burst");
    CHECK(rlcd_burst_continue(1, false), "failure on attempt 1 continues");
    CHECK(rlcd_burst_continue(2, false), "failure on attempt 2 continues");
    CHECK(!rlcd_burst_continue(3, false), "failure on attempt 3 ends the burst");
    CHECK(!rlcd_burst_continue(4, false), "never a 4th attempt in a burst");

    CHECK(rlcd_tick_action(true) == RLCD_TICK_NONE, "ready panel: tick does nothing");
    CHECK(rlcd_tick_action(false) == RLCD_TICK_WARN_AND_RETRY, "unready panel: tick warns and retries");

    CHECK(strcmp(RLCD_NOT_READY_MSG, "LCD NOT READY, passphrase not displayed") == 0,
          "unready log text is exact");
    CHECK(rlcd_draw_ok(0), "no failed lines is a good draw");
    CHECK(!rlcd_draw_ok(1), "one failed line is a failed draw");
    CHECK(!rlcd_draw_ok(9), "many failed lines is a failed draw");
}

static void test_classify(void)
{
    CHECK(rlcd_classify_boot_guard(RLCD_GET_OK, true) == RLCD_BG_VALID, "decoded record is valid");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_OK, false) == RLCD_BG_INVALID, "undecodable record is invalid");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_BAD_LENGTH, false) == RLCD_BG_INVALID,
          "wrong-length blob (ESP_ERR_NVS_INVALID_LENGTH) is invalid, not none");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_BAD_LENGTH, true) == RLCD_BG_INVALID, "bad length wins over decoded");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_NOT_FOUND, false) == RLCD_BG_NONE, "absent key is none");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_OTHER, false) == RLCD_BG_UNREADABLE, "other NVS error is unreadable");
    CHECK(rlcd_classify_boot_guard(RLCD_GET_OTHER, true) == RLCD_BG_UNREADABLE, "unreadable ignores decoded");
    CHECK(strcmp(rlcd_bg_state_name(RLCD_BG_NONE), "none") == 0, "name none");
    CHECK(strcmp(rlcd_bg_state_name(RLCD_BG_VALID), "valid") == 0, "name valid");
    CHECK(strcmp(rlcd_bg_state_name(RLCD_BG_INVALID), "invalid") == 0, "name invalid");
    CHECK(strcmp(rlcd_bg_state_name(RLCD_BG_UNREADABLE), "unreadable") == 0, "name unreadable");
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

// Returns a malloc'd copy of the function body starting at `sig` (up to the
// next line that is exactly "}"), or NULL. Works on LF or CRLF sources
// (slurp() reads binary, so strip CRs first via normalize()).
static char *fn_body(const char *src, const char *sig)
{
    const char *a = strstr(src, sig);
    if (!a) {
        return NULL;
    }
    const char *e = strstr(a, "\n}\n");
    if (!e) {
        e = a + strlen(a);
    }
    size_t n = (size_t)(e - a);
    char *b = (char *)malloc(n + 1);
    memcpy(b, a, n);
    b[n] = 0;
    return b;
}

static void normalize(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r != '\r') {
            *w++ = *r;
        }
    }
    *w = 0;
}

static void test_lcd_source(const char *path)
{
    char *src = slurp(path);
    CHECK(src != NULL, "recovery_lcd.c readable");
    if (!src) {
        return;
    }
    normalize(src);
    strip_comments(src);

    char *wrap = fn_body(src, "static esp_err_t draw_line(");
    CHECK(wrap != NULL, "draw_line wrapper found");
    if (wrap) {
        CHECK(strstr(wrap, "draw_line_raw(") != NULL, "draw_line wraps the raw drawer");
        CHECK(strstr(wrap, "s_pass_failed++") != NULL, "draw_line counts a failed line");
        free(wrap);
    }
    char *ds = fn_body(src, "static void draw_status(void)");
    CHECK(ds != NULL, "draw_status found");
    if (ds) {
        CHECK(strstr(ds, "s_pass_failed = 0") != NULL, "draw_status resets the failure count");
        const char *chk = strstr(ds, "rlcd_draw_ok(s_pass_failed)");
        CHECK(chk != NULL, "draw_status aggregates line failures");
        CHECK(chk != NULL && strstr(chk, "s_ready = false") != NULL, "failed draw clears s_ready");
        CHECK(chk != NULL && strstr(chk, "s_draw_failures++") != NULL, "failed draw is counted");
        CHECK(strstr(ds, "RLCD_BG_INVALID") != NULL, "screen has a 'record invalid' state");
        free(ds);
    }
    char *burst = fn_body(src, "static bool lcd_bring_up_burst(void)");
    CHECK(burst != NULL, "burst function found");
    if (burst) {
        CHECK(strstr(burst, "rlcd_reset_expander_before(") != NULL, "burst uses the re-reset rule");
        CHECK(strstr(burst, "recovery_io_expander_rehold()") != NULL, "burst re-resets the expander");
        CHECK(strstr(burst, "rlcd_burst_continue(") != NULL, "burst uses the continue rule");
        CHECK(strstr(burst, "s_init_attempts++") != NULL, "burst counts attempts");
        free(burst);
    }
    char *task = fn_body(src, "static void lcd_task(void *arg)");
    CHECK(task != NULL, "lcd_task found");
    if (task) {
        CHECK(strstr(task, "vTaskDelay(pdMS_TO_TICKS(LCD_TASK_PERIOD_MS))") != NULL, "lcd_task ticks periodically");
        CHECK(strstr(task, "rlcd_tick_action(s_ready)") != NULL, "lcd_task consults the tick rule");
        CHECK(strstr(task, "ESP_LOGE(TAG, RLCD_NOT_READY_MSG)") != NULL, "lcd_task logs the unready line");
        CHECK(strstr(task, "lcd_bring_up_burst()") != NULL, "lcd_task retries bring-up");
        free(task);
    }
    CHECK(strstr(src, "lcd_retry") != NULL, "retry task is created");
    char *gs = fn_body(src, "static void gather_status(void)");
    CHECK(gs != NULL, "gather_status found");
    if (gs) {
        CHECK(strstr(gs, "ric_boot_guard_decode(") != NULL, "gather_status uses ric_boot_guard_decode");
        CHECK(strstr(gs, "rlcd_classify_boot_guard(") != NULL, "gather_status classifies the record");
        CHECK(strstr(gs, "ESP_ERR_NVS_INVALID_LENGTH") != NULL, "gather_status maps the wrong-length error");
        free(gs);
    }
    // No log statement may mention the passphrase.
    int bad = 0;
    for (const char *p = src; (p = strstr(p, "ESP_LOG")) != NULL; p++) {
        const char *semi = strchr(p, ';');
        if (!semi) {
            break;
        }
        size_t n = (size_t)(semi - p);
        char *stmt = (char *)malloc(n + 1);
        memcpy(stmt, p, n);
        stmt[n] = 0;
        if (strstr(stmt, "s_net_pass") || strstr(stmt, "passphrase)")) {
            bad++;
        }
        free(stmt);
    }
    CHECK(bad == 0, "no log statement carries the passphrase");
    free(src);
}

static void test_http_source(const char *path)
{
    char *src = slurp(path);
    CHECK(src != NULL, "recovery_http.c readable");
    if (!src) {
        return;
    }
    normalize(src);
    strip_comments(src);
    char *st = fn_body(src, "static esp_err_t recovery_status_get(");
    CHECK(st != NULL, "status handler found");
    if (st) {
        CHECK(strstr(st, "\\\"lcd_ready\\\"") != NULL, "status reports lcd_ready");
        CHECK(strstr(st, "\\\"lcd_init_attempts\\\"") != NULL, "status reports lcd_init_attempts");
        CHECK(strstr(st, "\\\"lcd_draw_failures\\\"") != NULL, "status reports lcd_draw_failures");
        CHECK(strstr(st, "recovery_lcd_get_status(") != NULL, "status reads the LCD health");
        free(st);
    }
    CHECK(strstr(src, "\\\"boot_guard_record\\\"") != NULL, "status reports boot_guard_record");
    CHECK(strstr(src, "rlcd_classify_boot_guard(") != NULL, "http classifies the boot_guard record");
    free(src);
}

int main(int argc, char **argv)
{
    test_schedule();
    test_classify();
    if (argc < 3) {
        fprintf(stderr, "usage: %s <recovery_lcd.c> <recovery_http.c>\n", argv[0]);
        return 2;
    }
    test_lcd_source(argv[1]);
    test_http_source(argv[2]);
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
