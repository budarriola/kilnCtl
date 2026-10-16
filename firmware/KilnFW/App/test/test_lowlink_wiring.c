// test_lowlink_wiring.c -- source-text scans pinning the UP-01, BW-01, BW-02, BW-03 fixes
// (docs/KNOWN_ISSUES.md). The files are not host-compilable; fail closed if not found.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static const char *find_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = strstr(text, sig);
    if (!s) return NULL;
    const char *open = strchr(s, '{');
    if (!open) return NULL;
    const char *close = strstr(open, "\n}");
    if (!close) return NULL;
    *out_len = (size_t)(close - open);
    return open;
}

static char *body_of(const char *rel, const char *sig)
{
    const char *cand[3];
    char a[256], b[256], c[256];
    snprintf(a, sizeof a, "../drivers/%s", rel);
    snprintf(b, sizeof b, "App/drivers/%s", rel);
    snprintf(c, sizeof c, "firmware/KilnFW/App/drivers/%s", rel);
    cand[0] = a; cand[1] = b; cand[2] = c;
    char *text = test_read_source_anchored(__FILE__, a, cand, 3);
    if (!text) return NULL;
    size_t len = 0;
    const char *body = find_body(text, sig, &len);
    char *out = NULL;
    if (body) {
        out = (char *)malloc(len + 1);
        if (out) { memcpy(out, body, len); out[len] = '\0'; }
    }
    free(text);
    return out;
}

static void check_up01(void)
{
    TEST_SECTION("UP-01: attempt_update_acquire re-checks run/interlocks after the claim");
    char *fn = body_of("net/pico_auto_update_boot.c", "attempt_update_acquire(");
    TEST_CHECK(fn != NULL, "could not locate attempt_update_acquire body");
    if (!fn) return;
    const char *claim = strstr(fn, "ota_http_update_try_begin");
    const char *recheck = claim ? strstr(claim, "relay_authority_heat_run_active") : NULL;
    TEST_CHECK(claim != NULL && recheck != NULL, "heat-run recheck must follow the claim");
    TEST_CHECK(recheck != NULL && strstr(recheck, "ota_http_check_interlocks") != NULL &&
               strstr(recheck, "if (profile_running || autotune_running || gate2 != OTA_INTERLOCK_OK)") != NULL &&
               strstr(recheck, "ota_http_update_end") != NULL,
               "recheck must re-read interlocks and release the claim on refusal");
    free(fn);
}

static void check_bw01(void)
{
    TEST_SECTION("BW-01: forget network returns the NVS save error");
    char *fn = body_of("net/wifi_prov_api.c", "esp_err_t do_forget_network(");
    TEST_CHECK(fn != NULL, "could not locate do_forget_network body");
    if (!fn) return;
    const char *save = strstr(fn, "nvs_save_saved_nets()");
    TEST_CHECK(save != NULL && strstr(save, "return err;") != NULL, "must return err after a failed save");
    free(fn);
}

static void check_bw02(void)
{
    TEST_SECTION("BW-02: AP teardown flags cleared only after set_mode(STA) succeeds");

    const char *cand[3] = {"../drivers/net/wifi_prov_link.c", "App/drivers/net/wifi_prov_link.c",
                           "firmware/KilnFW/App/drivers/net/wifi_prov_link.c"};
    char *t = test_read_source_anchored(__FILE__, cand[0], cand, 3);
    TEST_CHECK(t != NULL, "could not locate wifi_prov_link.c");
    if (!t) return;
    int n = 0;
    const char *p = t;
    while ((p = strstr(p, "esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);")) != NULL) {
        const char *clr = strstr(p, "ap_fallback_active = false");
        const char *ret = strstr(p, "will retry");
        const char *rr = ret ? strstr(ret, "return;") : NULL;
        TEST_CHECK(clr != NULL && ret != NULL && rr != NULL && rr < clr,
                   "flag clear must come after the failure early return");
        n++;
        p += 10;
    }
    TEST_CHECK(n == 2, "expected exactly two AP-teardown set_mode(STA) sites");
    free(t);
}

static void check_bw03(void)
{
    TEST_SECTION("BW-03: factory reset folds esp_wifi_restore failure into first_err");
    const char *cand[3] = {"../drivers/http/factory_reset.c", "App/drivers/http/factory_reset.c",
                           "firmware/KilnFW/App/drivers/http/factory_reset.c"};
    char *t = test_read_source_anchored(__FILE__, cand[0], cand, 3);
    TEST_CHECK(t != NULL, "could not locate factory_reset.c");
    if (!t) return;
    const char *r = strstr(t, "esp_err_t restore_err = esp_wifi_restore();");
    const char *f = r ? strstr(r, "first_err = restore_err;") : NULL;
    const char *end = r ? strstr(r, "esp_wifi_set_storage(WIFI_STORAGE_RAM)") : NULL;
    TEST_CHECK(f != NULL && end != NULL && f < end, "restore_err must be folded into first_err");
    free(t);
}

void run_test_lowlink_wiring(void)
{
    check_up01();
    check_bw01();
    check_bw02();
    check_bw03();
}
