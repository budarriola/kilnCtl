// test_recovery_wifi_policy.c -- host test for recovery_wifi_policy.h and a
// source-order scan of recovery_wifi.c. Built and run by
// check_recovery_wifi_policy.ps1 (MSVC) as: test.exe <path to recovery_wifi.c>.
// Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_wifi_policy.h"
#include "recovery_http_policy.h"

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
        // CRLF checkouts: the scans below use LF needles.
        size_t w = 0;
        for (size_t r = 0; r < got; r++) {
            if (buf[r] != '') {
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

// Body of the function whose definition starts with `sig`, up to the next
// line-start "}" (all handlers here are top-level, closing brace at column 0).
static char *fn_body(const char *src, const char *sig)
{
    const char *a = strstr(src, sig);
    if (!a) {
        return NULL;
    }
    const char *e = strstr(a, "\n}");
    if (!e) {
        return NULL;
    }
    size_t n = (size_t)(e - a);
    char *b = (char *)malloc(n + 1);
    memcpy(b, a, n);
    b[n] = 0;
    return b;
}

static void test_http(const char *path)
{
    CHECK(rhp_wifi_reset_ok(0, 0, 0), "wifi reset ok when everything succeeded");
    CHECK(!rhp_wifi_reset_ok(1, 0, 0), "wifi reset not ok on key erase failure");
    CHECK(!rhp_wifi_reset_ok(0, -1, 0), "wifi reset not ok on commit failure");
    CHECK(!rhp_wifi_reset_ok(0, 0, 0x105), "wifi reset not ok when legacy erase failed");

    char *src = slurp(path);
    CHECK(src != NULL, "recovery_http.c readable");
    if (!src) {
        return;
    }
    strip_comments(src);
    // R2-L1: exit and boot_guard_reset treat a failed kiln_nvs as not applicable,
    // and a failed clear refuses (500) -- the result must gate a 500 reply, not
    // just be called.
    const char *sigs[] = {"esp_err_t recovery_exit_post(", "esp_err_t boot_guard_reset_post("};
    for (int i = 0; i < 2; i++) {
        char *b = fn_body(src, sigs[i]);
        CHECK(b != NULL, "boot_guard route handler found");
        if (b) {
            const char *gate = strstr(b, "if (!boot_guard_clear_or_na(bg_msg, sizeof(bg_msg))) {");
            CHECK(gate != NULL, "route gates on the clear result (if (!boot_guard_clear_or_na(...)))");
            if (gate) {
                const char *st = strstr(gate, "500 Internal Server Error");
                const char *close = strstr(gate, "\n    }");
                CHECK(st != NULL && close != NULL && st < close, "a failed clear answers 500 inside the gate branch");
                const char *ok = i == 0 ? strstr(b, "ok, rebooting into the application; %s\", bg_msg)")
                                        : strstr(b, "return httpd_resp_sendstr(req, bg_msg);");
                CHECK(ok != NULL && close != NULL && ok > close,
                      "success reply follows the gate and carries the clear message");
            }
            CHECK(strstr(b, " clear_boot_guard(") == NULL && strstr(b, "!clear_boot_guard(") == NULL,
                  "route does not call plain clear_boot_guard()");
            free(b);
        }
    }
    // R2-I1: ota_esp_post clears (gated) BEFORE the upload overwrites `app`.
    char *o = fn_body(src, "esp_err_t ota_esp_post(");
    CHECK(o != NULL, "ota_esp_post found");
    if (o) {
        const char *g = strstr(o, "if (!boot_guard_clear_or_na(bg_msg, sizeof(bg_msg))) {");
        const char *u = strstr(o, "recovery_upload_stream(");
        CHECK(g != NULL && u != NULL && g < u, "ota_esp_post clears boot_guard (gated) before recovery_upload_stream()");
        if (g && u) {
            const char *st = strstr(g, "500 Internal Server Error");
            CHECK(st != NULL && st < u, "ota_esp_post refuses 500 on a failed clear before the upload");
        }
        free(o);
    }
    // boot_guard_clear_or_na: only the kiln_nvs failure bit makes "not applicable"
    // true; everything else must actually clear.
    char *na = fn_body(src, "static bool boot_guard_clear_or_na(char *msg, size_t cap)\n{");
    CHECK(na != NULL, "boot_guard_clear_or_na definition found");
    if (na) {
        CHECK(strstr(na, "if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN) {") != NULL,
              "not-applicable is gated on RECOVERY_NVS_FAIL_KILN");
        CHECK(strstr(na, "RECOVERY_NVS_FAIL_DEFAULT") == NULL && strstr(na, "RECOVERY_NVS_FAIL_WIFI") == NULL,
              "not-applicable is not gated on another partition's bit");
        const char *r1 = strstr(na, "return true;");
        const char *r2 = strstr(na, "return clear_boot_guard(msg, cap);");
        CHECK(r1 != NULL && r2 != NULL && r1 < r2 && strstr(r1 + 1, "return true;") == NULL,
              "exactly one 'return true' (the not-applicable branch), then the real clear is returned");
        free(na);
    }
    // R2-L2: the Wi-Fi reset also erases the legacy default-partition copy and
    // reports success only through rhp_wifi_reset_ok(), using the real result.
    char *w = fn_body(src, "esp_err_t wifi_reset_post(");
    CHECK(w != NULL, "wifi_reset_post found");
    if (w) {
        const char *call = strstr(w, "int legacy_rc = erase_legacy_default_wifi(&legacy_skipped);");
        CHECK(call != NULL, "wifi reset stores the legacy erase result in legacy_rc");
        CHECK(call != NULL && strstr(call + 8, "legacy_rc =") == NULL && strstr(w, "legacy_rc =") == call + 4,
              "legacy_rc is assigned exactly once, from the erase");
        const char *gate = strstr(w, "if (!rhp_wifi_reset_ok(erase_failed, (int)err, legacy_rc) && erase_failed == 0 && err == ESP_OK) {");
        CHECK(gate != NULL && call != NULL && call < gate, "failure branch is taken on !rhp_wifi_reset_ok(..., legacy_rc), after the erase");
        if (gate) {
            const char *st = strstr(gate, "500 Internal Server Error");
            const char *ok = strstr(w, "ok, Wi-Fi settings cleared, restarting\"");
            CHECK(st != NULL && ok != NULL && st < ok, "legacy erase failure answers 500 before the success reply");
        }
        CHECK(strstr(w, "legacy copy not checked") != NULL && strstr(w, "httpd_resp_sendstr(req, legacy_skipped") != NULL,
              "reply says when the legacy erase was skipped");
        free(w);
    }
    char *l = fn_body(src, "static int erase_legacy_default_wifi(");
    CHECK(l != NULL, "legacy erase found");
    if (l) {
        const char *rw = strstr(l, "nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h)");
        const char *er = strstr(l, "err = nvs_erase_all(h);");
        const char *cm = strstr(l, "err = nvs_commit(h);");
        const char *cl = strstr(l, "nvs_close(h);\n    return (int)err;");
        CHECK(rw && er && cm && rw < er && er < cm, "legacy erase: open read-write, erase_all, then commit");
        CHECK(er != NULL && cm != NULL && strstr(er, "if (err == ESP_OK) {") != NULL &&
                  strstr(er, "if (err == ESP_OK) {") < cm,
              "legacy erase: commit only when erase_all succeeded");
        CHECK(cl != NULL && cm != NULL && cm < cl, "legacy erase returns the erase/commit result");
        free(l);
    }
    free(src);
}

int main(int argc, char **argv)
{
    test_policy();
    if (argc < 3) {
        fprintf(stderr, "usage: %s <recovery_wifi.c> <recovery_http.c>\n", argv[0]);
        return 2;
    }
    test_wifi_source_order(argv[1]);
    test_http(argv[2]);
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
