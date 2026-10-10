// Host tests for App/drivers/update/update_fetch_heap.c (owner floor: internal min_free >= 8192 B).
#include "test_common.h"

#include "../drivers/update/update_fetch_heap.h"

static void test_admit(void)
{
    TEST_SECTION("update_fetch_heap -- admission");
    TEST_CHECK(FETCH_HEAP_PRECHECK_MIN == 29556u && FETCH_HEAP_WORST_DRAW_BYTES == 17524u && FETCH_HEAP_SLACK_BYTES == 3840u, "precheck is 8192+17524+3840");
    TEST_CHECK(FETCH_HEAP_SCRATCH_BYTES <= 4096u, "stager scratch is at most 4 KiB (MED-1)");
    TEST_CHECK(FETCH_HEAP_WORST_DRAW_BYTES == 16500u + FETCH_HEAP_SCRATCH_BYTES, "scratch is counted in the worst draw");
    // The derivation: floor + worst draw leaves the owner floor intact.
    TEST_CHECK(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_WORST_DRAW_BYTES >= FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_SLACK_BYTES,
               "after the worst draw, floor plus the concurrent margin remain");
    TEST_CHECK(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_WORST_DRAW_BYTES >= FETCH_HEAP_FLOOR_BYTES,
               "admitted free minus worst draw stays at or above 8192");
    // Idle bench numbers (logs/sk04_sampling/2026-10-06.tsv) must pass.
    TEST_CHECK(update_fetch_heap_admit(29647u, 9728u) == FETCH_HEAP_OK, "idle minimum admitted");
    TEST_CHECK(update_fetch_heap_admit(29815u, 9728u) == FETCH_HEAP_OK, "bench idle 29815 B admitted");
    TEST_CHECK(29647u - FETCH_HEAP_WORST_DRAW_BYTES - FETCH_HEAP_FLOOR_BYTES == 3931u, "idle-min margin over floor is 3931 B");
    TEST_CHECK(update_fetch_heap_admit(31123u, 9728u) == FETCH_HEAP_OK, "idle maximum admitted");
    TEST_CHECK(update_fetch_heap_admit(FETCH_HEAP_PRECHECK_MIN, FETCH_LARGEST_BLOCK_MIN) == FETCH_HEAP_OK,
               "exact thresholds admitted");
    // Negative: one byte under each limit refuses with the right reason.
    TEST_CHECK(update_fetch_heap_admit(FETCH_HEAP_PRECHECK_MIN - 1u, 9728u) == FETCH_HEAP_LOW_FREE,
               "free one byte short refused");
    TEST_CHECK(update_fetch_heap_admit(31123u, FETCH_LARGEST_BLOCK_MIN - 1u) == FETCH_HEAP_LOW_BLOCK,
               "largest block one byte short refused");
    TEST_CHECK(update_fetch_heap_admit(28672u, 9728u) == FETCH_HEAP_LOW_FREE,
               "the old 28672 B threshold no longer admits");
    TEST_CHECK(update_fetch_heap_admit(8295u, 4096u) == FETCH_HEAP_LOW_FREE, "post-stage dip refused");
    TEST_CHECK(update_fetch_heap_admit(0u, 0u) == FETCH_HEAP_LOW_FREE, "zero refused");
}

static void test_abort(void)
{
    TEST_SECTION("update_fetch_heap -- mid-body abort");
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW == 12288u, "abort threshold unchanged at 12288");
    TEST_CHECK(!update_fetch_heap_abort(12288u), "at threshold continues");
    TEST_CHECK(update_fetch_heap_abort(12287u), "one byte under aborts");
    TEST_CHECK(update_fetch_heap_abort(8192u), "at the owner floor aborts");
}

void run_test_update_fetch_heap(void)
{
    test_admit();
    test_abort();
}
