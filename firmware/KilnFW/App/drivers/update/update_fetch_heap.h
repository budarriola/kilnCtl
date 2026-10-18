// update_fetch_heap.h -- the internal-heap admission rule for the GitHub fetch (WP8), pure C so
// the decision is host-tested (App/test/test_update_fetch_heap.c). update_fetch.c supplies the
// live numbers (heap_caps_get_free_size / heap_caps_get_largest_free_block, MALLOC_CAP_INTERNAL, and
// the DMA-capable internal free).
//
// Owner floor: heap_internal min_free >= 8192 B. This is a model, not a guarantee (see "Limits").
//
// THE MODEL (UPDFX7 re-derivation of UPDFX6; REVIEW_UPDFX6 MED-1/MED-2/LOW-1..3). Every level is the live
// reading taken by heap_admit() in update_fetch.c, which runs AFTER fetch_task() has allocated the 1024 B
// stager scratch. So "admission level" means the level with the scratch already held, and a "draw" is on that
// same base: idle free (scratch held) minus the global low-water of the whole run. A draw is NOT the change of
// a prior min_free (REVIEW_UPDFX6 MED-1: the previous text mistook benchupd4's 2908 B min_free delta for one).
//
//   FLOOR                8192 B   owner floor
//   MAX_STEP             1024 B   margin for heap movement between two samples (LOW-1 below)
//   ABORT_BELOW          9216 B   = FLOOR + MAX_STEP: a SAMPLED BACKSTOP, not a guarantee (UPDFX8, REVIEW_UPDFX7
//                                 MED-1). The abort fires on a sample under this level; nothing bounds the movement
//                                 between two samples (see "Limits"). What actually protects the floor is the draw
//                                 model plus the admission margin below.
//   MEASURED_DRAW       21488 B   the LARGEST measured whole-run draw, scratch held. Draw table:
//                                   run        firmware   idle   low-water  from idle  scratch held
//                                   benchupd2  c549a144c  30299    8339      21960      20936
//                                   benchupd3  c549a144c  30427   10079      20348      19324
//                                   benchupd4  36f3ad031  47191   24679      22512      21488  <- maximum
//                                   benchupd5  468f0c5cb  34783   14115      20668      19644  (UART ISR in IRAM)
//                                   xipland C  e9b50d8f8  45423   28463      16960      15936  (CURRENT: XIP from PSRAM, ISR not in IRAM)
//                                 The xipland row is from BENCH_TEST_LOG.md "2026-10-14 xipland": fetch start free
//                                 (scratch held) 44399 B, low-water 28463 B, so the draw is 44399 - 28463 = 15936 B
//                                 (idle is the start free plus the 1024 B scratch; the 45.4..45.9 KB idle band there
//                                 is the same figure). The older rows are history: benchupd2..5 ran firmware with the
//                                 ISR-in-IRAM or the pre-XIP layout, which this tree no longer builds.
//   DRAW_MARGIN          1024 B   BENCH-PENDING (UPDFX8, REVIEW_UPDFX7 MED-2): explicit margin over the maximum: the runs spread 19.3..21.5 KB (2.2 KB)
//                                 because every low-water includes concurrent httpd/MCP/LVGL activity, so no
//                                 single run is a bound
//   MODEL_DRAW          22512 B   = MEASURED_DRAW + DRAW_MARGIN
//   BENCH-PENDING: the margin (1024 B) is under half the observed spread (2.2 KB) of only five runs, and only ONE
//   (xipland C) ran on the CURRENT firmware (XIP from PSRAM, ISR not in IRAM); benchupd5 ran the superseded
//   ISR-in-IRAM build. The "5 readings" rule counts readings from the CURRENT build only (GET /api/update/fetch,
//   heap.min_free): one is in the table, four more are needed. Do not call these numbers sized, and do not lower
//   ADMISSION further, until then. The XIP draw (15.9 KB) is smaller than every older one, so MEASURED_DRAW stays the
//   conservative maximum from benchupd4.
//   ADMISSION (held)    31728 B   = ABORT_BELOW + MODEL_DRAW (9216 + 22512). If the draw is within MODEL_DRAW
//                                 (not yet established, see BENCH-PENDING), an admitted fetch bottoms out at or
//                                 above ABORT_BELOW with the margin to spare.
//   same before scratch 32752 B   = ADMISSION + SCRATCH (FETCH_HEAP_PRECHECK_MIN: what an idle board must show)
//   floor check         10240 B   = ADMISSION - MEASURED_DRAW: the worst measured fetch, admitted exactly at the
//                                 edge, still ends 2048 B above the 8192 B floor (1024 B above ABORT_BELOW)
//
// Idle boards against this rule (admitted when idle >= 32752 B before the scratch):
//   xipland C e9b50d8f8, XIP from PSRAM (CURRENT build)    45423 B  ADMITTED, margin 45423 - 32752 = +12671 B
//                                                         (start free 44399 B after the scratch; the
//                                                         pre-XIP dev tip (A) was refused with low_heap_free)
//   superseded: 468f0c5cb, UART_ISR_IN_IRAM (benchupd5) 34783 B  ADMITTED, margin 34783 - 32752 = +2031 B
//                                                         (it was +535 B against the UPDFX6 need of 34248 B)
//   36f3ad031, before the UART change (benchupd4)         47191 B  ADMITTED (about 14 KB over)
//   older firmware (sk04 log 2026-10-06; update_check)    29647..31123 B, 29815 B  REFUSED
//   No rule that is consistent with the measured draw can admit the older figures: from 29647 B the benchupd4
//   draw bottoms at 29647 - 1024 - 21488 = 7135 B, 1057 B under the floor. A refused start fails with
//   low_heap_free after the settle wait; it never runs into the floor. Re-measure on the bench before lowering
//   ADMISSION.
//
// LOW-1 (MAX_STEP). ABORT_BELOW is a backstop (UPDFX8), see "Limits" for what it does not bound. REVIEW_UPDFX6 LOW-1 showed the old 4096 B justification (a flash-write bounce buffer) does
// not exist. Re-derived from the allocations one read-loop pass actually makes (ESP-IDF v6.0.2, the toolchain
// this tree builds with, /c/esp/v6.0.2/esp-idf):
//   - flash write: esp_flash_write copies a non-DRAM source through a 32 B STACK buffer, no heap
//     (components/spi_flash/esp_flash_api.c:1135-1142, `uint32_t temp_buf[8]`);
//   - SHA-256 (hardware SHA DMA): a partial block is copied into an internal DMA buffer of at most one block,
//     64 B (components/mbedtls/port/sha/core/sha.c:335, buf_len <= block_length checked at :322); the reallocs at
//     :172/:185 are the flash-encryption-only paths and are inactive here (flash encryption is off);
//   - mbedtls record buffers: PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y); the client rx/tx buffers (2048 B each)
//     are allocated at open, before the loop; lwIP/WiFi pbufs prefer PSRAM and the WiFi static rx buffers are
//     preallocated.
// So the fetch itself moves internal heap by about 100 B per pass. The 1024 B is therefore NOT a fetch
// allocation: it is an empirical margin for concurrent tasks (one httpd request-handler allocation, a KDF, an
// LVGL object) landing between two samples, sized as one stager scratch. It is a margin, not a derived bound; the
// residual is in "Limits". update_fetch.c no longer asserts the read chunk or the tx buffer against it: neither
// is a per-pass internal allocation.
//
// Redirect hops (MED-2, LOW-3). Every redirect hop opens a fresh TLS session while the previous request's
// lwIP/TLS teardown is still draining: the heap at the 302 hop reads FETCH_HOP_DIP_BYTES below the scratch-held
// job start (benchupd: 27855 B at the hop against 29275 B = 30299 - 1024 at the start, a 1420 B dip, rounded up
// to 1536 B). That dip is already inside the whole-run draw (the draw is idle minus the global low-water), so at
// the hop only MODEL_DRAW - DIP of the draw is left, and a download hop needs
//   ABORT_BELOW + MODEL_DRAW - HOP_DIP = 9216 + 22512 - 1536 = 30192 B.
// Floor arithmetic at the hop: 30192 - (22512 - 1536) = 9216 B at the model draw; 30192 - (21488 - 1420) = 10124 B
// at the worst measured draw. On the benchupd5 idle board the hop reads about 34783 - 1024 - 1420 = 32339 B,
// 2147 B over the hop need with no settle wait at all. Because the hop need is below the start need, a board
// admitted at the start edge (31728 B held) reaches the hop at about 31728 - 1420 = 30308 B, still admitted
// (+116 B); the effective idle need for a download that survives its redirect is the start need, 32752 B, and
// does not depend on the 3 s settle recovering the teardown. A LOW_FREE verdict is still re-sampled
// FETCH_HEAP_SETTLE_TRIES x FETCH_HEAP_SETTLE_MS (12 x 250 ms = 3 s, cancel/deadline/heat aware, in
// update_fetch.c); a heap that never recovers is refused low_heap_free. The settle result at the hop and the
// job's low-waters are exported in GET /api/update/fetch ("heap" object). A check hop needs ABORT_BELOW +
// CHECK_DRAW (a check runs the same abort loop); the first admission of a check uses the download need (a check
// that starts is a precursor to a download).
//
// DMA (LOW-2; UPDFX8 REVIEW_UPDFX7 LOW-1: rtc_free is the smallest offset of FETCH_DMA_OFFSET_PAIRS (internal, DMA)
// pairs, so a concurrent allocation between one pair's two reads cannot lower the DMA floor
// provided at least one pair is clean: a reduction, not a bound. Pair 0's internal reading is taken before
// the largest-block read and a heap_note lock cycle, so its pair has the widest window; pairs 1.. are read back-to-back).
// internal free = DMA free + RTC-heap free: with CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y the
// RTC FAST heap (7788 B free on the bench in every pair) is internal but not DMA-capable. The guard no longer
// hard-codes 7788: heap_admit() derives rtc_free = internal free - DMA free at the job's first admission
// (update_fetch_heap_dma_offset) and uses it for every DMA number of that job, so a build with a different RTC
// heap can neither wrongly refuse nor silently weaken the guard. Every DMA need is the matching internal need
// less rtc_free: admission = ADMISSION - rtc_free (31728 - 7788 = 23940 B on the bench), sustained-low floor =
// ABORT_BELOW - rtc_free (1428 B), hard minimum = FLOOR - rtc_free (404 B: at the owner floor only ~404 B of
// DMA-capable DRAM is left, which is what the floor means on this board). At the FIRST admission the DMA check
// re-judges the picked pair (pf[best]), not pair 0: it can refuse where the internal verdict (pair 0) admitted if
// free fell after pair 0; its main value is the later samples: after every read that returned data, DMA free is compared against the offset fixed at the job
// start, so DMA-capable memory that falls faster than the internal total is caught by the window/consecutive
// rule and the hard minimum. Predicted on the benchupd5 idle board: DMA free 26995 - 1024 = 25971 B against a
// need of 23940 B, +2031 B.
//
// GP-01: update_fetch_wr's stack was raised 4096 -> 5120 B. The table above was measured with 4096 B; the extra
// 1024 B of internal RAM is NOT added to MEASURED_DRAW (that would move every pinned number and host test). It
// consumes DRAW_MARGIN (1024 B) in full; idle bench min_free is far above the 8192 B floor (about 96 KB). The
// BENCH-PENDING re-measure covers it.
//
// Largest free block: the biggest single internal allocation at start is the 5120 B update_fetch_wr stack
// (+ overhead); 6144 B = 1.2x passes the idle largest block (9728 B, sk04 log; 11776 B in benchupd5, low-water
// 7680 B in the TLS handshake). A check needs only the 2 KB client buffers (3072 B).
//
// Limits (review 2026-10-07, REVIEW_UPDFX4/5/6/7).
// ABORT_BELOW is a sampled backstop (REVIEW_UPDFX7 MED-1). Concurrent tasks allocate internal RAM in blocks larger
// than MAX_STEP: profile_exec_status_t (1512 B) is heap_caps_malloc'd MALLOC_CAP_INTERNAL on about 10 paths
// (safety_build_and_send_context, ui_page_home_refresh, screen_idle, dashboard_exec_http, ota_http,
// safety_cfg_http, uart_bridge_ext_control), 2-3 of them can be in flight (about 3-4.5 KB), and
// diagnostics_http.c allocates 8192 B internal; the Wi-Fi blob's own internal RX buffers are not visible from
// source. So one sample can read above ABORT_BELOW and the next below FLOOR; the abort cannot prevent that.
// Protection of the floor is the draw model (whole-run low-waters already contain these transients) plus the
// admission margin: the worst measured run admitted at the edge ends 2048 B above the floor (n = 4 runs,
// 2.2 KB spread; the 258b80d4 dip to 8295 B and benchupd2's 8339 B both sat within about 150 B of the floor). The
// residual risk is a run whose draw exceeds MODEL_DRAW; a steady-state lowest-sample rule cannot remove it, only
// more bench evidence can narrow it.
// DMA floor residual (REVIEW_UPDFX7 LOW-3): the sustained DMA floor is 1428 B, about 3 KB closer to exhaustion than
// the former 4500 B. Runtime MALLOC_CAP_DMA allocators outside the fetch: the MAX31856 SPI bounce buffer (at most
// about 20 B, an unaligned stack source on the DMA-enabled shared bus; failure is a thermocouple read error, which
// fails safe: fault, heat off, and the operator must clear the latched thermo fault), the AES DMA bounce (up to
// 1600 B for a non-GCM suite with an unaligned PSRAM record; GCM bounces about 16 B and GitHub negotiates GCM, not
// proven), and the Wi-Fi blob's RX/TX DMA (not verifiable). The fetch refuses to run during a heat run, so the
// residual is an idle-board thermo read error during a fetch. Admission is the ONLY guard for the TLS handshake,
// esp_http_client_open and anything inside a single esp_http_client_read: the abort runs between reads only.
// The numbers are a snapshot (TOCTOU), the draw is the maximum of four bench runs plus a stated margin, and the
// board's own transients are larger than any margin here (idle low-water 16555 B against idle free 31123 B). The
// 258b80d4 bench run that dipped to 8295 B is not explained by this rule. Bench-pending: a fetch on a board
// flashed with this build; GET /api/update/fetch reports heap.start_free, heap.hop_*, heap.min_free,
// heap.min_dma and heap.min_block for it.
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
// Margin for heap movement between two samples (LOW-1 above): empirical, not a per-pass fetch allocation.
// Not a bound: ABORT_BELOW is a sampled backstop (see "Limits").
#define FETCH_HEAP_MAX_STEP_BYTES 1024u
// Idle bench minimum of the older firmware (logs/sk04_sampling/2026-10-06.tsv). Historical: admission is now
// ABOVE it, see the header comment.
#define FETCH_IDLE_MIN_FREE_BYTES 29647u
// Largest measured whole-run draw with the scratch held (benchupd4, table above) plus the explicit margin.
// BENCH-PENDING (UPDFX8): needs at least 5 heap.min_free readings from this build before any further lowering.
#define FETCH_HEAP_MEASURED_DRAW_BYTES 21488u
#define FETCH_HEAP_DRAW_MARGIN_BYTES 1024u
#define FETCH_HEAP_MODEL_DRAW_BYTES (FETCH_HEAP_MEASURED_DRAW_BYTES + FETCH_HEAP_DRAW_MARGIN_BYTES)

// Mid-body abort (read loop only, session already built): floor plus the step margin.
#define FETCH_HEAP_ABORT_BELOW (FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES)
// Admission with the scratch held (what heap_admit() reads). Bottoming out at ABORT_BELOW exactly at the model draw.
#define FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH (FETCH_HEAP_ABORT_BELOW + FETCH_HEAP_MODEL_DRAW_BYTES)
// The same level before the scratch is taken: what an idle board must read.
#define FETCH_HEAP_PRECHECK_MIN (FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH + FETCH_HEAP_SCRATCH_BYTES)

_Static_assert(FETCH_HEAP_ABORT_BELOW == FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES,
               "ABORT_BELOW is the floor plus the between-samples margin");
_Static_assert(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MEASURED_DRAW_BYTES >=
                   FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_MAX_STEP_BYTES + FETCH_HEAP_DRAW_MARGIN_BYTES,
               "the worst measured draw, admitted at the edge, must end above the owner floor by step + margin");
_Static_assert(FETCH_HEAP_PRECHECK_MIN - FETCH_HEAP_SCRATCH_BYTES == FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH,
               "the pre-scratch level is the admission level plus exactly the scratch");

#define FETCH_LARGEST_BLOCK_MIN 6144u
// Check job: TLS handshake only, no writer task. Needs the 2 KB client buffers as the biggest block.
#define FETCH_CHECK_DRAW_BYTES (11044u + 1024u)
#define FETCH_CHECK_BLOCK_MIN 3072u
// Heap at a redirect hop below the scratch-held job start (benchupd: 29275 -> 27855 = 1420 B, rounded up).
#define FETCH_HOP_DIP_BYTES 1536u
// Redirect hops after the first: a download needs the start need less the dip (the dip is part of the draw); a
// check needs the abort level plus its own draw.
#define FETCH_HOP_DOWNLOAD_HEAP_MIN (FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HOP_DIP_BYTES)
#define FETCH_HOP_CHECK_HEAP_MIN (FETCH_HEAP_ABORT_BELOW + FETCH_CHECK_DRAW_BYTES)
_Static_assert(FETCH_HOP_DOWNLOAD_HEAP_MIN - (FETCH_HEAP_MODEL_DRAW_BYTES - FETCH_HOP_DIP_BYTES) == FETCH_HEAP_ABORT_BELOW,
               "a download admitted at the hop edge ends at the abort level at the model draw less the dip already taken");
_Static_assert(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HOP_DIP_BYTES >= FETCH_HOP_DOWNLOAD_HEAP_MIN,
               "a board admitted at the start edge must reach the hop admitted after the measured dip");
_Static_assert(FETCH_HOP_CHECK_HEAP_MIN - FETCH_CHECK_DRAW_BYTES >= FETCH_HEAP_ABORT_BELOW,
               "an admitted check hop must not self-abort");
// A LOW_FREE verdict is re-sampled this many times, this far apart, before the request is refused.
#define FETCH_HEAP_SETTLE_TRIES 12u
#define FETCH_HEAP_SETTLE_MS 250u

// DMA-capable internal free (LOW-2 above). All DMA numbers are the internal numbers less the job's rtc_free
// offset, computed at run time from update_fetch_heap_dma_offset(); nothing here hard-codes an offset.
// Mid-body: the sample is taken only AFTER a read that returned data (a stalled EAGAIN slice is not a read and
// is never sampled). Abort on FETCH_DMA_LOW_READS consecutive low samples, or FETCH_DMA_WINDOW_LOW low samples
// among the last FETCH_DMA_WINDOW (low,low,ok repeating), or one sample under the hard minimum.
#define FETCH_DMA_LOW_READS 3u
// (internal, DMA) pairs read at the job's first admission to derive rtc_free (UPDFX8 LOW-1).
#define FETCH_DMA_OFFSET_PAIRS 3u
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

// Internal free needed at a start (hop=false) or a redirect hop (hop=true) for this kind: what the two admit
// functions compare against. Used by the DMA needs and by the refusal log.
uint32_t update_fetch_heap_need(bool is_download, bool hop);

// RTC-heap free (internal but not DMA-capable) derived from one pair of readings; saturates at 0.
uint32_t update_fetch_heap_dma_offset(uint32_t free_internal, uint32_t dma_free);

// Index of the pair (f[i], dma[i]), i < n, with the smallest RTC offset (internal free - DMA free, saturating at 0);
// ties go to the lower DMA reading. The smallest offset is the conservative one: it gives the highest DMA floor.
unsigned update_fetch_heap_dma_pick_pair(const uint32_t *f, const uint32_t *dma, unsigned n);

// DMA needs for a job whose offset is rtc_free; each is the internal quantity less rtc_free, saturating at 0.
uint32_t update_fetch_heap_dma_need(bool is_download, bool hop, uint32_t rtc_free);  // admission
uint32_t update_fetch_heap_dma_floor(uint32_t rtc_free);                            // sustained-low (ABORT_BELOW)
uint32_t update_fetch_heap_dma_hard_min(uint32_t rtc_free);                         // owner floor

// DMA admission (true = enough): DMA free with the scratch held against update_fetch_heap_dma_need().
bool update_fetch_heap_dma_admit(uint32_t dma_free, uint32_t rtc_free, bool is_download, bool hop);

// Mid-body DMA check after every read that returned data. *hist is caller-owned (start 0, reset per response):
// a bit history of the last FETCH_DMA_WINDOW samples (1 = under the sustained-low floor). True = abort.
bool update_fetch_heap_dma_abort(uint32_t dma_free, uint32_t rtc_free, unsigned *hist);

// Mid-body: true when the job must give up.
bool update_fetch_heap_abort(uint32_t free_internal);

#ifdef __cplusplus
}
#endif

#endif
