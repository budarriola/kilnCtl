// Host tests for App/drivers/update/update_fetch_heap.c (owner floor: internal min_free >= 8192 B).
// UPDFX7 model: admission is judged on the live reading taken with the 1024 B stager scratch already held; an admitted
// fetch bottoms out at admission minus the MODEL draw (largest measured draw 21488 B + 1024 B margin), and that bottom
// is the abort level (floor + 1024 B).
#include "test_common.h"

#include "../drivers/update/update_fetch_heap.h"

#define BENCH_RTC_FREE 7788u  // internal free - DMA free on the bench, every measured pair

static void test_admit(void)
{
    TEST_SECTION("update_fetch_heap -- admission (UPDFX7 model, scratch held)");
    TEST_CHECK(FETCH_HEAP_FLOOR_BYTES == 8192u && FETCH_HEAP_MAX_STEP_BYTES == 1024u && FETCH_HEAP_SCRATCH_BYTES == 1024u,
               "floor 8192, step margin 1024, scratch 1024 pinned");
    TEST_CHECK(FETCH_HEAP_MEASURED_DRAW_BYTES == 21488u && FETCH_HEAP_DRAW_MARGIN_BYTES == 1024u &&
                   FETCH_HEAP_MODEL_DRAW_BYTES == 22512u,
               "largest measured draw (benchupd4, scratch held) 21488 + margin 1024 = model draw 22512");
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH == 31728u, "admission with the scratch held is 31728 B");
    TEST_CHECK(FETCH_HEAP_PRECHECK_MIN == 32752u, "the same level before the scratch is taken is 32752 B");
    TEST_CHECK(update_fetch_heap_admit(31728u, 9728u, true) == FETCH_HEAP_OK, "31728 B admitted");
    TEST_CHECK(update_fetch_heap_admit(31727u, 9728u, true) == FETCH_HEAP_LOW_FREE, "31727 B refused");
    // Admitted at the edge: the model draw ends exactly at the abort level; the worst MEASURED draw ends 1024 above it.
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MODEL_DRAW_BYTES == FETCH_HEAP_ABORT_BELOW,
               "an admitted fetch at the model draw bottoms out exactly at the abort level (9216 B)");
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES == 10240u,
               "the worst measured draw, admitted at the edge, ends at 10240 B");
    TEST_CHECK(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES - FETCH_HEAP_FLOOR_BYTES == 2048u,
               "... which is 2048 B above the owner floor");
    // Each measured run (idle from the draw table, scratch held = idle - 1024, low-water) ends above the floor when
    // admitted at its own idle; the rows are idle / low-water.
    static const uint32_t runs[4][2] = { { 30299u, 8339u }, { 30427u, 10079u }, { 47191u, 24679u }, { 34783u, 14115u } };
    uint32_t max_draw = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t draw = runs[i][0] - FETCH_HEAP_SCRATCH_BYTES - runs[i][1];
        if (draw > max_draw) {
            max_draw = draw;
        }
    }
    TEST_CHECK(max_draw == FETCH_HEAP_MEASURED_DRAW_BYTES, "MEASURED_DRAW is exactly the largest of the four bench runs (benchupd2..5)");
    // The benchupd5 idle board (34783 B, UART ISR in IRAM) has the stated +2 KB margin.
    TEST_CHECK(update_fetch_heap_admit(34783u - FETCH_HEAP_SCRATCH_BYTES, 11776u, true) == FETCH_HEAP_OK,
               "benchupd5 idle 34783 B admitted");
    TEST_CHECK(34783u - FETCH_HEAP_PRECHECK_MIN >= 2000u, "benchupd5 idle clears the pre-scratch need by at least 2 KB");
    TEST_CHECK(update_fetch_heap_admit(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_SCRATCH_BYTES - 1u, 11776u, true) == FETCH_HEAP_LOW_FREE,
               "one byte under the pre-scratch need is refused");
    // Older-firmware idle levels (scratch held = idle - 1024) are refused instead of being admitted into the floor.
    TEST_CHECK(update_fetch_heap_admit(29647u - FETCH_HEAP_SCRATCH_BYTES, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "older idle minimum 29647 B refused");
    TEST_CHECK(update_fetch_heap_admit(29815u - FETCH_HEAP_SCRATCH_BYTES, 9728u, true) == FETCH_HEAP_LOW_FREE,
               "older idle 29815 B refused");
    TEST_CHECK(29647u - FETCH_HEAP_SCRATCH_BYTES - FETCH_HEAP_MEASURED_DRAW_BYTES == 7135u,
               "the refusal avoids ending at 7135 B, 1057 B under the floor");
    // Pre-UART-change idle (47191 B): admitted with room.
    TEST_CHECK(update_fetch_heap_admit(47191u - FETCH_HEAP_SCRATCH_BYTES, 18432u, true) == FETCH_HEAP_OK,
               "benchupd4 idle 47191 B admitted");
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
    TEST_CHECK(update_fetch_heap_need(true, false) == 31728u && update_fetch_heap_need(false, false) == 31728u,
               "the start need is the download need for both kinds");
}

static void test_hop(void)
{
    TEST_SECTION("update_fetch_heap -- redirect hop admission (UPDFX7: measured dip, not the start need)");
    TEST_CHECK(FETCH_HOP_DIP_BYTES == 1536u && FETCH_HOP_DIP_BYTES >= 1420u, "hop dip 1536 B covers the measured 1420 B (29275 -> 27855)");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN == 30192u, "download hop need is the start need less the dip (31728 - 1536)");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN == 21284u, "check hop need is the abort level + the check draw (9216 + 12068)");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN < FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, "hop need is below the start need");
    // MED-2: a board admitted exactly at the start edge sees the measured 1420 B hop dip and must still be admitted.
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 1420u, 9000u, true) == FETCH_HEAP_OK,
               "start-edge board after the measured hop dip is admitted at the hop");
    // The benchupd5 hop reading: 34783 - 1024 scratch - 1420 dip.
    TEST_CHECK(update_fetch_heap_admit_hop(34783u - 1024u - 1420u, 7680u, true) == FETCH_HEAP_OK,
               "benchupd5 idle board at the hop is admitted with no settle wait");
    TEST_CHECK(update_fetch_heap_admit_hop(27855u, 7680u, true) == FETCH_HEAP_LOW_FREE, "bench 27855 B hop reading (30299 idle board) refused");
    TEST_CHECK(update_fetch_heap_admit_hop(30000u, 9000u, true) == FETCH_HEAP_LOW_FREE, "30000 B download hop refused");
    TEST_CHECK(update_fetch_heap_admit_hop(25000u, 9000u, false) == FETCH_HEAP_OK, "25000 B check hop admitted");
    TEST_CHECK(update_fetch_heap_admit(25000u, 9000u, false) == FETCH_HEAP_LOW_FREE, "25000 B check refused as a start");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_DOWNLOAD_HEAP_MIN - 1u, 9000u, true) == FETCH_HEAP_LOW_FREE,
               "download hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_DOWNLOAD_HEAP_MIN, 9000u, true) == FETCH_HEAP_OK, "download hop exact admitted");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN - 1u, 9000u, false) == FETCH_HEAP_LOW_FREE,
               "check hop one byte short refused");
    TEST_CHECK(update_fetch_heap_admit_hop(FETCH_HOP_CHECK_HEAP_MIN, 9000u, false) == FETCH_HEAP_OK, "check hop exact admitted");
    TEST_CHECK(update_fetch_heap_admit_hop(40000u, FETCH_LARGEST_BLOCK_MIN - 1u, true) == FETCH_HEAP_LOW_BLOCK,
               "hop block short refused");
    // Floor arithmetic at the hop: the dip is already part of the draw, so the draw left is model draw less the dip.
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN - (FETCH_HEAP_MODEL_DRAW_BYTES - FETCH_HOP_DIP_BYTES) == FETCH_HEAP_ABORT_BELOW,
               "download hop at the edge, model draw: ends at the abort level");
    TEST_CHECK(FETCH_HOP_DOWNLOAD_HEAP_MIN - (FETCH_HEAP_MEASURED_DRAW_BYTES - 1420u) == 10124u,
               "download hop at the edge, worst measured draw: ends at 10124 B (1932 B above the floor)");
    TEST_CHECK(FETCH_HOP_CHECK_HEAP_MIN - FETCH_CHECK_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW, "check hop post-draw >= abort");
}

static void test_dma(void)
{
    TEST_SECTION("update_fetch_heap -- DMA-capable free (UPDFX7: offset derived, not hard-coded)");
    // The offset is derived from one pair of readings and saturates at 0.
    TEST_CHECK(update_fetch_heap_dma_offset(34783u - 1024u, 26995u - 1024u) == BENCH_RTC_FREE,
               "benchupd5 idle pair (33759 internal / 25971 DMA) derives the 7788 B RTC free");
    TEST_CHECK(update_fetch_heap_dma_offset(30299u, 22511u) == BENCH_RTC_FREE, "benchupd2 idle pair derives 7788 B");
    TEST_CHECK(update_fetch_heap_dma_offset(8339u, 551u) == BENCH_RTC_FREE, "benchupd2 low-water pair derives 7788 B");
    TEST_CHECK(update_fetch_heap_dma_offset(1000u, 5000u) == 0u, "DMA above internal saturates the offset at 0");

    // With the bench offset the numbers are the previous constants' successors.
    TEST_CHECK(update_fetch_heap_dma_need(true, false, BENCH_RTC_FREE) == 23940u, "DMA download admission is 31728 - 7788 = 23940 B");
    TEST_CHECK(update_fetch_heap_dma_need(false, false, BENCH_RTC_FREE) == 23940u, "DMA check start uses the download need");
    TEST_CHECK(update_fetch_heap_dma_need(true, true, BENCH_RTC_FREE) == 22404u, "DMA download hop need is 30192 - 7788 = 22404 B");
    TEST_CHECK(update_fetch_heap_dma_need(false, true, BENCH_RTC_FREE) == 13496u, "DMA check hop need is 21284 - 7788 = 13496 B");
    TEST_CHECK(update_fetch_heap_dma_floor(BENCH_RTC_FREE) == 1428u, "DMA sustained-low floor is 9216 - 7788 = 1428 B");
    TEST_CHECK(update_fetch_heap_dma_hard_min(BENCH_RTC_FREE) == 404u, "DMA hard minimum is the owner floor less the offset (404 B)");
    // Different RTC heap: every DMA number moves with it (a hard-coded 7788 would not).
    TEST_CHECK(update_fetch_heap_dma_need(true, false, 0u) == FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH, "no RTC heap: DMA need equals the internal need");
    TEST_CHECK(update_fetch_heap_dma_need(true, false, 4096u) == 31728u - 4096u, "a 4096 B RTC heap lowers the DMA need by 4096");
    TEST_CHECK(update_fetch_heap_dma_floor(4096u) == 9216u - 4096u && update_fetch_heap_dma_hard_min(4096u) == 8192u - 4096u,
               "the DMA floors follow the offset");
    TEST_CHECK(update_fetch_heap_dma_need(true, false, 100000u) == 0u && update_fetch_heap_dma_floor(100000u) == 0u &&
                   update_fetch_heap_dma_hard_min(100000u) == 0u,
               "an offset above every need saturates at 0");

    // At the first admission the DMA check is redundant with the internal one: need(rtc = f - dma) <= dma iff f >= need.
    {
        const uint32_t f = FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH;
        const uint32_t d = f - BENCH_RTC_FREE;
        const uint32_t rtc = update_fetch_heap_dma_offset(f, d);
        TEST_CHECK(update_fetch_heap_dma_admit(d, rtc, true, false), "first admission at the internal edge passes the derived DMA check");
        TEST_CHECK(!update_fetch_heap_dma_admit(d - 1u, rtc, true, false), "a DMA reading one byte under the derived need is refused");
    }
    // The benchupd5 idle board: DMA 26995 - 1024 = 25971 against 23940.
    TEST_CHECK(update_fetch_heap_dma_admit(26995u - 1024u, BENCH_RTC_FREE, true, false), "benchupd5 idle DMA admitted");
    TEST_CHECK(26995u - 1024u - update_fetch_heap_dma_need(true, false, BENCH_RTC_FREE) == 2031u, "... with a 2031 B DMA margin");

    // DMA admission edge: exactly the need admits, one under refuses.
    const uint32_t need_dl = update_fetch_heap_dma_need(true, false, BENCH_RTC_FREE);
    TEST_CHECK(update_fetch_heap_dma_admit(need_dl, BENCH_RTC_FREE, true, false), "download: exact DMA need admitted");
    TEST_CHECK(!update_fetch_heap_dma_admit(need_dl - 1u, BENCH_RTC_FREE, true, false), "download: one under refused");
    TEST_CHECK(!update_fetch_heap_dma_admit(need_dl - 1u, BENCH_RTC_FREE, false, false), "check start uses the download need");
    const uint32_t need_hop = update_fetch_heap_dma_need(true, true, BENCH_RTC_FREE);
    TEST_CHECK(update_fetch_heap_dma_admit(need_hop, BENCH_RTC_FREE, true, true), "download hop: exact need admitted");
    TEST_CHECK(!update_fetch_heap_dma_admit(need_hop - 1u, BENCH_RTC_FREE, true, true), "download hop: one under refused");
    const uint32_t need_ch = update_fetch_heap_dma_need(false, true, BENCH_RTC_FREE);
    TEST_CHECK(update_fetch_heap_dma_admit(need_ch, BENCH_RTC_FREE, false, true), "check hop: exact need admitted");
    TEST_CHECK(!update_fetch_heap_dma_admit(need_ch - 1u, BENCH_RTC_FREE, false, true), "check hop: one under refused");

    TEST_CHECK(FETCH_DMA_LOW_READS == 3u && FETCH_DMA_WINDOW == 8u && FETCH_DMA_WINDOW_LOW == 4u, "DMA abort constants pinned");
    const uint32_t low = update_fetch_heap_dma_floor(BENCH_RTC_FREE) - 1u;
    const uint32_t floor_ok = update_fetch_heap_dma_floor(BENCH_RTC_FREE);
    const uint32_t hard = update_fetch_heap_dma_hard_min(BENCH_RTC_FREE);
    unsigned h = 0;
    TEST_CHECK(!update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h), "first low sample does not abort");
    TEST_CHECK(!update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h), "second low sample does not abort");
    TEST_CHECK(update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h), "third consecutive low sample aborts");
    h = 0;
    update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h);
    update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h);
    TEST_CHECK(!update_fetch_heap_dma_abort(floor_ok, BENCH_RTC_FREE, &h), "a recovered sample (exact floor) does not abort");
    TEST_CHECK(!update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h), "after recovery a single low sample does not abort");
    h = 0;
    TEST_CHECK(update_fetch_heap_dma_abort(hard - 1u, BENCH_RTC_FREE, &h), "under the hard minimum aborts at once");
    h = 0;
    TEST_CHECK(!update_fetch_heap_dma_abort(hard, BENCH_RTC_FREE, &h), "exactly the hard minimum is only a low sample");
    // The same sample judged with a different offset: 1500 B DMA is fine with the bench offset, but with no RTC heap
    // DMA free is the whole internal free and 1500 B is far under the owner floor.
    h = 0;
    TEST_CHECK(!update_fetch_heap_dma_abort(1500u, BENCH_RTC_FREE, &h), "1500 B DMA with the bench offset is above the 1428 B floor");
    h = 0;
    TEST_CHECK(update_fetch_heap_dma_abort(1500u, 0u, &h), "1500 B DMA with no RTC heap is under the 8192 B hard minimum");

    /* low, low, ok repeating never gets 3 in a row but must abort within the window (4 lows of 8). */
    h = 0;
    int aborted_at = 0;
    for (int i = 1; i <= 12 && !aborted_at; i++) {
        const uint32_t v = (i % 3 == 0) ? 100000u : low;
        if (update_fetch_heap_dma_abort(v, BENCH_RTC_FREE, &h)) {
            aborted_at = i;
        }
    }
    TEST_CHECK(aborted_at == 5, "low,low,ok repeating aborts on the fourth low of the window (sample 5)");
    h = 0;
    bool any = false;
    for (int i = 0; i < 40; i++) {
        any |= update_fetch_heap_dma_abort((i % 4 == 0) ? low : 100000u, BENCH_RTC_FREE, &h);
    }
    TEST_CHECK(!any, "one low in four (2 of 8) never aborts");
    h = 0;
    const unsigned seq3[4] = { 1, 1, 0, 1 };
    any = false;
    for (int i = 0; i < 4; i++) {
        any |= update_fetch_heap_dma_abort(seq3[i] ? low : 100000u, BENCH_RTC_FREE, &h);
    }
    TEST_CHECK(!any, "3 lows within 4 samples (no 3 in a row) does not abort");
    TEST_CHECK(update_fetch_heap_dma_abort(low, BENCH_RTC_FREE, &h), "the fourth low within 8 samples aborts");
}

static void test_abort(void)
{
    TEST_SECTION("update_fetch_heap -- mid-body abort");
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW == FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES && FETCH_HEAP_ABORT_BELOW == 9216u,
               "abort threshold is floor + the 1024 B step margin");
    // A fetch admitted at the edge must not self-abort: it bottoms out at admission minus the draw on the same base.
    const uint32_t bottom = FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MODEL_DRAW_BYTES;
    TEST_CHECK(bottom == 9216u, "bottom of a fetch admitted at the edge at the model draw is 9216 B");
    TEST_CHECK(!update_fetch_heap_abort(bottom), "fetch admitted at the edge, bottoming out, does not abort");
    TEST_CHECK(bottom - FETCH_HEAP_MAX_STEP_BYTES >= FETCH_HEAP_FLOOR_BYTES, "one more step margin from the bottom still holds the floor");
    TEST_CHECK(update_fetch_heap_abort(bottom - 1u), "one byte lower aborts");
    TEST_CHECK(!update_fetch_heap_abort(9216u), "at threshold continues");
    TEST_CHECK(update_fetch_heap_abort(9215u), "one byte under aborts");
    TEST_CHECK(update_fetch_heap_abort(8192u), "at the owner floor aborts");
    // The abort level leaves exactly the step margin above the floor: a sample at the level cannot reach the floor
    // unless a single step exceeds the margin.
    TEST_CHECK(FETCH_HEAP_ABORT_BELOW - FETCH_HEAP_FLOOR_BYTES == FETCH_HEAP_MAX_STEP_BYTES, "abort level - floor equals the step margin");
}

void run_test_update_fetch_heap(void)
{
    test_admit();
    test_hop();
    test_dma();
    test_abort();
}
