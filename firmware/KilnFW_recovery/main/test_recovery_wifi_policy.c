// test_recovery_wifi_policy.c -- host test for recovery_wifi_policy.h and a
// source-order scan of recovery_wifi.c. Built and run by
// check_recovery_wifi_policy.ps1 (MSVC) as: test.exe <path to recovery_wifi.c>.
// Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_wifi_policy.h"

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
    CHECK(rwifi_may_configure_ap(0), "ESP_OK storage result allows AP bring-up");
    // ESP_FAIL (-1), ESP_ERR_INVALID_ARG (0x102), ESP_ERR_WIFI_NOT_INIT (0x3001),
    // ESP_ERR_NO_MEM (0x101) and a few arbitrary others must all refuse.
    static const int bad[] = {-1, 1, 0x101, 0x102, 0x103, 0x3001, 0x3002, 0x1100, 0x7fffffff, -0x7fffffff};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!rwifi_may_configure_ap(bad[i]), "non-ESP_OK storage result refuses AP bring-up");
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

// recovery_wifi_start() must (1) call esp_wifi_set_storage, (2) gate on
// rwifi_may_configure_ap() and return from inside that branch, and (3) only
// afterwards reach start_softap() (the sole caller of esp_wifi_set_config /
// esp_wifi_start with the passphrase).
static void test_wifi_source_order(const char *path)
{
    char *src = slurp(path);
    CHECK(src != NULL, "recovery_wifi.c readable");
    if (!src) {
        return;
    }
    strip_comments(src);
    const char *start_fn = strstr(src, "void recovery_wifi_start(void)");
    CHECK(start_fn != NULL, "recovery_wifi_start() found");
    if (!start_fn) {
        free(src);
        return;
    }
    const char *store = strstr(start_fn, "esp_wifi_set_storage(WIFI_STORAGE_RAM)");
    const char *gate = strstr(start_fn, "if (!rwifi_may_configure_ap(");
    const char *call = strstr(start_fn, "start_softap(pass)");
    CHECK(store != NULL, "esp_wifi_set_storage(RAM) is called");
    CHECK(gate != NULL, "storage result is gated by rwifi_may_configure_ap()");
    CHECK(call != NULL, "start_softap(pass) call found");
    if (store && gate && call) {
        CHECK(store < gate, "storage call precedes the gate");
        CHECK(gate < call, "gate precedes start_softap(pass)");
        const char *ret = strstr(gate, "return;");
        CHECK(ret != NULL && ret < call, "gate branch returns before start_softap(pass)");
        if (ret) {
            // Inside the refusal branch: the passphrase is wiped, the LCD state is
            // set, and the passphrase is never handed to the driver or the LCD.
            size_t n = (size_t)(ret - gate);
            char *branch = (char *)malloc(n + 1);
            memcpy(branch, gate, n);
            branch[n] = 0;
            CHECK(strstr(branch, "secure_zero(pass") != NULL, "refusal branch wipes the passphrase");
            CHECK(strstr(branch, "recovery_lcd_set_wifi_storage_fail()") != NULL,
                  "refusal branch shows WIFI STORAGE FAIL");
            CHECK(strstr(branch, "s_error = \"wifi_storage_fail\"") != NULL, "refusal branch records the error string");
            CHECK(strstr(branch, "esp_wifi_set_config") == NULL, "refusal branch never configures the AP");
            CHECK(strstr(branch, "esp_wifi_start") == NULL, "refusal branch never starts Wi-Fi");
            CHECK(strstr(branch, "recovery_lcd_set_ap") == NULL, "refusal branch never gives the LCD a passphrase");
            free(branch);
        }
    }
    // The only esp_wifi_set_config/esp_wifi_start callers must sit in start_softap(),
    // which is only reached through the gated path above.
    const char *first_cfg = strstr(src, "esp_wifi_set_config(");
    CHECK(first_cfg != NULL && first_cfg < start_fn, "esp_wifi_set_config lives in start_softap (before recovery_wifi_start)");
    CHECK(first_cfg != NULL && strstr(first_cfg + 1, "esp_wifi_set_config(") == NULL,
          "exactly one esp_wifi_set_config call");
    free(src);
}

int main(int argc, char **argv)
{
    test_policy();
    if (argc < 2) {
        fprintf(stderr, "usage: %s <recovery_wifi.c>\n", argv[0]);
        return 2;
    }
    test_wifi_source_order(argv[1]);
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
