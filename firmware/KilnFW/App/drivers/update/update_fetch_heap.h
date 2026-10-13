// update_fetch_heap.h -- the internal-heap admission rule for the GitHub fetch (WP8), pure C so
// the decision is host-tested (App/test/test_update_fetch_heap.c). update_fetch.c supplies the
// two live numbers (heap_caps_get_free_size / heap_caps_get_largest_free_block, MALLOC_CAP_INTERNAL).
//
// Owner floor: heap_internal min_free >= 8192 B. A start is admitted only when the CURRENT free
// internal heap covers floor + estimated worst fetch draw + slack, so that the fetch's own draw
// should not take the low-water mark under the floor (see "Limits" below: not a guarantee), AND
// the largest free block can hold the biggest single internal allocation (the 4 KB
// update_fetch_wr stack plus its TCB, the 2 KB client buffers).
//
// Derivation (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 14, WP8 heap budget):
//   floor                                 8192 B
//   worst-case fetch draw, option D      16500 B   (about 16.5 KB estimated: WP7 bench handshake
//                                                   draw 11044 B, pre_init 31683 -> sampled_min
//                                                   20639, measured with a 2 KB rx and 1 KB tx
//                                                   client buffer; +1024 B for the 2 KB tx buffer
//                                                   here; plus the 4096 B writer stack with
//                                                   TCB/heap overhead, about 4.5 KB; mbedTLS
//                                                   buffers are PSRAM, so not counted. One hop,
//                                                   one hop of the API check; the asset download is the final hop and carries the writer stack)
// REVISED (bench finding, origin/dev 8fcd3237: update_check refused at idle, 29815 B free < 30836 B):
//   floor                                8192 B  (owner floor, unchanged)
//   worst draw                          17524 B  (16500 + 1024 B stager scratch; scratch cut 2048 -> 1024,
//                                                 UPDATE_STAGE_SCRATCH_MIN is 256, see
//                                                 docs/audits/UPDATE_FETCH_HEAP_DRAW_2026-10-09.md route 4)
//   concurrent httpd / KDF margin        3840 B  (was 4096). The login KDF (web_auth_store.c) does no heap
//                                                 allocation of its own (iterated HMAC on the caller's stack),
//                                                 so the margin only covers a concurrent httpd request; it
//                                                 is NOT shown mutually exclusive with the fetch, so kept.
//   -> FETCH_HEAP_PRECHECK_MIN          29556 B  (8192 + 17524 + 3840), below the 29647 B idle minimum;
//      at the idle minimum the post-draw margin over the floor is 29647 - 17524 - 8192 = 3931 B (superseded by UPDFX4: measured draw).
//      (old: 8192 + 18548 + 4096 = 30836 B, above idle.) Estimate, not a measurement: the WP8 gate
//      (b) bench run must confirm min_free >= 8192 B with scratch 1024.
// UPDFX1 (bench 2026-10-12, main 5098a1316: update_check -> low_heap at 30.3 KB free, 10.7 KB block):
//   Root cause: admission ran in run_job(), AFTER fetch_task had already allocated the 1024 B internal
//   stager scratch, so the live reading had that scratch taken out while the threshold counted it again
//   (30311 - 1024 = 29287 < 29556). Also a CHECK (releases/latest only, memory sink, no writer task, no
//   stager) was held to the full DOWNLOAD draw. Fix: admission is per kind and compares against the
//   draw still to come with the scratch already held:
//     download need = PRECHECK_MIN - SCRATCH = 28532 B   (floor + 16500 + slack; writer stack + TLS)
//     check    need = 8192 + CHECK_DRAW 12068 + 3840 = 24100 B
//   CHECK_DRAW = WP7 measured handshake draw 11044 B (2 KB rx, 1 KB tx) + 1024 B (2 KB tx here); no
//   4096 B writer stack (wr_start only runs for the stage sink). The 8192 B owner floor and the 3840 B
//   concurrent margin are unchanged. (The mid-body abort was 12288 B here; UPDFX2 below moved it to 10240 B.)
//   UPDFX2 (MED-1, LOW-1): a check is admitted at the SAME 28532 B need as a download (a check that
//   starts is a precursor to a download; the bench board idles at 29.6-31.1 KB so both pass). The
//   check keeps its own 3072 B largest-block need (client buffers only). The mid-body abort is now
//   FLOOR + 2048 = 10240 B, not 12288 B: a download admitted at 28532 B ends at 28532 - 16500 = 12032 B,
//   which the old 12288 B abort would have tripped immediately (256 B under). 10240 B leaves 1792 B of
//   margin over that post-draw level and still keeps 2048 B over the 8192 B floor.
//   Redirect hops (UPDFX2/UPDFX3). After the first hop the heap is ~2.4 KB lower than at job start
//   (27855 B seen at the 302 hop vs 30299 B idle; the first client is closed before the next admission,
//   so the remainder is lwIP/TLS teardown still draining). UPDFX2 admitted a hop on ABORT_BELOW + draw
//   (26740 B), dropping the 3840 B concurrent-request slack: a hop admitted at that minimum could end at
//   10240 B and a concurrent httpd request (3840 B) takes it to 6400 B, under the 8192 B floor (UPDFX2
//   review MED-1). UPDFX3 restores the slack, so a hop uses the same arithmetic as a start:
//     download hop need = FLOOR 8192 + DRAW 16500 + SLACK 3840 = 28532 B  (= the start need)
//     check    hop need = FLOOR 8192 + CHECK_DRAW 12068 + SLACK 3840 = 24100 B
//   Worst case at the minimum: 28532 - 16500 = 12032 B after the draw; minus the 3840 B request = 8192 B,
//   exactly the floor, and 1792 B above the abort level 10240 B, so the abort fires before a concurrent
//   request could be the one to cross the floor. The abort level itself keeps FLOOR + 2048 B.
//   The bench 302 hop at 27855 B is then 677 B short of 28532 B. Rather than weaken the rule, a LOW_FREE
//   verdict is re-sampled for up to FETCH_HEAP_SETTLE_TRIES x FETCH_HEAP_SETTLE_MS (12 x 250 ms = 3 s,
//   cancel- and deadline-aware, in update_fetch.c): the teardown frees its memory within that time, and the
//   same wait also covers a residual from the previous http_get (UPDFX2 LOW-1), so the first admission of
//   a release, manifest or asset request is not intermittently refused by the previous call's leftovers.
//   A heap that never recovers is still refused with low_heap_free.
//   DMA vs internal (UPDFX2 LOW-3): on the ESP32-S3 the DMA-capable internal pool is nearly the same RAM
//   as the general internal pool (MALLOC_CAP_DMA internal = internal minus a few regions), so the two
//   free figures move together and the DMA floor is much lower than the 8192 B internal floor (see UPDFX5 at FETCH_DMA_FREE_MIN).
//   The bench run benchupd2 saw DMA min_free 551 B: a transient low-water inside a TLS record read that
//   the between-read checks cannot see. The read loop now also checks DMA free after each read
//   (FETCH_DMA_FREE_MIN) and aborts with low_heap_dma; it narrows, not closes, the TOCTOU.
// UPDFX4 (REVIEW_UPDFX3 MED-1, MED-3): the measured cost model. Supersedes the 16500 B estimate above.
//   benchupd2 (c549a144c): the job started at about 29275 B free (30299 idle minus the 1024 B scratch) and
//   the low-water was 8339 B, so the whole download drew about 20936 B (FETCH_HEAP_MEASURED_DRAW_BYTES),
//   concurrent httpd polling included. The 16500 B + 3840 B slack budget (20340 B) was 596 B short of it, and
//   floor + measured draw (29128 B) is above the idle minimum after the scratch (29647 - 1024 = 28623 B).
//   Admission therefore cannot guarantee the floor from a minimum-idle start: any need that covers the
//   measured draw refuses every idle board and no update could ever start.
//   Choice: the start/hop need is the highest value an idle board still passes,
//     FETCH_HEAP_PRECHECK_MIN = FETCH_IDLE_MIN_FREE_BYTES = 29647 B   (28623 B with the scratch held),
//   so admission refuses every board below the observed idle minimum (busy or fragmented). Admission is the
//   ONLY guard for the TLS handshake, the hop's esp_http_client_open and anything inside a single
//   esp_http_client_read: the mid-body abort (FETCH_HEAP_ABORT_BELOW) runs only BETWEEN body reads, so it covers
//   the body phase only and cannot see a dip in those other phases (UPDFX4 MED-1). The floor is NOT guaranteed
//   there; the admission value is left unchanged pending bench data. At the idle minimum the measured draw leaves 28623 - 20936 = 7687 B, 505 B under the floor, if
//   httpd polling coincides with the handshake: a documented residual hazard, closed only by the per-phase
//   bench measurement in REVIEW_UPDFX3 Fix status (MED-1/MED-2), not by a larger constant.
//   The older 28532 B / 29556 B / 3931 B / 31123 figures in the UPDFX1..3 paragraphs above are superseded by
//   28623 / 29647 and the measured 20936 B draw.
//   DMA (MED-3, reworked by UPDFX5): see FETCH_DMA_FREE_MIN below; the 551 B benchupd2 figure is a low-water over
//   the whole job (inside TLS record reads), not a between-read sample, so it is not used as a test shape.
// The slack is deliberately NOT larger: the board idles at 29647-31123 B free
// (logs/sk04_sampling/2026-10-06.tsv, 29 samples), so anything above ~29.6 KB would refuse on an
// idle board. (Historical: against the 17524 B estimate the idle minimum would end at 12123 B; the measured
// 20936 B draw above supersedes that, leaving 7687 B at the idle minimum.)
// Largest block at idle is 9728 B (same log), so the block floor must sit below that. The biggest
// single internal allocation is the 4096 B update_fetch_wr stack (+ heap overhead); the 2 KB
// client buffers and TLS pieces are smaller and separate. 6144 B = 1.5x the stack, passes at idle.
//
// Limits (review 2026-10-07). This is an admission check, not a guarantee of the floor:
//  - It would also have admitted the 258b80d4 bench run that dipped to min_free 8295 B if that run
//    started near idle (31123 would be admitted; the current need is 28623 B). From an idle start that dip is a draw of up to about
//    22.8 KB, some 6 KB over the 16500 B budget and unexplained; the run did not record current
//    free before the job, so the start level is unknown. Do not read this rule as the fix for it.
//  - The numbers are a snapshot (TOCTOU): nothing is reserved between admission and the TLS
//    handshake, and the mid-body abort only runs between reads. The board's own transients are
//    larger than the slack (idle low-water 16555 B against idle free 31123 B, about 14.5 KB).
// WP8 gate (b) must log current free immediately before the job and min_free after it.
#ifndef KILNCTL_UPDATE_FETCH_HEAP_H
#define KILNCTL_UPDATE_FETCH_HEAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FETCH_HEAP_FLOOR_BYTES 8192u
// Stager scratch: INTERNAL RAM (MALLOC_CAP_INTERNAL), one per job. Must be internal and small: a PSRAM
// scratch makes esp_partition_read borrow an internal bounce buffer of up to 16 KiB per read (review
// GITHUB_UPDATE_CHAIN_REVIEW_2026-10-09 MED-1). Its bytes are part of the worst draw, taken out of the slack.
#define FETCH_HEAP_SCRATCH_BYTES 1024u
#define FETCH_HEAP_WORST_DRAW_BYTES (16500u + FETCH_HEAP_SCRATCH_BYTES)
// Margin for a concurrent httpd request / login KDF / httpd request while the fetch holds its allocations (review 3 LOW-5:
// the old 1932 B left almost nothing against the 8192 B floor). Used only by FETCH_HOP_CHECK_HEAP_MIN now;
// the start and download-hop need is the idle minimum minus the scratch (UPDFX4).
#define FETCH_HEAP_SLACK_BYTES 3840u
// Idle bench minimum of free internal heap (logs/sk04_sampling/2026-10-06.tsv). Admission never asks for
// more than this, or an idle board could never start a fetch (UPDFX4 above).
#define FETCH_IDLE_MIN_FREE_BYTES 29647u
// Measured whole-download draw (benchupd2), httpd traffic included. Informational pin, not an admission input.
#define FETCH_HEAP_MEASURED_DRAW_BYTES 20936u
#define FETCH_HEAP_PRECHECK_MIN FETCH_IDLE_MIN_FREE_BYTES
#define FETCH_LARGEST_BLOCK_MIN 6144u
// Check job: TLS handshake only, no writer task. Needs the 2 KB client buffers as the biggest block.
#define FETCH_CHECK_DRAW_BYTES (11044u + 1024u)
#define FETCH_CHECK_BLOCK_MIN 3072u
// Start need (both kinds). Download jobs hold the scratch already (the live reading excludes it); a check
// allocates none, so its live reading is 1024 B higher and the same number is slightly more lenient.
#define FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH (FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_SCRATCH_BYTES)
// Mid-body abort (read loop only, session already built): floor plus 2 KB of transient room.
#define FETCH_HEAP_ABORT_BELOW (FETCH_HEAP_FLOOR_BYTES + 2048u)
// Redirect hops after the first: same value as a start, the idle minimum minus the held scratch (UPDFX4).
#define FETCH_HOP_DOWNLOAD_HEAP_MIN FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH
#define FETCH_HOP_CHECK_HEAP_MIN (FETCH_HEAP_FLOOR_BYTES + FETCH_CHECK_DRAW_BYTES + FETCH_HEAP_SLACK_BYTES)
// A LOW_FREE verdict is re-sampled this many times, this far apart, before the request is refused.
#define FETCH_HEAP_SETTLE_TRIES 12u
#define FETCH_HEAP_SETTLE_MS 250u
// DMA-capable internal free. UPDFX5 (REVIEW_UPDFX4 MED-2): the floor is DERIVED, not a fixed 4096 B. Idle DMA-capable
// free sits about 7788 B below internal free (22511 vs 30299 B, benchupd2), so the DMA level equivalent to the
// FETCH_HEAP_ABORT_BELOW internal abort (10240 B) is 10240 - 7788 = 2452 B. A fixed 4096 B corresponded to about
// 11.9 KB internal free, above the internal abort, and would have aborted a healthy body phase the internal rule
// lets finish. At 2452 B the DMA guard fires no earlier than the internal one for the same pressure.
#define FETCH_DMA_IDLE_OFFSET_BYTES 7788u
#define FETCH_DMA_FREE_MIN (FETCH_HEAP_ABORT_BELOW - FETCH_DMA_IDLE_OFFSET_BYTES)
// Mid-body: the sample is taken only AFTER a read that returned data (a stalled EAGAIN slice is not a read and
// is never sampled). Abort on FETCH_DMA_LOW_READS consecutive low samples, or FETCH_DMA_WINDOW_LOW low samples
// among the last FETCH_DMA_WINDOW (UPDFX5 LOW-1: low,low,ok repeating), or one sample under FETCH_DMA_HARD_MIN.
#define FETCH_DMA_LOW_READS 3u
#define FETCH_DMA_WINDOW 8u
#define FETCH_DMA_WINDOW_LOW 4u
#define FETCH_DMA_HARD_MIN 256u

typedef enum {
    FETCH_HEAP_OK = 0,
    FETCH_HEAP_LOW_FREE,   // current free internal heap below the start or hop need for this kind
    FETCH_HEAP_LOW_BLOCK,  // largest free internal block below FETCH_LARGEST_BLOCK_MIN
} fetch_heap_verdict_t;

// Admission at a job or redirect-hop start (every fresh TLS session). The caller reads the heap AFTER
// the job's stager scratch is allocated, so the scratch is not charged again. is_download selects the
// download (writer task + stager) or check (TLS + JSON only) draw.
fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block, bool is_download);

// Same, for a redirect hop after the first (heap not back to the job-start level, see UPDFX3 above).
fetch_heap_verdict_t update_fetch_heap_admit_hop(uint32_t free_internal, uint32_t largest_internal_block,
                                                 bool is_download);

// DMA-capable internal free check (true = enough).
bool update_fetch_heap_dma_ok(uint32_t dma_free);

// Mid-body DMA check after every read that returned data. *hist is caller-owned (start 0, reset per response):
// a bit history of the last FETCH_DMA_WINDOW samples (1 = under FETCH_DMA_FREE_MIN). True = abort the download.
bool update_fetch_heap_dma_abort(uint32_t dma_free, unsigned *hist);

// Mid-body: true when the job must give up.
bool update_fetch_heap_abort(uint32_t free_internal);

#ifdef __cplusplus
}
#endif

#endif
