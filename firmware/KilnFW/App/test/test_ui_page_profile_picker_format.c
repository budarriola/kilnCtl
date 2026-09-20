// Host tests for ui_page_profile_picker_format.c -- UI_PLAN.md Section 6.2's
// "tests owed" items (c) and (d): the favorite star is a display-only label
// prefix (never folded into the id), and a builtin id is never deletable.
#include "test_common.h"
#include "../drivers/persist/profiles_types.h" /* PROFILES_MAX_COUNT */
#include "../drivers/ui/ui_page_profile_picker_format.h"

#include <string.h>

void run_test_ui_page_profile_picker_format(void)
{
    TEST_SECTION("ui_page_profile_picker_format_label: star only in the label");
    {
        char buf[32];

        ui_page_profile_picker_format_label("Cone 6 Glaze", false, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "Cone 6 Glaze") == 0, "non-favorite: no star prefix");

        ui_page_profile_picker_format_label("Cone 6 Glaze", true, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "* Cone 6 Glaze") == 0, "favorite: star-space prefix, name unchanged");

        // Empty/NULL name must not crash and must still carry the star alone.
        ui_page_profile_picker_format_label(NULL, true, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "* ") == 0, "NULL name degrades to empty string, star still applied");

        ui_page_profile_picker_format_label("", false, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "") == 0, "empty name, non-favorite: empty label");

        // Truncation is safe (snprintf-backed), never an overflow.
        char small[4];
        ui_page_profile_picker_format_label("LongProfileName", true, small, sizeof(small));
        TEST_CHECK(strlen(small) == 3, "truncated label still NUL-terminated within out_cap");
    }

    TEST_SECTION("ui_page_profile_picker_format_label: out_cap/out guards");
    {
        char buf[8] = "unset";
        ui_page_profile_picker_format_label("X", true, NULL, sizeof(buf));
        // No crash is the assertion here; nothing else to check with out == NULL.
        ui_page_profile_picker_format_label("X", true, buf, 0);
        TEST_CHECK(strcmp(buf, "unset") == 0, "out_cap == 0 writes nothing");
    }

    TEST_SECTION("ui_page_profile_picker_is_deletable: user slots yes, builtins no");
    {
        TEST_CHECK(ui_page_profile_picker_is_deletable(0) == true, "user slot 0 is deletable");
        TEST_CHECK(ui_page_profile_picker_is_deletable(PROFILES_MAX_COUNT - 1) == true,
                   "last user slot (PROFILES_MAX_COUNT-1) is deletable");
        TEST_CHECK(ui_page_profile_picker_is_deletable(PROFILES_MAX_COUNT) == false,
                   "id PROFILES_MAX_COUNT, one past the last user slot, is not deletable");
        TEST_CHECK(ui_page_profile_picker_is_deletable(127) == false,
                   "id 127, just below PROFILE_BUILTIN_ID_BASE, is not deletable -- the header's contract is "
                   "PROFILES_MAX_COUNT, not PROFILE_BUILTIN_ID_BASE");
        TEST_CHECK(ui_page_profile_picker_is_deletable(128) == false, "PROFILE_BUILTIN_ID_BASE is not deletable");
        TEST_CHECK(ui_page_profile_picker_is_deletable(155) == false, "a builtin id well past the base is not deletable");
        TEST_CHECK(ui_page_profile_picker_is_deletable(255) == false, "the top of the uint8_t range is not deletable");
    }

    TEST_SECTION("ui_page_profile_picker_format_page_count: 0, 1, 4, 5, 101 entries at 4 rows/page");
    {
        TEST_CHECK(ui_page_profile_picker_format_page_count(0, 4) == 1, "0 entries: still 1 page, not 0");
        TEST_CHECK(ui_page_profile_picker_format_page_count(1, 4) == 1, "1 entry: 1 page");
        TEST_CHECK(ui_page_profile_picker_format_page_count(4, 4) == 1, "4 entries: exactly fills 1 page");
        TEST_CHECK(ui_page_profile_picker_format_page_count(5, 4) == 2, "5 entries: spills into a 2nd page");
        TEST_CHECK(ui_page_profile_picker_format_page_count(101, 4) == 26, "101 entries: ceil(101/4) == 26 pages");
    }

    TEST_SECTION("ui_page_profile_picker_format_row_index: page * rows_per_page + slot");
    {
        TEST_CHECK(ui_page_profile_picker_format_row_index(0, 0, 4) == 0, "page 0 slot 0 -> index 0");
        TEST_CHECK(ui_page_profile_picker_format_row_index(0, 3, 4) == 3, "page 0 slot 3 -> index 3");
        TEST_CHECK(ui_page_profile_picker_format_row_index(1, 0, 4) == 4, "page 1 slot 0 -> index 4");
        TEST_CHECK(ui_page_profile_picker_format_row_index(25, 0, 4) == 100, "page 25 slot 0 -> index 100 (last page of 101)");
        TEST_CHECK(ui_page_profile_picker_format_row_index(25, 3, 4) == 103,
                   "page 25 slot 3 -> index 103, past id_count for a 101-entry list -- the caller (render()) is the "
                   "one that must bounds-check against id_count, this helper only computes the arithmetic");
    }

    TEST_SECTION("ui_page_profile_picker_format_clamp_page: page clamp after a delete shrinks id_count");
    {
        // 101 entries -> 26 pages (0..25). Sitting on the last page, a delete
        // drops the count to 100 -> still 25 pages (0..24) -- page 25 must
        // clamp down to 24, the new last page.
        TEST_CHECK(ui_page_profile_picker_format_clamp_page(25, 100, 4) == 24,
                   "last page clamps down by exactly one page when a delete removes the tail's only entry");

        // Deleting far more so only 1 entry is left: page must clamp all the
        // way back to page 0, the only page left.
        TEST_CHECK(ui_page_profile_picker_format_clamp_page(25, 1, 4) == 0,
                   "page clamps all the way to 0 when almost everything was deleted");

        // A page that is still valid for the new count is left untouched.
        TEST_CHECK(ui_page_profile_picker_format_clamp_page(0, 4, 4) == 0, "still-valid page 0 is not moved");
        TEST_CHECK(ui_page_profile_picker_format_clamp_page(1, 5, 4) == 1, "still-valid page 1 (2 pages) is not moved");

        // Deleting everything: id_count == 0 must still resolve to the single
        // "empty page" (page 0), never underflow.
        TEST_CHECK(ui_page_profile_picker_format_clamp_page(3, 0, 4) == 0,
                   "id_count == 0 clamps to page 0, not an underflowed page count");
    }
}
