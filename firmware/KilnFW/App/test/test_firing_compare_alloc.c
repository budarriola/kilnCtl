// Host test for firing_compare()'s heap-allocation failure path
// (ITER_TUNE_REDESIGN_PLAN.md step 8 follow-up: raw[]/norm[]/in_band[] moved
// off the profile_executor task's stack into one malloc each, because
// firing_shadow_finish_firing() made firing_compare() reachable from that
// stack). Proves, for a failure at EACH of the three allocations:
//   - the verdict is FIRING_COMPARE_NO_MATCHED_PAIRS (never ACCEPT), and it
//     is also what lands in *out;
//   - every block that WAS allocated is freed (no leak on the early return);
// and, with no failure injected, that the normal path still reaches a real
// verdict and frees all three blocks.
//
// Its own executable: it #includes firing_compare.c with malloc/free
// redirected to counting fakes, so it cannot share an executable that links
// the real firing_compare.c (the main host-test exe does).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h> // before the redirect below -- keeps the real prototypes intact
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

static int s_alloc_calls;
static int s_fail_at;     // 1-based allocation to fail; 0 = never
static int s_live_blocks;

static void *fc_test_malloc(size_t n)
{
    s_alloc_calls++;
    if (s_alloc_calls == s_fail_at) return NULL;
    void *p = malloc(n);
    if (p) s_live_blocks++;
    return p;
}

static void fc_test_free(void *p)
{
    if (p) s_live_blocks--;
    free(p);
}

#define malloc fc_test_malloc
#define free fc_test_free
#include "../drivers/control/firing_compare.c"
#undef malloc
#undef free

// One matched ramp class, every sub-score present, trial slightly better.
static void build_sets(firing_score_set_t *base, firing_score_set_t *trial)
{
    memset(base, 0, sizeof(*base));
    firing_segment_score_t *e = &base->entry[0];
    e->key.zone_index = 0;
    e->key.kind = 0;
    e->key.rate_bucket = 2;
    e->key.temp_bucket = 1;
    for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
        e->has[s] = true;
        e->value[s] = 2.0f;
    }
    e->in_band_frac = 0.8f;
    e->rate_c_per_s = 120.0f / 3600.0f;
    e->scored_ticks = 200;
    e->merged = 1;
    base->count = 1;

    *trial = *base;
    for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
        trial->entry[0].value[s] = 1.9f;
    }
}

static void test_each_allocation_failure_fails_safe_without_leaking(void)
{
    TEST_SECTION("firing_compare() -- a malloc failure at any of its three allocations returns "
                 "NO_MATCHED_PAIRS and frees whatever was already allocated");
    firing_score_set_t base, trial;
    build_sets(&base, &trial);

    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        s_alloc_calls = 0;
        s_live_blocks = 0;
        s_fail_at = fail_at;
        firing_compare_result_t r;
        memset(&r, 0xA5, sizeof(r));
        firing_compare_verdict_t v = firing_compare(&base, &trial, NULL, &r);
        char msg[160];
        snprintf(msg, sizeof(msg), "allocation %d failing -> NO_MATCHED_PAIRS", fail_at);
        TEST_CHECK(v == FIRING_COMPARE_NO_MATCHED_PAIRS, msg);
        snprintf(msg, sizeof(msg), "allocation %d failing -> *out carries the same verdict", fail_at);
        TEST_CHECK(r.verdict == FIRING_COMPARE_NO_MATCHED_PAIRS, msg);
        snprintf(msg, sizeof(msg), "allocation %d failing -> no block leaked", fail_at);
        TEST_CHECK(s_live_blocks == 0, msg);
    }
}

static void test_success_path_reaches_a_verdict_and_frees_everything(void)
{
    TEST_SECTION("firing_compare() -- with every allocation succeeding, a matched class reaches a "
                 "real verdict and all three blocks are freed");
    firing_score_set_t base, trial;
    build_sets(&base, &trial);
    s_alloc_calls = 0;
    s_live_blocks = 0;
    s_fail_at = 0;
    firing_compare_result_t r;
    firing_compare_verdict_t v = firing_compare(&base, &trial, NULL, &r);
    TEST_CHECK(v != FIRING_COMPARE_NO_MATCHED_PAIRS, "a matched class is not reported as NO_MATCHED_PAIRS");
    TEST_CHECK(r.matched_classes == 1, "exactly one class matched");
    TEST_CHECK(s_alloc_calls == 3, "exactly three allocations");
    TEST_CHECK(s_live_blocks == 0, "all three blocks freed on the normal return");
}

int main(void)
{
    TEST_SECTION("firing_compare_alloc");
    test_each_allocation_failure_fails_safe_without_leaking();
    test_success_path_reaches_a_verdict_and_frees_everything();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
