// Arbiter for the flash-writer op / caller-timeout race (review 5 L1). Pure state logic with no OS
// calls: the caller serializes every function below with one critical section, so exactly one side
// decides an op's fate.
//   PENDING  op issued, neither side has decided
//   DONE     the writer finished first; the caller must treat the op as completed (not wedged)
//   WEDGED   the caller timed out first; the writer must undo its own late result and nobody waits
#pragma once

#include <stdbool.h>

typedef enum { UPDATE_WR_ARB_IDLE = 0, UPDATE_WR_ARB_PENDING, UPDATE_WR_ARB_DONE, UPDATE_WR_ARB_WEDGED } update_wr_arb_t;

static inline void update_wr_arb_issue(volatile update_wr_arb_t *a)
{
    *a = UPDATE_WR_ARB_PENDING;
}

// Writer, after the op returned. true = the caller already gave up: run the cleanup, do not report.
static inline bool update_wr_arb_writer_done(volatile update_wr_arb_t *a)
{
    if (*a == UPDATE_WR_ARB_WEDGED) {
        return true;
    }
    *a = UPDATE_WR_ARB_DONE;
    return false;
}

// Caller, on timeout. true = the op is now abandoned (wedged). false = the writer finished in the
// same instant: collect its result as a normal completion.
static inline bool update_wr_arb_caller_timeout(volatile update_wr_arb_t *a)
{
    if (*a == UPDATE_WR_ARB_DONE) {
        return false;
    }
    *a = UPDATE_WR_ARB_WEDGED;
    return true;
}
