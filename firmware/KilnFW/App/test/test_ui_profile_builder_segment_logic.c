// Host tests for ui_profile_builder_segment_logic.c (review A3: R4 captured pad unit, R5 captions).
#include "test_common.h"
#include "../drivers/ui/ui_profile_builder_segment_logic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* D2 (REVIEW_WEB4_TESTS): the page (LVGL) is not host-built, so the R4 guarantee that the done callbacks convert
 * through the captured s_pad -- never a fresh unit_pref_get() -- is pinned in source text. */
static void test_r4_done_callbacks_use_captured_pad(void)
{
    TEST_SECTION("R4 source guard: target/ramp done callbacks convert via s_pad, not unit_pref_get()");
    char path[600];
    snprintf(path, sizeof(path), "%s", __FILE__);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (!slash) { TEST_CHECK(false, "cannot derive test dir from __FILE__"); return; }
    snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", "../drivers/ui/ui_page_profile_builder_segment.c");
    FILE *f = fopen(path, "rb");
    TEST_CHECK(f != NULL, "ui_page_profile_builder_segment.c readable");
    if (!f) return;
    static char src[200000];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    src[n] = 0;
    const char *names[2] = { "static void target_done_cb(", "static void ramp_done_cb(" };
    for (int i = 0; i < 2; i++) {
        const char *b = strstr(src, names[i]);
        TEST_CHECK(b != NULL, "done callback found");
        if (!b) continue;
        const char *e = strstr(b + 1, "\nstatic void ");
        size_t len = e ? (size_t)(e - b) : strlen(b);
        char body[2048];
        if (len >= sizeof(body)) len = sizeof(body) - 1;
        memcpy(body, b, len);
        body[len] = 0;
        TEST_CHECK(strstr(body, "ui_pbs_pad_to_celsius(&s_pad") != NULL, "callback converts through the captured s_pad");
        TEST_CHECK(strstr(body, "unit_pref_get") == NULL, "callback does not re-read unit_pref_get()");
    }
}

void run_test_ui_profile_builder_segment_logic(void)
{
    test_r4_done_callbacks_use_captured_pad();
    TEST_SECTION("ui_profile_builder_segment_logic: R4 pad converts with the captured unit");
    {
        ui_pbs_pad_t pad;
        ui_pbs_pad_capture(&pad, UNIT_PREF_FAHRENHEIT);
        /* The live unit may flip while the pad is open; the pad is not re-captured,
         * so 212 typed on a Fahrenheit pad is still 100 C. */
        float c = ui_pbs_pad_to_celsius(&pad, 212.0f, UNIT_PREF_KIND_ABSOLUTE, 0.0f, 1400.0f);
        TEST_CHECK(fabsf(c - 100.0f) < 0.6f, "F pad: 212 -> 100 C");
        float r = ui_pbs_pad_to_celsius(&pad, 180.0f, UNIT_PREF_KIND_RATE, 0.0f, 1000.0f);
        TEST_CHECK(fabsf(r - 100.0f) < 0.6f, "F pad rate: 180 F/hr -> 100 C/hr (no offset)");

        ui_pbs_pad_capture(&pad, UNIT_PREF_CELSIUS);
        c = ui_pbs_pad_to_celsius(&pad, 212.0f, UNIT_PREF_KIND_ABSOLUTE, 0.0f, 1400.0f);
        TEST_CHECK(fabsf(c - 212.0f) < 0.6f, "C pad: 212 stays 212 C");
    }

    TEST_SECTION("ui_profile_builder_segment_logic: R5 captions");
    {
        char c[24];
        char f[24];
        ui_pbs_target_caption(UNIT_PREF_CELSIUS, c, sizeof(c));
        ui_pbs_target_caption(UNIT_PREF_FAHRENHEIT, f, sizeof(f));
        TEST_CHECK(strncmp(c, "Target ", 7) == 0 && strncmp(f, "Target ", 7) == 0, "target caption prefix");
        TEST_CHECK(strcmp(c, f) != 0, "target caption differs per unit");
        ui_pbs_ramp_caption(UNIT_PREF_CELSIUS, c, sizeof(c));
        ui_pbs_ramp_caption(UNIT_PREF_FAHRENHEIT, f, sizeof(f));
        TEST_CHECK(strncmp(f, "Ramp ", 5) == 0 && strstr(f, "/hr") != NULL, "ramp caption has Ramp and /hr");
        TEST_CHECK(strcmp(c, f) != 0, "ramp caption differs per unit");

        TEST_CHECK(strcmp(ui_pbs_limit_caption(1, 12), "At least one segment is required") == 0, "1 segment");
        TEST_CHECK(strcmp(ui_pbs_limit_caption(5, 12), "") == 0, "mid count blank");
        TEST_CHECK(strcmp(ui_pbs_limit_caption(12, 12), "Maximum 12 segments reached") == 0, "max reached");
    }
}
