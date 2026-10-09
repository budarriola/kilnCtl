// debounce_policy.c -- see debounce_policy.h. Logic copied verbatim from
// discrete_task.c's static debounce_update(); do not "simplify" this without
// re-reading that header's behaviour-preservation note.
#include "debounce_policy.h"

bool debounce_policy_update(debounce_policy_state_t *db, bool raw, uint32_t n_samples)
{
    if (db->streak == 0u || raw != db->candidate) {
        db->candidate = raw;
        db->streak = 1u;
    } else if (db->streak < n_samples) {
        db->streak++;
    }

    if (db->streak >= n_samples) {
        db->published = db->candidate;
    }
    return db->published;
}
