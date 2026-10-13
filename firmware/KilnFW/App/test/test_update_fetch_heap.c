// Host tests for App/drivers/update/update_fetch_heap.c (owner floor: internal min_free >= 8192 B).
// UPDFX6 model: admission is judged on the live reading taken with the 1024 B stager scratch already held; an admitted
// fetch bottoms out at admission minus the measured draw (same base), and that bottom is the abort level.
#include "test_common.h"

#include "../drivers/update/update_fetch_heap.h"

static void test_admit(void)
{
    TEST_SECTION("update_fetch_heap -- admission (UPDFX6 model, scratch held)");
    TEST_CHECK(FETCH_HEAP_FLOOR_BYTES == 8192u && FETCH_HEAP_MAX_STEP_BYTES == 4096u && FETCH_HEAP_SCRATCH_BYTES == 1024u,
               "floor 8192, largest single step 4096, scratch 1024 pinned");
    TEST_CHECK(FETCH_HEAP_MEASURED_DRAW_BYTES == 20936u && FETCH_IDLE_MIN_FREE_BYTES == 29647u,
               "measured draw (scratch held, benchupd2) and the older idle minimum pinned");
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH == 33224u, "admission with the scratch held is 33224 B");
    TEST_CHECK(FETCH_HEAP_PRECHECK_MIN == 34248u, "the same level before the scratch is taken is 34248 B");
    TEST_CHECK(update_fetch_heap_admit(33224u, 9728u, true) == FETCH_HEAP_OK, "33224 B admitted");
    TEST_CHECK(update_fetch_heap_admit(33223u, 9728u, true) == FETCH_HEAP_LOW_FREE, "33223 B refused");
    // Level after admission minus the remaining draw is the abort level; the abort level covers floor plus one step.
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES == FETCH_HEAP_ABORT_BELOW,
               "an admitted fetch bottoms out exactly at the abort level (12288 B)");
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW >= FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES, "abort level covers floor plus one step");
    // Older-firmware idle levels (scratch held = idle - 1024) are refused instead of being admitted into the floor.
    TEST_CHECK(update_fetch_heap_admit(29647u - FETCH_HEAP_SCRATCH_BYTES, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "older idle minimum 29647 B refused");
    TEST_CHECK(update_fetch_heap_admit(29815u - FETCH_HEAP_SCRATCH_BYTES, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "older idle 29815 B refused");
    TEST_CHECK(29647u - FETCH_HEAP_SCRATCH_BYTES - FETCH_HEAP_MEASURED_DRAW_BYTES == 7687u,
               "the refusal avoids ending at 7687 B, 505 B under the floor");
    // Current-firmware idle (47191 B): admitted with room.
    TEST_CHECK(update_fetch_heap_admit(47191u - FETCH_HEAP_SCRATCH_BYTES, 18432u, true) == FETCH_HEAP_OK,
               "current-firmware idle 47191 B admitted");
    TEST_CHECK(update_fetch_heap_admit(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_SCRATCH_BYTES, 9728u, true) == FETCH_HEAP_OK,
               "idle exactly at the pre-scratch level admitted");
    // A check is admitted at the download need, with its own (smaller) block need.
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
    // Free is judged before the block: both short reports free.
    TEST_CHECK(update_fetch_heap_admit(1000u, 100u, true) == FETCH_HEAP_LOW_FREE, "free shortfall reported before block");
    TEST_CHECK(update_fetch_heap_admit(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, FETCH_LARGEST_BLOCK_MIN, true) == FETCH_HEAP_OK,
               "exact thresholds admitted");
    TEST_CHECK(update_fetch_heap_admit(60000u, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "largest block one byte short refused");
    TEST_CHECK(update_fetch_heap_admit(8295u, 4096u, true) == FETCH_HEAP_LOW_FREE, "post-stage dip refused");
    TEST_CHECK(update_fetch_heap_admit(0u, 0u, true) == FETCH_HEAP_LOW_FREE, "zero refused");
}

static void test_hop(void)
{
    TEST_SECTION("update_fetch_heap -- redirect hop admission");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN == 33224u, "download hop need equals the start need");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN == 24356u, "check hop need is the abort level + the check draw (12288 + 12068)");
    TEST_CHECK(update_fetch_heap_admit_hop(27855u, 7680u, true) == FETCH_HEAP_LOW_FREE, "bench 27855 B hop reading refused");
    TEST_CHECK(update_fetch_heap_admit_hop(33000u, 9000u, true) == FETCH_HEAP_LOW_FREE, "33000 B download hop refused");
    TEST_CHECK(update_fetch_heap_admit_hop(25000u, 9000u, false) == FETCH_HEAP_OK, "25000 B check hop admitted");
    TEST_CHECK(update_fetch_heap_admit(25000u, 9000u, false) == FETCH_HEAP_LOW_FREE, "25000 B check refused as a start");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_DOWNLOAD_HEAP_MIN - 1u, 9000u, true) == FETCH_HEAP_LOW_FREE,
               "download hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN - 1u, 9000u, false) == FETCH_HEAP_LOW_FREE,
               "check hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN, 9000u, false) == FETCH_HEAP_OK, "check hop exact admitted");
    TEST_CHECK(update_fetch_heap_admit_hop(40000u, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "hop block short refused");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN - FETCH_HEAP_MEASURED_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW, "download hop post-draw >= abort");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN - FETCH_CHECK_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW, "check hop post-draw >= abort");
}

static void test_dma(void)
{
    TEST_SECTION("update_fetch_heap -- DMA-capable free (UPDFX6: same source as the internal numbers)");
    TEST_CHECK(FETCH_DMA_OFFSET_BYTES == 7788u, "DMA offset below internal pinned (30299-22511, 8339-551, 10079-2291)");
    TEST_CHECK(FETCH_DMA_FREE_MIN == 4500u && FETCH_DMA_FREE_MIN == FETCH_HEAP_ABORT_BELOW - FETCH_DMA_OFFSET_BYTES,
               "DMA sustained-low floor is the abort level less the offset (4500 B)");
    TEST_CHECK(FETCH_DMA_HARD_MIN == 404u && FETCH_DMA_HARD_MIN == FETCH_HEAP_FLOOR_BYTES - FETCH_DMA_OFFSET_BYTES,
               "DMA hard minimum is the owner floor less the offset (404 B)");
    TEST_CHECK(FETCH_DMA_ADMIT_DOWNLOAD_MIN == 25436u, "DMA download admission is floor + measured draw (4500 + 20936)");
    TEST_CHECK(FETCH_DMA_ADMIT_CHECK_HOP_MIN == FETCH_DMA_FREE_MIN + FETCH_CHECK_DRAW_BYTES, "DMA check-hop admission pinned");
    TEST_CHECK(FETCH_DMA_ADMIT_DOWNLOAD_MIN == FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_DMA_OFFSET_BYTES,
               "DMA admission equals the internal admission less the offset");

    // DMA admission edge: exactly the need admits, one under refuses.
    TEST_CHECK(update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_DOWNLOAD_MIN, true, false), "download: exact DMA need admitted");
    TEST_CHECK(!update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_DOWNLOAD_MIN - 1u, true, false), "download: one under refused");
    TEST_CHECK(!update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_DOWNLOAD_MIN - 1u, true, true), "download hop: one under refused");
    TEST_CHECK(!update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_DOWNLOAD_MIN - 1u, false, false), "check start uses the download need");
    TEST_CHECK(update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_CHECK_HOP_MIN, false, true), "check hop: exact need admitted");
    TEST_CHECK(!update_fetch_heap_dma_admit(FETCH_DMA_ADMIT_CHECK_HOP_MIN - 1u, false, true), "check hop: one under refused");
    // A DMA-admitted fetch bottoms out at the DMA floor; so does an internally-admitted one (same offset).
    TEST_CHECK(FETCH_DMA_ADMIT_DOWNLOAD_MIN - FETCH_HEAP_MEASURED_DRAW_BYTES == FETCH_DMA_FREE_MIN,
               "a DMA-admitted fetch bottoms out at the DMA floor");
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES - FETCH_DMA_OFFSET_BYTES >= FETCH_DMA_FREE_MIN,
               "internal-admitted start ends with DMA at or above the DMA floor");

    TEST_CHECK(FETCH_DMA_LOW_READS == 3u && FETCH_DMA_WINDOW == 8u && FETCH_DMA_WINDOW_LOW == 4u, "DMA abort constants pinned");
    const uint32_t low = FETCH_DMA_FREE_MIN - 1u;
    unsigned h = 0;
    TEST_CHECK(!update_fetch_heap_dma_abort(low, &h), "first low sample does not abort");
    TEST_CHECK(!update_fetch_heap_dma_abort(low, &h), "second low sample does not abort");
    TEST_CHECK(update_fetch_heap_dma_abort(low, &h), "third consecutive low sample aborts");
    h = 0;
    update_fetch_heap_dma_abort(low, &h);
    update_fetch_heap_dma_abort(low, &h);
    TEST_CHECK(!update_fetch_heap_dma_abort(FETCH_DMA_FREE_MIN, &h), "a recovered sample (exact floor) does not abort");
    TEST_CHECK(!update_fetch_heap_dma_abort(low, &h), "after recovery a single low sample does not abort");
    h = 0;
    TEST_CHECK(update_fetch_heap_dma_abort(FETCH_DMA_HARD_MIN - 1u, &h), "under the hard minimum aborts at once");
    h = 0;
    TEST_CHECK(!update_fetch_heap_dma_abort(FETCH_DMA_HARD_MIN, &h), "exactly the hard minimum is only a low sample");

    /* low, low, ok repeating never gets 3 in a row but must abort within the window (4 lows of 8). */
    h = 0;
    int aborted_at = 0;
    for (int i = 1; i <= 12 && !aborted_at; i++) {
        const uint32_t v = (i % 3 == 0) ? 100000u : low;
        if (update_fetch_heap_dma_abort(v, &h)) {
            aborted_at = i;
        }
    }
    TEST_CHECK(aborted_at == 5, "low,low,ok repeating aborts on the fourth low of the window (sample 5)");
    h = 0;
    bool any = false;
    for (int i = 0; i < 40; i++) {
        any |= update_fetch_heap_dma_abort((i % 4 == 0) ? low : 100000u, &h);
    }
    TEST_CHECK(!any, "one low in four (2 of 8) never aborts");
    h = 0;
    const unsigned seq3[4] = { 1, 1, 0, 1 };
    any = false;
    for (int i = 0; i < 4; i++) {
        any |= update_fetch_heap_dma_abort(seq3[i] ? low : 100000u, &h);
    }
    TEST_CHECK(!any, "3 lows within 4 samples (no 3 in a row) does not abort");
    TEST_CHECK(update_fetch_heap_dma_abort(low, &h), "the fourth low within 8 samples aborts");
}

static void test_abort(void)
{
    TEST_SECTION("update_fetch_heap -- mid-body abort");
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW == FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES && FETCH_HEAP_ABORT_BELOW == 12288u,
               "abort threshold is floor + one 4096 B step");
    // A fetch admitted at the edge must not self-abort: it bottoms out at admission minus the draw on the same base.
    const uint32_t bottom = FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES;
    TEST_CHECK(bottom == 12288u, "bottom of a fetch admitted at the edge is 12288 B");
    TEST_CHECK(!update_fetch_heap_abort(bottom), "fetch admitted at the edge, bottoming out, does not abort");
    TEST_CHECK(bottom - FETCH_HEAP_MAX_STEP_BYTES >= FETCH_HEAP_FLOOR_BYTES, "one more largest step from the bottom still holds the floor");
    TEST_CHECK(update_fetch_heap_abort(bottom - 1u), "one byte lower aborts");
    TEST_CHECK(!update_fetch_heap_abort(12288u), "at threshold continues");
    TEST_CHECK(update_fetch_heap_abort(12287u), "one byte under aborts");
    TEST_CHECK(update_fetch_heap_abort(8192u), "at the owner floor aborts");
}

void run_test_update_fetch_heap(void)
{
    test_admit();
    test_hop();
    test_dma();
    test_abort();
}
