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
//                                                   releases/latest only, never the asset hop)
//   (the 16500 B above now also includes the 2048 B internal stager scratch, FETCH_HEAP_SCRATCH_BYTES,
//    so worst draw = 18548 B and slack = 1932 B; the PRECHECK sum is unchanged)
//   slack, concurrent httpd / login KDF   3980 B (1932 B after the scratch)
//   REVISED (review 3 LOW-5): worst draw 18548 B, floor 8192 B, concurrent KDF/httpd margin 4096 B
//   -> FETCH_HEAP_PRECHECK_MIN           30836 B   (8192 + 18548 + 4096). The idle board sits at
//      29647-31123 B free, so an idle-minimum board is now REFUSED (retry later) rather than admitted
//      with only 1932 B of margin; the older 28672 B figure above is superseded.
// REVISED AGAIN (bench finding, origin/dev 8fcd3237: update_check refused at idle, 29815 B free < 30836 B):
//   floor                                8192 B  (owner floor, unchanged)
//   worst draw                          17524 B  (16500 + 1024 B stager scratch; scratch cut 2048 -> 1024,
//                                                 UPDATE_STAGE_SCRATCH_MIN is 256, see
//                                                 docs/audits/UPDATE_FETCH_HEAP_DRAW_2026-10-09.md route 4)
//   concurrent httpd / KDF margin        3840 B  (was 4096). The login KDF (web_auth_store.c) does no heap
//                                                 allocation of its own (iterated HMAC on the caller's stack),
//                                                 so the margin only covers a concurrent httpd request; it
//                                                 is NOT shown mutually exclusive with the fetch, so kept.
//   -> FETCH_HEAP_PRECHECK_MIN          29556 B  (8192 + 17524 + 3840), below the 29647 B idle minimum;
//      at the idle minimum the post-draw margin over the floor is 29647 - 17524 - 8192 = 3931 B.
//      (old: 8192 + 18548 + 4096 = 30836 B, above idle.) Estimate, not a measurement: the WP8 gate
//      (b) bench run must confirm min_free >= 8192 B with scratch 1024.// The slack is deliberately NOT larger: the board idles at 29647-31123 B free
// (logs/sk04_sampling/2026-10-06.tsv, 29 samples), so anything above ~29.6 KB would refuse on an
// idle board. At the idle minimum the fetch's own estimated draw ends at 29647 - 16500 = 13147 B,
// 4955 B above the 8192 B floor.
// Largest block at idle is 9728 B (same log), so the block floor must sit below that. The biggest
// single internal allocation is the 4096 B update_fetch_wr stack (+ heap overhead); the 2 KB
// client buffers and TLS pieces are smaller and separate. 6144 B = 1.5x the stack, passes at idle.
//
// Limits (review 2026-10-07). This is an admission check, not a guarantee of the floor:
//  - It would also have admitted the 258b80d4 bench run that dipped to min_free 8295 B if that run
//    started near idle (31123 >= 28672). From an idle start that dip is a draw of up to about
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
// the old 1932 B left almost nothing against the 8192 B floor). Admitted free minus the full worst draw
// (18548 B) must still leave floor + this margin.
#define FETCH_HEAP_SLACK_BYTES 3840u
#define FETCH_HEAP_PRECHECK_MIN (FETCH_HEAP_FLOOR_BYTES + FETCH_HEAP_WORST_DRAW_BYTES + FETCH_HEAP_SLACK_BYTES)
#define FETCH_LARGEST_BLOCK_MIN 6144u
// Mid-body abort (read loop only, session already built): floor plus 4 KB of transient room.
#define FETCH_HEAP_ABORT_BELOW (FETCH_HEAP_FLOOR_BYTES + 4096u)

typedef enum {
    FETCH_HEAP_OK = 0,
    FETCH_HEAP_LOW_FREE,   // current free internal heap below FETCH_HEAP_PRECHECK_MIN
    FETCH_HEAP_LOW_BLOCK,  // largest free internal block below FETCH_LARGEST_BLOCK_MIN
} fetch_heap_verdict_t;

// Admission at a job or redirect-hop start (every fresh TLS session).
fetch_heap_verdict_t update_fetch_heap_admit(uint32_t free_internal, uint32_t largest_internal_block);

// Mid-body: true when the job must give up.
bool update_fetch_heap_abort(uint32_t free_internal);

#ifdef __cplusplus
}
#endif

#endif
