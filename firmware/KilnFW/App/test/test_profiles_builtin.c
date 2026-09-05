// Host test for App/drivers/profiles_builtin.c -- specifically the 2026-09-05
// "Unrated" cone sentinel (PROFILES_BUILTIN_CONE_UNRATED, owner decision):
// the 10 catalogue entries whose Digital Fire source page states no cone
// number (FSCG1, FSCGB1, FSCGCL, FSCGWM, FSCRGL, FSHP1, FSHP3, FSNM5, MDDCL,
// QICA -- previously marked UNRESOLVED with an invented placeholder number
// in profiles_builtin_table.inc) now carry that sentinel instead, and must:
//   1. Print as "Unrated" via profiles_builtin_cone_label(), not a number.
//   2. Sort AFTER every real cone within the same firing-type list -- the
//      point of the sentinel is to stop pretending a number was published,
//      not to silently sort as if it were cone -128 (INT8_MIN's raw value,
//      which a plain signed compare would put FIRST, the opposite of what's
//      wanted).
//
// This file #includes profiles_builtin.c directly (own executable, same
// convention as test_profiles_http.c/test_zones_http.c) so it links the
// REAL, generated profiles_builtin_table.inc rather than a test-local fake
// catalogue -- the defect class this guards against (a sort/label bug) only
// exists against the real table's actual entries.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// stubs/nvs.h doesn't provide nvs_get_u32()/nvs_set_u32() (no other host
// test needed them before) -- profiles_builtin.c's hidden-mask persistence
// (profiles_builtin_start()/profiles_builtin_set_hidden()) calls them, but
// none of this file's tests exercise that path, so trivial always-empty /
// always-OK stand-ins are enough just to satisfy the linker.
#include "esp_err.h"
#include "nvs.h"
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

#include "profiles_builtin.c"

// Mirrors ui_page_profiles_builtin_list.c's static sort_key() -- that
// function lives in an LVGL-dependent UI file with no host-test seam, so
// this test re-expresses the same widening rule against the real table
// rather than skip verifying the invariant it exists for. If the UI file's
// sort_key() and this ever diverge, the fix is to make the UI one the single
// source (e.g. move it into profiles_builtin.c), not to relax this test.
static int16_t test_sort_key(int8_t cone)
{
    return (cone == PROFILES_BUILTIN_CONE_UNRATED) ? INT16_MAX : (int16_t)cone;
}

static const char *UNRATED_CODES[] = {
    "FSCG1", "FSCGB1", "FSCGCL", "FSCGWM", "FSCRGL",
    "FSHP1", "FSHP3", "FSNM5", "MDDCL", "QICA",
};
#define UNRATED_CODE_COUNT (sizeof(UNRATED_CODES) / sizeof(UNRATED_CODES[0]))

static bool code_is_expected_unrated(const char *code)
{
    for (size_t i = 0; i < UNRATED_CODE_COUNT; i++) {
        if (strcmp(code, UNRATED_CODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void test_cone_label_unrated(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- PROFILES_BUILTIN_CONE_UNRATED prints \"Unrated\", "
                 "not a number");
    char buf[8];
    profiles_builtin_cone_label(PROFILES_BUILTIN_CONE_UNRATED, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "Unrated") == 0, "sentinel prints as the literal word \"Unrated\"");
}

static void test_cone_label_real_cone_unaffected(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- a real low-fire cone (-4, i.e. cone04) still prints "
                 "\"04\", unaffected by the sentinel branch");
    char buf[8];
    profiles_builtin_cone_label(-4, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "04") == 0, "existing negative-encoding printed form is untouched");
}

static void test_exactly_the_ten_named_entries_are_unrated(void)
{
    TEST_SECTION("g_builtin_profiles -- exactly the 10 named codes carry the Unrated sentinel; every "
                 "other entry keeps a real (non-sentinel) cone");
    size_t found_unrated = 0;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *b = &g_builtin_profiles[i];
        bool expected = code_is_expected_unrated(b->code);
        bool actual = (b->cone == PROFILES_BUILTIN_CONE_UNRATED);
        if (expected) {
            found_unrated++;
        }
        TEST_CHECK(expected == actual,
                   "entry's sentinel state matches the owner's named list (code follows in next check)");
    }
    TEST_CHECK(found_unrated == UNRATED_CODE_COUNT,
               "all 10 named codes were actually present in the table and matched");
}

static void test_unrated_sorts_after_every_real_cone_same_firing_type(void)
{
    TEST_SECTION("sort key -- NEGATIVE CHECK: a real cone must sort BEFORE Unrated within the same "
                 "firing-type group, not after (a naive plain-int8_t compare would put INT8_MIN "
                 "first, which is backwards)");
    // All 10 Unrated entries are PROFILE_FIRING_GLAZE or PROFILE_FIRING_OTHER
    // (see profiles_builtin_table.inc). Cross-check against a real cone in
    // each of those two groups.
    bool any_glaze_unrated_checked = false;
    bool any_other_unrated_checked = false;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *u = &g_builtin_profiles[i];
        if (u->cone != PROFILES_BUILTIN_CONE_UNRATED) {
            continue;
        }
        for (size_t j = 0; j < g_builtin_profile_count; j++) {
            const builtin_profile_t *r = &g_builtin_profiles[j];
            if (r->firing_type != u->firing_type || r->cone == PROFILES_BUILTIN_CONE_UNRATED) {
                continue;
            }
            TEST_CHECK(test_sort_key(r->cone) < test_sort_key(u->cone),
                       "a real cone in the same firing-type group sorts strictly before Unrated");
            if (u->firing_type == PROFILE_FIRING_GLAZE) {
                any_glaze_unrated_checked = true;
            } else if (u->firing_type == PROFILE_FIRING_OTHER) {
                any_other_unrated_checked = true;
            }
            break; // one real comparison per Unrated entry is enough
        }
    }
    TEST_CHECK(any_glaze_unrated_checked, "at least one Glaze Unrated entry was actually compared "
                                          "(else the loop above silently checked nothing)");
    TEST_CHECK(any_other_unrated_checked, "at least one Other Unrated entry was actually compared");
}

void run_test_profiles_builtin(void)
{
    test_cone_label_unrated();
    test_cone_label_real_cone_unaffected();
    test_exactly_the_ten_named_entries_are_unrated();
    test_unrated_sorts_after_every_real_cone_same_firing_type();
}

int main(void)
{
    run_test_profiles_builtin();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
