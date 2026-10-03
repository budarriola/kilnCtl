/* Single-flight guard over "who is currently writing the Pico's safety
 * config" -- docs/HTTP_POST_OWNER_MIGRATION_PLAN.md A2's open interleaving
 * gap. Exactly ONE of these may own the guard at a time:
 *
 *   ASYNC_JOB  http_async_job.c's job task (ct_auto_zero, bench_preset,
 *              backup_import) -- taken by http_async_job_try_start() on
 *              httpd_worker, released by run_job() when the job ends.
 *   SWEEP      zones_current_sweep_task.c -- taken by
 *              zones_current_sweep_start(), released by the sweep task as
 *              its last act (and on every failed-start path).
 *   SWAP       kiln_cfg_swap_worker.c -- taken by kiln_cfg_swap_worker_submit()
 *              (and around boot recovery), released by the worker when the
 *              apply finishes (and on a failed queue send).
 *   RECONCILE  safety_ceiling_sync.c's NON-BLOCKING (safety_poll) reconcile,
 *              held only around its guard_raise() write attempt.
 *
 * Every holder does SET_PARAM/COMMIT_CONFIG round trips against the same
 * Pico staged-config transaction; two of them interleaving can commit each
 * other's half-staged values. A claim that cannot be had is REFUSED, never
 * waited for: callers either refuse their own request (HTTP 409 / sweep
 * refusal / swap reason) or skip a level-triggered tick (reconcile).
 *
 * Leaf lock: the guard's own spinlock is never held across any other call
 * and takes nothing else, so it sits below every module lock (s_exec.lock,
 * s_at.lock, s_reconcile_lock, relay_authority) in the lock order and can be
 * called with any of them held. Never hold a MODULE lock across the
 * protected write itself -- that is the caller's own existing rule.
 *
 * Not reentrant, deliberately. The one nested case, kiln_cfg_swap.c calling
 * the BLOCKING safety_ceiling_sync_reconcile_on_link_up() while SWAP is
 * held, does not claim (the blocking entry is only ever reached from the
 * swap path, which already owns the guard).
 *
 * "Reset one side of a pair": release names its owner. A release by a
 * non-owner is refused and logged, never clears someone else's claim -- so
 * a stray or double release cannot silently reopen the window under a
 * different holder. Every claim must be paired with exactly one release on
 * every path (success, failure, early return). */
#ifndef SAFETY_CFG_WRITER_GUARD_H
#define SAFETY_CFG_WRITER_GUARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAFETY_CFG_WRITER_NONE = 0,
    SAFETY_CFG_WRITER_ASYNC_JOB,
    SAFETY_CFG_WRITER_SWEEP,
    SAFETY_CFG_WRITER_SWAP,
    SAFETY_CFG_WRITER_RECONCILE,
} safety_cfg_writer_t;

/* Atomic test-and-set. True and `who` now owns the guard; false if ANY
 * owner (including `who` itself) already holds it. `who` must not be NONE. */
bool safety_cfg_writer_try_claim(safety_cfg_writer_t who);

/* Releases the guard iff `who` is the current owner. Returns false (and logs)
 * if it was not -- nothing is changed in that case. */
bool safety_cfg_writer_release(safety_cfg_writer_t who);

/* Current owner, NONE when free. Informational snapshot only -- never
 * test-then-claim off it; use safety_cfg_writer_try_claim(). */
safety_cfg_writer_t safety_cfg_writer_owner(void);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_WRITER_GUARD_H
