// Host test for ui_page_temperature_safety.c -- the pure text-formatting
// seam behind UI_PLAN.md section 6.3's LCD safety-relay line, factored out
// of ui_page_temperature.c so it (and the web/LCD wording parity it claims)
// can be checked without LVGL/esp_log stubs.
#include "test_common.h"
#include "../drivers/ui/ui_page_temperature_safety.h"

#include <string.h>

void run_test_ui_page_temperature_safety(void)
{
    TEST_SECTION("ui_page_temperature_safety: three states");
    {
        char buf[24];

        ui_page_temperature_safety_text(false, false, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "Safety (K4): n/a") == 0,
                   "known=false -> n/a regardless of energized (unknown != off)");

        ui_page_temperature_safety_text(false, true, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "Safety (K4): n/a") == 0,
                   "known=false, energized=true -> still n/a (known gates everything)");

        ui_page_temperature_safety_text(true, true, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "Safety (K4): ON") == 0, "known=true, energized=true -> ON");

        ui_page_temperature_safety_text(true, false, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "Safety (K4): off") == 0, "known=true, energized=false -> off");
    }

    // NEGATIVE TEST (feedback_negative_test_every_check): prove the "unknown
    // is never off" rule is load-bearing by inlining the buggy collapse this
    // function must NOT do, and confirming that WOULD produce the wrong
    // string for the known=false case above.
    TEST_SECTION("ui_page_temperature_safety: negative test (collapsed unknown->off)");
    {
        bool known = false;
        bool energized = false;
        const char *buggy = (!known) ? (energized ? "Safety (K4): ON" : "Safety (K4): off")
                                      : (energized ? "Safety (K4): ON" : "Safety (K4): off");
        char real[24];
        ui_page_temperature_safety_text(known, energized, real, sizeof(real));
        TEST_CHECK(strcmp(buggy, real) != 0,
                   "MUTATION: a naive known-blind formatter reads 'off' for an unknown link -- "
                   "the real function must disagree with it, proving the known-gate is real");
    }
}
