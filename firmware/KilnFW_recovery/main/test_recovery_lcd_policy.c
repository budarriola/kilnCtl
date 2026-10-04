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
    CHECK(rlcd_rereset_allowed(true, 0), "expander error with budget left allows a re-reset");
    CHECK(rlcd_rereset_allowed(true, RLCD_MAX_RERESETS - 1), "last budgeted re-reset allowed");
    CHECK(!rlcd_rereset_allowed(true, RLCD_MAX_RERESETS), "re-reset budget exhausted");
    CHECK(!rlcd_rereset_allowed(true, RLCD_MAX_RERESETS + 5), "re-reset stays refused past the cap");
    CHECK(!rlcd_rereset_allowed(false, 0), "SPI-only failure never re-resets the expander");
    CHECK(!rlcd_rereset_allowed(false, 1), "SPI-only failure never re-resets (2)");

    // A permanent EXPANDER fault, 1 Hz bursts for an hour: re-resets stop at the cap.
    int attempts = 0, resets = 0;
    for (int tick = 0; tick < 3600; tick++) {
        for (int a = 0;; a++) {
            attempts++;
            if (a > 0 && rlcd_rereset_allowed(true, resets)) {
                resets++;
            }
            if (!rlcd_burst_continue(a + 1, false)) {
                break;
            }
        }
    }
    CHECK(attempts == 3 * 3600, "failing bursts make exactly 3 attempts each");
    CHECK(resets == RLCD_MAX_RERESETS, "an hour of expander failures re-resets only RLCD_MAX_RERESETS times");
    // A permanent SPI fault: never any re-reset.
    resets = 0;
    for (int tick = 0; tick < 100; tick++) {
        for (int a = 0;; a++) {
            if (a > 0 && rlcd_rereset_allowed(false, resets)) {
                resets++;
            }
            if (!rlcd_burst_continue(a + 1, false)) {
                break;
            }
        }
    }
    CHECK(resets == 0, "permanent SPI failure never re-resets the expander");

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
        CHECK(strstr(wrap, "s_line_errs++") != NULL, "draw_line counts a failed line");
        free(wrap);
    }
    char *ds = fn_body(src, "static void draw_status(void)");
    CHECK(ds != NULL, "draw_status found");
    if (ds) {
        CHECK(strstr(ds, "s_line_errs = 0") != NULL, "draw_status resets the failure count");
        const char *chk = strstr(ds, "rlcd_draw_ok(s_line_errs)");
        CHECK(chk != NULL, "draw_status aggregates line failures");
        CHECK(chk != NULL && strstr(chk, "s_ready = false") != NULL, "failed draw clears s_ready");
        CHECK(chk != NULL && strstr(chk, "s_draw_failures++") != NULL, "failed draw is counted");
        CHECK(strstr(ds, "RLCD_BG_INVALID") != NULL, "screen has a 'record invalid' state");
        free(ds);
    }
    char *burst = fn_body(src, "static bool lcd_bring_up_burst(void)");
    CHECK(burst != NULL, "burst function found");
    if (burst) {
        CHECK(strstr(burst, "rlcd_rereset_allowed(s_expander_err, s_rereset_count)") != NULL, "burst gates re-reset on expander error and cap");
        CHECK(strstr(burst, "s_rereset_count++") != NULL, "burst counts re-resets");
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
    CHECK(strstr(src, "&s_lcd_task") != NULL, "retry task handle is kept");
    CHECK(strstr(src, "s_expander_err = true") != NULL, "expander I2C errors are recorded");
    char *gs = fn_body(src, "static void gather_status(void)");
    CHECK(gs != NULL, "gather_status found");
    if (gs) {
        CHECK(strstr(gs, "ric_boot_guard_decode(") != NULL, "gather_status uses ric_boot_guard_decode");
        CHECK(strstr(gs, "rlcd_classify_boot_guard(") != NULL, "gather_status classifies the record");
        CHECK(strstr(gs, "ESP_ERR_NVS_INVALID_LENGTH") != NULL, "gather_status maps the wrong-length error");
        free(gs);
    }
    free(src);
}

// --- passphrase-in-log scan ---------------------------------------------------

static int is_id(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int ci_contains(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && ((s[i + k] | 0x20) == (needle[k] | 0x20))) {
            k++;
        }
        if (k == m) {
            return 1;
        }
    }
    return 0;
}

static int is_log_name(const char *s, size_t n)
{
    static const char *prefixes[] = {"ESP_LOG", "ESP_EARLY_LOG", "ESP_DRAM_LOG"};
    static const char *exact[] = {"printf", "ets_printf", "esp_rom_printf", "fprintf", "puts"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t m = strlen(prefixes[i]);
        if (n >= m && strncmp(s, prefixes[i], m) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (n == strlen(exact[i]) && strncmp(s, exact[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

// Counts log/printf calls (comments already stripped) whose argument list
// holds an identifier containing "pass" or "psk" OUTSIDE string/char literals.
// The argument list is cut at the balanced closing parenthesis, never at a ';'
// inside a literal.
static int count_passphrase_log_leaks(const char *src, const char *what)
{
    int leaks = 0;
    for (const char *p = src; *p;) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                p += (*p == '\\' && p[1]) ? 2 : 1;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (!is_id(*p) || (p > src && is_id(p[-1]))) {
            p++;
            continue;
        }
        const char *id = p;
        while (is_id(*p)) {
            p++;
        }
        if (!is_log_name(id, (size_t)(p - id))) {
            continue;
        }
        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n') {
            q++;
        }
        if (*q != '(') {
            continue;
        }
        int depth = 0;
        int leak = 0;
        for (; *q; q++) {
            if (*q == '"' || *q == '\'') {
                char qc = *q++;
                while (*q && *q != qc) {
                    q += (*q == '\\' && q[1]) ? 2 : 1;
                }
                if (!*q) {
                    break;
                }
            } else if (*q == '(') {
                depth++;
            } else if (*q == ')') {
                if (--depth == 0) {
                    break;
                }
            } else if (is_id(*q) && (q == src || !is_id(q[-1]))) {
                const char *s = q;
                while (is_id(*q)) {
                    q++;
                }
                if (ci_contains(s, (size_t)(q - s), "pass") || ci_contains(s, (size_t)(q - s), "psk")) {
                    leak = 1;
                }
                q--;
            }
        }
        if (leak) {
            fprintf(stderr, "LEAK: %s: a log/printf call carries a passphrase-named argument\n", what);
            leaks++;
        }
        p = q && *q ? q + 1 : p;
    }
    return leaks;
}

static void test_log_scan_self(void)
{
    CHECK(count_passphrase_log_leaks("ESP_LOGI(TAG, \"x %s\", s_net_pass);", "t") == 1, "scan flags s_net_pass");
    CHECK(count_passphrase_log_leaks("ESP_EARLY_LOGE(TAG, \"x %s\", pass);", "t") == 1, "scan flags EARLY log");
    CHECK(count_passphrase_log_leaks("ESP_DRAM_LOGW(TAG, \"x %s\", cfg.ap.password);", "t") == 1, "scan flags DRAM log");
    CHECK(count_passphrase_log_leaks("esp_rom_printf(\"%s\", passphrase);", "t") == 1, "scan flags esp_rom_printf");
    CHECK(count_passphrase_log_leaks("ets_printf(\"a;b\", s_net_pass);", "t") == 1, "scan ignores ; inside a literal");
    CHECK(count_passphrase_log_leaks("ESP_LOGI(TAG, \"passphrase on the LCD only\");", "t") == 0, "word in a literal is fine");
    CHECK(count_passphrase_log_leaks("snprintf(b, n, \"%s\", s_net_pass);", "t") == 0, "snprintf is not a log");
    CHECK(count_passphrase_log_leaks("ESP_LOGI(TAG, \"ok\"); x = s_net_pass;", "t") == 0, "later statement not attributed");
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
        CHECK(strstr(st, "\\\"lcd_task_stack_free_bytes\\\"") != NULL, "status reports lcd task stack margin");
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
    test_log_scan_self();
    test_lcd_source(argv[1]);
    test_http_source(argv[2]);
    // argv[1], argv[2] and every further argument (all recovery .c files): no
    // log/printf call may carry the passphrase.
    for (int i = 1; i < argc; i++) {
        char *src = slurp(argv[i]);
        CHECK(src != NULL, "source readable for the passphrase log scan");
        if (src) {
            normalize(src);
            strip_comments(src);
            CHECK(count_passphrase_log_leaks(src, argv[i]) == 0, "no log/printf call carries the passphrase");
            free(src);
        }
    }
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
