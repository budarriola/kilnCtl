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

static void check_oa02(void)
{
    TEST_SECTION("OA-02: Pico auto-update waits for the ESP image to be VALID");
    char *w = body_of("net/pico_auto_update_boot.c", "wait_for_esp_image_confirmed(void)");
    TEST_CHECK(w != NULL, "could not locate wait_for_esp_image_confirmed body");
    if (w) {
        TEST_CHECK(strstr(w, "esp_ota_get_state_partition") != NULL && strstr(w, "ESP_OTA_IMG_VALID") != NULL,
                   "gate must read the otadata state and require VALID");
        free(w);
    }
    char *t = body_of("net/pico_auto_update_boot.c", "pico_auto_update_task(void *arg)");
    TEST_CHECK(t != NULL, "could not locate pico_auto_update_task body");
    if (t) {
        const char *off = strstr(t, "goto done;");
        const char *gate = strstr(t, "wait_for_esp_image_confirmed();");
        const char *use = strstr(t, "pico_image_embedded_describe");
        TEST_CHECK(off != NULL && gate != NULL && use != NULL && off < gate && gate < use,
                   "gate must run after the compiled-off exit and before any image/decision work");
        free(t);
    }
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

static void check_pm(void)
{
    TEST_SECTION("PM-03/04/06: scratch fallback logged, LVGL create retried, OTA part primed");
    char *fn = body_of("persist/persist_scratch.h", "static inline void *persist_scratch_alloc(");
    TEST_CHECK(fn != NULL && strstr(fn, "s_fallbacks++") != NULL && strstr(fn, "ESP_LOGW") != NULL,
               "persist_scratch_alloc must count and log the internal fallback");
    free(fn);
    char *t = body_of("ui/ui_page_diagnostics.c", "void ui_page_diagnostics_prime_ota_state(void)");
    TEST_CHECK(t != NULL && strstr(t, "hal_sysinfo_get_running_partition") != NULL,
               "prime must read the running partition");
    free(t);
    t = body_of("ui/ui_page_diagnostics.c", "static void build_firmware_statics(void)");
    TEST_CHECK(t != NULL && strstr(t, "hal_sysinfo_get_running_partition") == NULL,
               "LVGL builder must not call hal_sysinfo_get_running_partition");
    free(t);
}

static void check_lcd_kiln_nvs(void)
{
    TEST_SECTION("LCD diagnostics NVS row reads the kiln_nvs partition and says so");
    char *t = body_of("ui/ui_page_diagnostics.c", "static void build_nvs_statics(void)");
    TEST_CHECK(t != NULL && strstr(t, "hal_kv_stats(\"kiln_nvs\", &stats)") != NULL,
               "build_nvs_statics must call hal_kv_stats(\"kiln_nvs\", ...)");
    free(t);
    const char *cand[3] = {"../drivers/ui/ui_page_diagnostics.c", "App/drivers/ui/ui_page_diagnostics.c",
                           "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c"};
    char *f = test_read_source_anchored(__FILE__, cand[0], cand, 3);
    TEST_CHECK(f != NULL && strstr(f, "build_stat_row(psram_page, \"kiln_nvs used/free/total\"") != NULL,
               "row label must name the kiln_nvs partition");
    free(f);
}

void run_test_lowlink_wiring(void)
{
    check_lcd_kiln_nvs();
    check_pm();
    check_up01();
    check_oa02();
    check_bw01();
    check_bw02();
    check_bw03();
}
