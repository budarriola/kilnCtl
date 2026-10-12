// Host tests for ui_page_home_rail.c -- the pure decision/formatting seam
// behind the home dashboard's right-quarter rail (docs/UI_PLAN.md section
// 6.5) and the "Kiln: <name>" status-line suffix visibility rule (section
// 6.8 item 6).
#include "test_common.h"
#include "../drivers/ui/ui_page_home_rail.h"
#include "../drivers/ui/ui_unit_entry.h"

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

    TEST_SECTION("ui_page_home_rail: pill_on / aux_caption (spare-relay WP-6)");
    {
        TEST_CHECK(ui_page_home_rail_pill_on(true, true) == true, "good read + on: lit");
        TEST_CHECK(ui_page_home_rail_pill_on(true, false) == false, "good read + off: dark");
        TEST_CHECK(ui_page_home_rail_pill_on(false, true) == false, "failed/absent read never lights a pill");

        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0x00, 0), "") == 0, "no aux: zone relay stays caption-free");
        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0x0F, 0), "A1") == 0, "relay 0 aux -> A1");
        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0x0F, 3), "A4") == 0, "relay 3 aux -> A4");
        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0x04, 1), "") == 0, "only the enabled relay is captioned");
        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0x04, 2), "A3") == 0, "bit 2 -> relay 2 -> A3");
        TEST_CHECK(strcmp(ui_page_home_rail_aux_caption(0xFF, 4), "") == 0, "out-of-range relay index: empty");
        TEST_CHECK(ui_page_home_rail_aux_caption(0x0F, 0) != NULL, "never NULL");
    }

    TEST_SECTION("ui_page_home_rail: text_changed (write-on-change, L30)");
    {
        TEST_CHECK(ui_page_home_rail_text_changed("Zone 1", "Zone 1") == false, "same text: no write");
        TEST_CHECK(ui_page_home_rail_text_changed("Zone 1", "Zone 2") == true, "different text: write");
        TEST_CHECK(ui_page_home_rail_text_changed("", "x") == true, "empty to text: write");
        TEST_CHECK(ui_page_home_rail_text_changed(NULL, "x") == true, "NULL current: write");
        TEST_CHECK(ui_page_home_rail_text_changed("x", NULL) == true, "NULL next: write");
        TEST_CHECK(UI_PAGE_HOME_RAIL_AUX_CAPTION_COUNT == 4u, "caption table covers 4 relays");
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
        ui_page_home_rail_format_zone_temp(false, false, 123.4f, UNIT_PREF_CELSIUS, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "--.-") == 0, "invalid: --.- (never a fabricated number)");

        ui_page_home_rail_format_zone_temp(true, false, 987.6f, UNIT_PREF_CELSIUS, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "988C") == 0, "valid Celsius: real number with unit");

        ui_page_home_rail_format_zone_temp(true, false, 1000.0f, UNIT_PREF_FAHRENHEIT, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1832F") == 0, "Fahrenheit preference converts and shows F (N3)");

        ui_page_home_rail_format_zone_temp(true, true, 1000.0f, UNIT_PREF_CELSIUS, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "--.-") == 0, "stale reading hidden like the Temperature page (N4)");

        ui_page_home_rail_format_zone_temp(true, false, NAN, UNIT_PREF_CELSIUS, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "--.-") == 0, "valid flag but NaN payload still renders --.-");
    }

    TEST_SECTION("ui_unit_entry: pad conversion (LCD review N8)");
    {
        TEST_CHECK(ui_unit_entry_to_display(1000.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE) == 1832.0f,
                   "1000 C shows as 1832 F on the pad");
        TEST_CHECK(ui_unit_entry_to_display(100.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_RATE) == 180.0f,
                   "rate has no +32");
        float c = ui_unit_entry_to_celsius(1832.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE, 0.0f, 1400.0f);
        TEST_CHECK(fabsf(c - 1000.0f) < 0.01f, "1832 F stores 1000 C");
        c = ui_unit_entry_to_celsius(180.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_RATE, 0.0f, 1000.0f);
        TEST_CHECK(fabsf(c - 100.0f) < 0.01f, "180 F/hr stores 100 C/hr");
        c = ui_unit_entry_to_celsius(1234.0f, UNIT_PREF_CELSIUS, UNIT_PREF_KIND_ABSOLUTE, 0.0f, 1400.0f);
        TEST_CHECK(c == 1234.0f, "Celsius pref is identity");
        c = ui_unit_entry_to_celsius(9999.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE, 0.0f, 1400.0f);
        TEST_CHECK(c == 1400.0f, "result clamped to the Celsius bound");
        c = ui_unit_entry_to_celsius(-500.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE, 20.0f, 1400.0f);
        TEST_CHECK(c == 20.0f, "unit_entry_lower_clamp: result clamped to the Celsius minimum");
        TEST_CHECK(ui_unit_entry_to_display(100.3f, UNIT_PREF_CELSIUS, UNIT_PREF_KIND_ABSOLUTE) == 100.0f,
                   "unit_entry_display_round: 100.3 rounds down");
        TEST_CHECK(ui_unit_entry_to_display(100.6f, UNIT_PREF_CELSIUS, UNIT_PREF_KIND_ABSOLUTE) == 101.0f,
                   "unit_entry_display_round: 100.6 rounds up");
        /* A4: Fahrenheit clamp and rounding. */
        c = ui_unit_entry_to_celsius(1.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_RATE, 50.0f, 1000.0f);
        TEST_CHECK(c == 50.0f, "unit_entry_f_rate_lower_clamp: 1 F/hr clamps to the rate minimum");
        c = ui_unit_entry_to_celsius(9999.0f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_RATE, 0.0f, 1000.0f);
        TEST_CHECK(c == 1000.0f, "unit_entry_f_rate_upper_clamp: huge F/hr clamps to the rate maximum");
        /* 100 C = 212 F exactly; 100.3 C = 212.54 F -> 213; 100.2 C = 212.36 F -> 212. */
        TEST_CHECK(ui_unit_entry_to_display(100.3f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE) == 213.0f,
                   "unit_entry_f_display_round: 212.54 F rounds up to 213");
        TEST_CHECK(ui_unit_entry_to_display(100.2f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_ABSOLUTE) == 212.0f,
                   "unit_entry_f_display_round: 212.36 F rounds down to 212");
        TEST_CHECK(ui_unit_entry_to_display(100.3f, UNIT_PREF_FAHRENHEIT, UNIT_PREF_KIND_RATE) == 181.0f,
                   "unit_entry_f_rate_round: 180.54 F/hr rounds up to 181");
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
