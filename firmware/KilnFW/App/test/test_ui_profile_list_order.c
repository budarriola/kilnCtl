// Host tests for ui_profile_list_order.c -- the pure stable-partition seam
// that must order LCD profile list/picker ids exactly the way main_page.html's
// orderProfilesByFavorite() orders web dashboard options (favorites first,
// each group's original relative order preserved, set equality, no drops).
#include "test_common.h"
#include "../drivers/ui/ui_profile_list_order.h"

#include <string.h>

// Test-local favorite sets, since the module takes the predicate as a
// function pointer rather than calling profiles_favorites_is() directly
// (that is the whole point -- no firmware deps at host-test level).
static const uint8_t *s_fav_set;
static size_t s_fav_set_len;

static bool fav_in_set(uint8_t id)
{
    for (size_t i = 0; i < s_fav_set_len; i++) {
        if (s_fav_set[i] == id) {
            return true;
        }
    }
    return false;
}

static bool fav_none(uint8_t id)
{
    (void)id;
    return false;
}

static bool fav_all(uint8_t id)
{
    (void)id;
    return true;
}

void run_test_ui_profile_list_order(void)
{
    TEST_SECTION("ui_profile_list_order: empty list");
    {
        uint8_t out[1] = { 0xAA };
        // count == 0 must be a safe no-op, including with NULL ids/predicate.
        ui_profile_list_order(NULL, 0, NULL, out);
        TEST_CHECK(out[0] == 0xAA, "count 0 writes nothing");

        uint8_t ids[1] = { 5 };
        ui_profile_list_order(ids, 0, fav_all, out);
        TEST_CHECK(out[0] == 0xAA, "count 0 with real ids/predicate still writes nothing");
    }

    TEST_SECTION("ui_profile_list_order: no favorites -- order unchanged");
    {
        uint8_t ids[5] = { 3, 1, 4, 1, 5 };
        uint8_t out[5] = { 0 };
        ui_profile_list_order(ids, 5, fav_none, out);
        TEST_CHECK(memcmp(ids, out, sizeof(ids)) == 0, "identity order when nothing is favorite");

        // NULL predicate must degrade the same way as fav_none (JS:
        // `(favIds || [])` on a missing/unreachable favorites endpoint).
        uint8_t out2[5] = { 0 };
        ui_profile_list_order(ids, 5, NULL, out2);
        TEST_CHECK(memcmp(ids, out2, sizeof(ids)) == 0, "NULL predicate degrades to no favorites");
    }

    TEST_SECTION("ui_profile_list_order: all favorites -- order unchanged");
    {
        uint8_t ids[4] = { 9, 2, 7, 0 };
        uint8_t out[4] = { 0 };
        ui_profile_list_order(ids, 4, fav_all, out);
        TEST_CHECK(memcmp(ids, out, sizeof(ids)) == 0, "identity order when everything is favorite");
    }

    TEST_SECTION("ui_profile_list_order: mixed order stability");
    {
        // Input order: 0 1 2 3 4 5 6 -- favorites {1,3,5}.
        // Expected (stable partition, favs.concat(rest)): 1 3 5 0 2 4 6.
        uint8_t ids[7] = { 0, 1, 2, 3, 4, 5, 6 };
        uint8_t favs[3] = { 1, 3, 5 };
        s_fav_set = favs;
        s_fav_set_len = 3;
        uint8_t out[7] = { 0 };
        ui_profile_list_order(ids, 7, fav_in_set, out);
        uint8_t expected[7] = { 1, 3, 5, 0, 2, 4, 6 };
        TEST_CHECK(memcmp(out, expected, sizeof(expected)) == 0, "favorites first, both groups stable");

        // Set equality: every input id appears exactly once in the output.
        for (size_t i = 0; i < 7; i++) {
            int count = 0;
            for (size_t j = 0; j < 7; j++) {
                if (out[j] == ids[i]) {
                    count++;
                }
            }
            TEST_CHECK(count == 1, "each id appears exactly once in the output");
        }
    }

    TEST_SECTION("ui_profile_list_order: builtins interleaved with user ids");
    {
        // User ids are 0..7 (PROFILES_MAX_COUNT), builtins are
        // PROFILE_BUILTIN_ID_BASE(128)+index. Mirrors a realistic combined
        // list: some user slots, some builtins, favorites drawn from both
        // namespaces, matching UI_PLAN.md 6.2's "ids come from
        // profiles_http_get() ... plus profiles_builtin_entry()" combined
        // list and profiles_favorites' "ANY profile can be favorited -- both
        // the user's saved slots and the shipped... catalogue entries."
        uint8_t ids[6] = { 128, 2, 129, 0, 5, 130 };
        uint8_t favs[2] = { 129, 5 };
        s_fav_set = favs;
        s_fav_set_len = 2;
        uint8_t out[6] = { 0 };
        ui_profile_list_order(ids, 6, fav_in_set, out);
        // Favorites in original relative order: 129 (idx2), 5 (idx4).
        // Rest in original relative order: 128, 2, 0, 130.
        uint8_t expected[6] = { 129, 5, 128, 2, 0, 130 };
        TEST_CHECK(memcmp(out, expected, sizeof(expected)) == 0,
                   "builtin (>=128) and user ids partition correctly when interleaved");
    }

    TEST_SECTION("ui_profile_list_order: single-element list");
    {
        uint8_t ids[1] = { 42 };
        uint8_t out[1] = { 0 };
        ui_profile_list_order(ids, 1, fav_all, out);
        TEST_CHECK(out[0] == 42, "single favorite element passes through");

        ui_profile_list_order(ids, 1, fav_none, out);
        TEST_CHECK(out[0] == 42, "single non-favorite element passes through");
    }
}
