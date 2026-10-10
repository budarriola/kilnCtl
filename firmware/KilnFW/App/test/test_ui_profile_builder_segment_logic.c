// Host tests for ui_profile_builder_segment_logic.c (review A3: R4 captured pad unit, R5 captions).
#include "test_common.h"
#include "../drivers/ui/ui_profile_builder_segment_logic.h"

#include <math.h>
#include <string.h>

void run_test_ui_profile_builder_segment_logic(void)
{
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
