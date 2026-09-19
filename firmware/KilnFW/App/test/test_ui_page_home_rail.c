// Host tests for ui_page_home_rail.c -- the pure decision/formatting seam
// behind the home dashboard's right-quarter rail (docs/UI_PLAN.md section
// 6.5) and the "Kiln: <name>" status-line suffix visibility rule (section
// 6.8 item 6).
#include "test_common.h"
#include "../drivers/ui/ui_page_home_rail.h"

#include <math.h>
#include <string.h>

void run_test_ui_page_home_rail(void)
{
    TEST_SECTION("ui_page_home_rail: kiln_suffix_visible");
    {
        TEST_CHECK(ui_page_home_kiln_suffix_visible(0) == false, "0 configs: hidden");
        TEST_CHECK(ui_page_home_kiln_suffix_visible(1) == false, "1 config: hidden");
        TEST_CHECK(ui_page_home_kiln_suffix_visible(2) == true, "2 configs: visible");
        TEST_CHECK(ui_page_home_kiln_suffix_visible(10) == true, "10 configs: visible");
    }

    TEST_SECTION("ui_page_home_rail: append_kiln_suffix");
    {
        char buf[64];

        // 0 configs -- suffix omitted entirely, base text untouched.
        strcpy(buf, "WiFi: OK");
        ui_page_home_append_kiln_suffix(buf, sizeof(buf), 0, true, "Bisque");
        TEST_CHECK(strcmp(buf, "WiFi: OK") == 0, "0 configs: base text unchanged");

        // 1 config -- still omitted, even with an active name available.
        strcpy(buf, "WiFi: OK");
        ui_page_home_append_kiln_suffix(buf, sizeof(buf), 1, true, "Bisque");
        TEST_CHECK(strcmp(buf, "WiFi: OK") == 0, "1 config: base text unchanged");

        // 2 configs, active name resolved -- must name it (not "(none)").
        strcpy(buf, "WiFi: OK");
        ui_page_home_append_kiln_suffix(buf, sizeof(buf), 2, true, "Bisque");
        TEST_CHECK(strcmp(buf, "WiFi: OK  Kiln: Bisque") == 0, "2 configs, active: names it");

        // 2 configs, nothing currently active -- falls back to "(none)".
        strcpy(buf, "WiFi: OK");
        ui_page_home_append_kiln_suffix(buf, sizeof(buf), 2, false, NULL);
        TEST_CHECK(strcmp(buf, "WiFi: OK  Kiln: (none)") == 0, "2 configs, no active: (none)");
    }

    TEST_SECTION("ui_page_home_rail: duty_pct");
    {
        TEST_CHECK(ui_page_home_rail_duty_pct(0.0f) == 0, "0.0 -> 0");
        TEST_CHECK(ui_page_home_rail_duty_pct(1.0f) == 100, "1.0 -> 100");
        TEST_CHECK(ui_page_home_rail_duty_pct(0.5f) == 50, "0.5 -> 50");
        TEST_CHECK(ui_page_home_rail_duty_pct(-0.2f) == 0, "negative clamps to 0");
        TEST_CHECK(ui_page_home_rail_duty_pct(1.5f) == 100, "over 1.0 clamps to 100");
        TEST_CHECK(ui_page_home_rail_duty_pct(NAN) == 0, "NaN -> 0");
    }

    TEST_SECTION("ui_page_home_rail: format_zone_temp");
    {
        char buf[16];
        ui_page_home_rail_format_zone_temp(false, 123.4f, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "--.-") == 0, "invalid: --.- (never a fabricated number)");

        ui_page_home_rail_format_zone_temp(true, 987.6f, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "987.6") == 0, "valid: real number");

        ui_page_home_rail_format_zone_temp(true, NAN, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "--.-") == 0, "valid flag but NaN payload still renders --.-");
    }

    TEST_SECTION("ui_page_home_rail: format_kiln_watts");
    {
        char buf[16];
        ui_page_home_rail_format_kiln_watts(false, 999.0f, buf, sizeof(buf));
        TEST_CHECK(buf[0] == '\0', "power invalid: empty string, never a fake 0 W");

        ui_page_home_rail_format_kiln_watts(true, 1234.0f, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1234 W") == 0, "power valid: real wattage");

        ui_page_home_rail_format_kiln_watts(true, 0.0f, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0 W") == 0, "power valid, genuinely zero: shows 0 W (not hidden)");
    }
}
