// update_fetch_heap.h -- the internal-heap admission rule for the GitHub fetch (WP8), pure C so
// the decision is host-tested (App/test/test_update_fetch_heap.c). update_fetch.c supplies the
// two live numbers (heap_caps_get_free_size / heap_caps_get_largest_free_block, MALLOC_CAP_INTERNAL).
//
// Owner floor: heap_internal min_free >= 8192 B. A start is admitted only when the CURRENT free
// internal heap covers floor + worst measured fetch draw + slack, so the low-water mark cannot
// dip under the floor, AND the largest free block can hold the biggest single internal
// allocation (the 4 KB update_fetch_wr stack plus its TCB, the 2 KB request buffer).
//
// Derivation (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 14, WP8 heap budget):
//   floor                                 8192 B
//   worst-case fetch draw, option D      16500 B   (13.5-16.5 KB measured/estimated: TLS residual
//                                                   8-11 KB, writer stack 4.5 KB, 2 KB tx buffer;
//                                                   mbedTLS buffers are PSRAM, so not counted)
//   slack, concurrent httpd / login KDF   3980 B
//   -> FETCH_HEAP_PRECHECK_MIN           28672 B   (28 KB)
// The slack is deliberately NOT larger: the board idles at 29647-31123 B free
// (logs/sk04_sampling/2026-10-06.tsv, 30 samples), so anything above ~29.6 KB would refuse on an
// idle board. At the idle minimum the fetch still ends with 29647 - 16500 = 13147 B headroom
// above the low-water mark's 8192 B floor even before slack.
// Largest block at idle is 9728 B (same log), so the block floor must sit below that. The biggest
// single internal allocation is the 4096 B update_fetch_wr stack (+ heap overhead); the 2 KB
// request buffer and TLS pieces are smaller and separate. 6144 B = 1.5x the stack, passes at idle.
#ifndef KILNCTL_UPDATE_FETCH_HEAP_H
#define KILNCTL_UPDATE_FETCH_HEAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FETCH_HEAP_FLOOR_BYTES 8192u
#define FETCH_HEAP_WORST_DRAW_BYTES 16500u
#define FETCH_HEAP_SLACK_BYTES 3980u
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
