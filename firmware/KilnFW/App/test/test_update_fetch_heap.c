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
    TEST_CHECK(update_fetch_heap_admit(29647u, 9728u, true) == FETCH_HEAP_OK, "idle minimum admitted");
    // UPDFX1 bench: admission runs after the 1024 B scratch is held. 30311 free was read as ~29287.
    TEST_CHECK(update_fetch_heap_admit(30311u - FETCH_HEAP_SCRATCH_BYTES, 10752u, false) == FETCH_HEAP_OK,
               "UPDFX1 check admitted at 30.3 KB free, 10.7 KB block");
    TEST_CHECK(update_fetch_heap_admit(30311u - FETCH_HEAP_SCRATCH_BYTES, 10752u, true) == FETCH_HEAP_OK,
               "UPDFX1 download admitted at 30.3 KB free (scratch already held)");
    // MED-1: a check is admitted at the download need, with its own (smaller) block need.
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 1u, 10752u, false) == FETCH_HEAP_LOW_FREE,
               "check one byte under the download need refused");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, 10752u, false) == FETCH_HEAP_OK,
               "check admitted exactly at the download need");
    TEST_CHECK(update_fetch_heap_admit(26000u, 10752u, false) == FETCH_HEAP_LOW_FREE, "26000 B refuses a check");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, FETCH_CHECK_BLOCK_MIN, false) == FETCH_HEAP_OK,
               "check admitted with a 3072 B block");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "download needs the 6144 B block a check does not");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, FETCH_CHECK_BLOCK_MIN - 1u, false) == FETCH_HEAP_LOW_BLOCK,
               "check small block refused");
    TEST_CHECK(update_fetch_heap_admit(12000u, 10752u, false) == FETCH_HEAP_LOW_FREE, "check below floor+draw refused");
    // Free is judged before the block: both short reports free.
    TEST_CHECK(update_fetch_heap_admit(1000u, 100u, true) == FETCH_HEAP_LOW_FREE, "free shortfall reported before block");
    TEST_CHECK(update_fetch_heap_admit(29815u, 9728u, true) == FETCH_HEAP_OK, "bench idle 29815 B admitted");
    TEST_CHECK(29647u - FETCH_HEAP_WORST_DRAW_BYTES - FETCH_HEAP_FLOOR_BYTES == 3931u, "idle-min margin over floor is 3931 B");
    TEST_CHECK(update_fetch_heap_admit(31123u, 9728u, true) == FETCH_HEAP_OK, "idle maximum admitted");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, FETCH_LARGEST_BLOCK_MIN, true) == FETCH_HEAP_OK,
               "exact thresholds admitted");
    // Negative: one byte under each limit refuses with the right reason.
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 1u, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "free one byte short refused");
    TEST_CHECK(update_fetch_heap_admit(31123u, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "largest block one byte short refused");
    TEST_CHECK(update_fetch_heap_admit(28000u, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "28000 B (below the 28532 B download need) refused");
    TEST_CHECK(update_fetch_heap_admit(8295u, 4096u, true) == FETCH_HEAP_LOW_FREE, "post-stage dip refused");
    TEST_CHECK(update_fetch_heap_admit(0u, 0u, true) == FETCH_HEAP_LOW_FREE, "zero refused");
}

static void test_hop(void)
{
    TEST_SECTION("update_fetch_heap -- redirect hop admission (UPDFX2)");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN == 28532u, "download hop need is floor + 16500 + slack 3840");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN == 24100u, "check hop need is floor + check draw + slack 3840");
    // Bench: 27855 B free, 7680 B block at the 302 hop of a healthy download.
    TEST_CHECK(update_fetch_heap_admit_hop(27855u, 7680u, true) == FETCH_HEAP_LOW_FREE, "bench 27855 B hop reading refused until the residual drains");
    TEST_CHECK(update_fetch_heap_admit_hop(28000u, 9000u, true) == FETCH_HEAP_LOW_FREE, "28000 B download hop refused");
    TEST_CHECK(update_fetch_heap_admit_hop(25000u, 9000u, false) == FETCH_HEAP_OK, "25000 B check hop admitted");
    TEST_CHECK(update_fetch_heap_admit(25000u, 9000u, false) == FETCH_HEAP_LOW_FREE, "25000 B check refused as a start");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_DOWNLOAD_HEAP_MIN - 1u, 9000u, true) == FETCH_HEAP_LOW_FREE,
               "download hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN - 1u, 9000u, false) == FETCH_HEAP_LOW_FREE,
               "check hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN, 9000u, false) == FETCH_HEAP_OK, "check hop exact admitted");
    TEST_CHECK(update_fetch_heap_admit_hop(30000u, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "hop block short refused");
    // The post-draw level of any admitted hop never drops under the abort level, hence >= floor + 2048.
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN - 16500u >= FETCH_HEAP_ABORT_BELOW, "download hop post-draw >= abort");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN - FETCH_CHECK_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW, "check hop post-draw >= abort");
}

static void test_dma(void)
{
    TEST_SECTION("update_fetch_heap -- DMA-capable free (LOW-6)");
    TEST_CHECK(FETCH_DMA_FREE_MIN == 4096u, "DMA floor 4096");
    TEST_CHECK(update_fetch_heap_dma_ok(6351u), "bench idle min_free 6351 passes");
    TEST_CHECK(update_fetch_heap_dma_ok(FETCH_DMA_FREE_MIN), "exact floor passes");
    TEST_CHECK(!update_fetch_heap_dma_ok(FETCH_DMA_FREE_MIN - 1u), "one under refused");
}

static void test_abort(void)
{
    TEST_SECTION("update_fetch_heap -- mid-body abort");
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW == FETCH_HEAP_FLOOR_BYTES + 2048u && FETCH_HEAP_ABORT_BELOW == 10240u,
               "abort threshold is floor + 2048");
    // LOW-1: an admitted download must not self-abort at once.
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 16500u >= FETCH_HEAP_ABORT_BELOW + 1024u,
               "post-draw level of a start admission clears the abort level with margin");
    TEST_CHECK(!update_fetch_heap_abort(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 16500u), "admitted download does not abort at once");
    TEST_CHECK(!update_fetch_heap_abort(10240u), "at threshold continues");
    TEST_CHECK(update_fetch_heap_abort(10239u), "one byte under aborts");
    TEST_CHECK(update_fetch_heap_abort(8192u), "at the owner floor aborts");
}

void run_test_update_fetch_heap(void)
{
    test_admit();
    test_hop();
    test_dma();
    test_abort();
}
