#ifndef PROFILE_HISTORY_SEARCH_H
#define PROFILE_HISTORY_SEARCH_H

/* Pure search over a chronologically ordered history ring by each entry's own
 * elapsed_s (LCD audit L24). Ring position is NOT time: the ring wraps at
 * HISTORY_MAX_SAMPLES and no sample is taken on a faulted tick, so index*period
 * drifts from elapsed time. Header-only so the executor (under its lock) and
 * the host tests share one implementation. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t (*profile_history_elapsed_at_fn)(const void *ctx, size_t index);

/* Finds the entry whose elapsed_s is nearest to t_s among n entries (ascending
 * elapsed_s). Returns true and sets *out_index only when that entry is within
 * tol_s of t_s; a bucket before the oldest sample or inside a sampling gap
 * wider than tol_s returns false (draw nothing, never a fabricated value). */
static inline bool profile_history_find_nearest(size_t n, profile_history_elapsed_at_fn elapsed_at, const void *ctx,
                                                float t_s, float tol_s, size_t *out_index)
{
    if (n == 0 || !elapsed_at || !out_index || !(t_s >= 0.0f)) {
        return false;
    }
    /* First index whose elapsed_s >= t_s. */
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if ((float)elapsed_at(ctx, mid) < t_s) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    size_t best = (lo < n) ? lo : n - 1u;
    if (lo > 0u && lo < n) {
        float d_lo = t_s - (float)elapsed_at(ctx, lo - 1u);
        float d_hi = (float)elapsed_at(ctx, lo) - t_s;
        if (d_lo <= d_hi) {
            best = lo - 1u;
        }
    }
    float d = t_s - (float)elapsed_at(ctx, best);
    if (d < 0.0f) {
        d = -d;
    }
    if (d > tol_s) {
        return false;
    }
    *out_index = best;
    return true;
}

#endif
