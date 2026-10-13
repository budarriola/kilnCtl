// update_fetch_heap.h -- the internal-heap admission rule for the GitHub fetch (WP8), pure C so
// the decision is host-tested (App/test/test_update_fetch_heap.c). update_fetch.c supplies the
// live numbers (heap_caps_get_free_size / heap_caps_get_largest_free_block, MALLOC_CAP_INTERNAL, and
// the DMA-capable internal free).
//
// Owner floor: heap_internal min_free >= 8192 B. This is a model, not a guarantee (see "Limits").
//
// THE MODEL (UPDFX6; one source for the internal and the DMA numbers). Every level is the live reading
// taken by heap_admit() in update_fetch.c, which runs AFTER fetch_task() has allocated the 1024 B stager
// scratch. So "admission level" means the level with the scratch already held, and the measured draw must
// be on that same base:
//
//   FLOOR                8192 B   owner floor
//   MAX_STEP             4096 B   largest single internal allocation between two heap samples (LOW-1 below)
//   ABORT_BELOW         12288 B   = FLOOR + MAX_STEP: a sample at or above this cannot be taken under the floor
//                                 by the next single step, so the mid-body abort fires before the floor is crossed
//   MEASURED_DRAW       20936 B   whole download draw, scratch already held (benchupd2: idle 30299 B, minus the
//                                 1024 B scratch = 29275 B at the job start, low-water 8339 B). From idle,
//                                 scratch included, that run drew 21960 B and the pre.3 run 20240 B.
//   ADMISSION (held)    33224 B   = ABORT_BELOW + MEASURED_DRAW. An admitted fetch bottoms out at or above
//                                 ABORT_BELOW, so it neither self-aborts nor reaches the floor.
//   same before scratch 34248 B   = ADMISSION + SCRATCH (FETCH_HEAP_PRECHECK_MIN: what an idle board must show)
//   DMA                           internal pool minus a fixed 7788 B (benchupd2 idle 30299 / 22511, low-waters
//                                 8339 / 551, pre.3 low-waters 10079 / 2291: the offset is 7788 B in every pair),
//                                 and the draw is the same. FETCH_DMA_FREE_MIN = ABORT_BELOW - 7788 = 4500 B,
//                                 FETCH_DMA_HARD_MIN = FLOOR - 7788 = 404 B (DMA that low means the internal
//                                 floor is already crossed), DMA admission = DMA_FREE_MIN + DRAW = 25436 B
//                                 = ADMISSION - 7788 B: never stricter than the internal rule on a board with
//                                 the measured offset, and it binds if DMA is scarcer than that.
//
// Idle boards against this rule (admitted when idle >= 34248 B before the scratch):
//   current firmware (bench 2026-10-13, update_stage_release run)   47191 B  ADMITTED (about 13 KB over; that
//                                                                   fetch drew only 2908 B below its prior
//                                                                   low-water)
//   older firmware (sk04 log 2026-10-06; update_check refusal)      29647..31123 B, 29815 B  REFUSED
//   No rule that is consistent with the measured draw can admit the older figures: from 29647 B the benchupd2
//   draw bottoms at 7687 B, 505 B under the floor. The previous revision admitted exactly that and its assert
//   compared the wrong pair (REVIEW_UPDFX5 MED-1). A refused start fails with low_heap_free after the settle
//   wait; it never runs into the floor. Re-measure on the bench before lowering ADMISSION.
//
// LOW-1 (MAX_STEP). The abort samples once per read-loop pass (update_fetch.c), so between two samples the heap
// can move by the largest allocation that pass makes. Candidates, all internal under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192:
//   - the flash write of one FETCH_CHUNK_LEN (4096 B) chunk from the PSRAM buffer: esp_partition_write bounces
//     through an internal buffer of min(len, 8192) = 4096 B (update_http.c:38, UPDATE_FETCH_HEAP_DRAW audit);
//   - the 2048 B rx/tx client buffers: allocated at open, before the loop, not per pass;
//   - TLS records and the read chunk: PSRAM (option D), not internal;
//   - lwIP/WiFi rx pbufs: a fixed pool of ~1.6 KB buffers.
// So the bound is 4096 B, and update_fetch.c static-asserts FETCH_CHUNK_LEN and FETCH_TX_BUF_BYTES against it.
// It bounds ONE allocation by this fetch; concurrent tasks (httpd request, KDF) can stack more on top, which is
// the residual in "Limits".
//
// Redirect hops. After the first hop the heap is ~2.4 KB lower than at job start (27855 B at the 302 hop vs
// 30299 B idle: the first client is closed but lwIP/TLS teardown is still draining). A LOW_FREE verdict is
// re-sampled FETCH_HEAP_SETTLE_TRIES x FETCH_HEAP_SETTLE_MS (12 x 250 ms = 3 s, cancel/deadline/heat aware, in
// update_fetch.c); a heap that never recovers is refused low_heap_free. A hop uses the start need for a download
// and ABORT_BELOW + CHECK_DRAW for a check (a check runs the same abort loop). The first admission of a check
// uses the download need (a check that starts is a precursor to a download).
//
// Largest free block: the biggest single internal allocation at start is the 4096 B update_fetch_wr stack
// (+ overhead); 6144 B = 1.5x passes the idle largest block (9728 B, sk04 log). A check needs only the 2 KB
// client buffers (3072 B).
//
// Limits (review 2026-10-07, REVIEW_UPDFX4/5). Admission is the ONLY guard for the TLS handshake,
// esp_http_client_open and anything inside a single esp_http_client_read: the abort runs between reads only.
// The numbers are a snapshot (TOCTOU), the draw comes from two bench runs, and the board's own transients are
// larger than any margin here (idle low-water 16555 B against idle free 31123 B). The 258b80d4 bench run that
// dipped to 8295 B is not explained by this rule. Bench-pending: a fetch on a board flashed with this build,
// logging free before the job, per-hop and between-read DMA samples, and the settle wait.
#ifndef KILNCTL_UPDATE_FETCH_HEAP_H
#define KILNCTL_UPDATE_FETCH_HEAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FETCH_HEAP_FLOOR_BYTES 8192u
// Stager scratch: INTERNAL RAM (MALLOC_CAP_INTERNAL), one per download job, allocated before admission. Must be
// internal and small: a PSRAM scratch makes esp_partition_read borrow an internal bounce buffer of up to 16 KiB
// per read (review GITHUB_UPDATE_CHAIN_REVIEW_2026-10-09 MED-1).
#define FETCH_HEAP_SCRATCH_BYTES 1024u
// Largest single internal allocation a read-loop pass makes (LOW-1 above).
#define FETCH_HEAP_MAX_STEP_BYTES 4096u
// Idle bench minimum of the older firmware (logs/sk04_sampling/2026-10-06.tsv). Historical: admission is now
// ABOVE it, see the header comment.
#define FETCH_IDLE_MIN_FREE_BYTES 29647u
// Measured whole-download draw with the scratch already held (benchupd2); an input to ADMISSION and to the assert.
#define FETCH_HEAP_MEASURED_DRAW_BYTES 20936u

// Mid-body abort (read loop only, session already built): floor plus the largest single step.
#define FETCH_HEAP_ABORT_BELOW (FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES)
// Admission with the scratch held (what heap_admit() reads). Bottoming out at ABORT_BELOW exactly.
#define FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH (FETCH_HEAP_ABORT_BELOW + FETCH_HEAP_MEASURED_DRAW_BYTES)
// The same level before the scratch is taken: what an idle board must read.
#define FETCH_HEAP_PRECHECK_MIN (FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH + FETCH_HEAP_SCRATCH_BYTES)

_Static_assert(FETCH_HEAP_ABORT_BELOW >= FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES,
               "ABORT_BELOW must be at least the floor plus the largest single allocation between samples");
_Static_assert(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW,
               "post-admission level (scratch held) minus the remaining measured draw must be >= ABORT_BELOW");
_Static_assert(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_SCRATCH_BYTES == FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH,
               "the pre-scratch level is the admission level plus exactly the scratch");

#define FETCH_LARGEST_BLOCK_MIN 6144u
// Check job: TLS handshake only, no writer task. Needs the 2 KB client buffers as the biggest block.
#define FETCH_CHECK_DRAW_BYTES (11044u + 1024u)
#define FETCH_CHECK_BLOCK_MIN 3072u
// Redirect hops after the first: a download uses the start need; a check needs the abort level plus its own draw.
#define FETCH_HOP_DOWNLOAD_HEAP_MIN FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH
#define FETCH_HOP_CHECK_HEAP_MIN (FETCH_HEAP_ABORT_BELOW + FETCH_CHECK_DRAW_BYTES)
_Static_assert(FETCH_HOP_CHECK_HEAP_MIN - FETCH_CHECK_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW,
               "an admitted check hop must not self-abort");
// A LOW_FREE verdict is re-sampled this many times, this far apart, before the request is refused.
#define FETCH_HEAP_SETTLE_TRIES 12u
#define FETCH_HEAP_SETTLE_MS 250u

// DMA-capable internal free, derived from the same source as the internal numbers (UPDFX6, REVIEW_UPDFX5 MED-2).
// Offset of DMA free below internal free, identical in all four bench low-water pairs (header comment).
#define FETCH_DMA_OFFSET_BYTES 7788u
// Sustained-low floor: the DMA reading at which the internal level equals ABORT_BELOW.
#define FETCH_DMA_FREE_MIN (FETCH_HEAP_ABORT_BELOW - FETCH_DMA_OFFSET_BYTES)
// Hard minimum: the DMA reading at which the internal level has already reached the owner floor. Below this the
// floor is crossed (on a board with the measured offset), so there is nothing to wait and see for.
#define FETCH_DMA_HARD_MIN (FETCH_HEAP_FLOOR_BYTES - FETCH_DMA_OFFSET_BYTES)
// DMA admission: DMA free (scratch held) must cover the sustained-low floor plus the same measured draw.
#define FETCH_DMA_ADMIT_DOWNLOAD_MIN (FETCH_DMA_FREE_MIN + FETCH_HEAP_MEASURED_DRAW_BYTES)
#define FETCH_DMA_ADMIT_CHECK_HOP_MIN (FETCH_DMA_FREE_MIN + FETCH_CHECK_DRAW_BYTES)
_Static_assert(FETCH_DMA_ADMIT_DOWNLOAD_MIN - FETCH_HEAP_MEASURED_DRAW_BYTES >= FETCH_DMA_FREE_MIN,
               "a DMA-admitted download must not end under the DMA sustained-low floor");
_Static_assert(FETCH_DMA_FREE_MIN + FETCH_DMA_OFFSET_BYTES >= FETCH_HEAP_ABORT_BELOW,
               "DMA floor must map to at least the internal abort level");
_Static_assert(FETCH_DMA_HARD_MIN + FETCH_DMA_OFFSET_BYTES >= FETCH_HEAP_FLOOR_BYTES,
               "DMA hard minimum must map to at least the internal owner floor");
_Static_assert(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_DMA_OFFSET_BYTES >= FETCH_DMA_ADMIT_DOWNLOAD_MIN,
               "DMA admission must not be stricter than the internal admission on a board with the measured offset");
// Mid-body: the sample is taken only AFTER a read that returned data (a stalled EAGAIN slice is not a read and
// is never sampled). Abort on FETCH_DMA_LOW_READS consecutive low samples, or FETCH_DMA_WINDOW_LOW low samples
// among the last FETCH_DMA_WINDOW (low,low,ok repeating), or one sample under FETCH_DMA_HARD_MIN.
#define FETCH_DMA_LOW_READS 3u
#define FETCH_DMA_WINDOW 8u
#define FETCH_DMA_WINDOW_LOW 4u

typedef enum {
    FETCH_HEAP_OK = 0,
    FETCH_HEAP_LOW_FREE,   // current free internal heap below the start or hop need for this kind
    FETCH_HEAP_LOW_BLOCK,  // largest free internal block below FETCH_LARGEST_BLOCK_MIN
} fetch_heap_verdict_t;

// Admission at a job or redirect-hop start (every fresh TLS session). The caller reads the heap AFTER
// the job's stager scratch is allocated, so the scratch is not charged again. is_download selects the
// download (writer task + stager) or check (TLS + JSON only) draw.
fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block, bool is_download);

// Same, for a redirect hop after the first (heap not back to the job-start level, see "Redirect hops" above).
fetch_heap_verdict_t update_fetch_heap_admit_hop(uint32_t free_internal, uint32_t largest_internal_block,
                                                 bool is_download);

// DMA admission (true = enough): DMA free with the scratch held against the measured DMA draw. A check at a
// redirect hop uses its own, smaller draw; every other case uses the download need.
bool update_fetch_heap_dma_admit(uint32_t dma_free, bool is_download, bool hop);

// Mid-body DMA check after every read that returned data. *hist is caller-owned (start 0, reset per response):
// a bit history of the last FETCH_DMA_WINDOW samples (1 = under FETCH_DMA_FREE_MIN). True = abort the download.
bool update_fetch_heap_dma_abort(uint32_t dma_free, unsigned *hist);

// Mid-body: true when the job must give up.
bool update_fetch_heap_abort(uint32_t free_internal);

#ifdef __cplusplus
}
#endif

#endif
