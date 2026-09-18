// watchdog_gate.c -- see watchdog_gate.h.
#include "watchdog_gate.h"

#include <stddef.h>

bool watchdog_gate_all_within_deadline(const watchdog_gate_entry_t *entries, uint32_t count,
                                        uint32_t *out_ok_mask)
{
    bool all_ok = true;
    uint32_t mask = 0;

    for (uint32_t i = 0; i < count && i < 32u; i++) {
        if (entries[i].elapsed_ms <= entries[i].deadline_ms) {
            mask |= (1u << i);
        } else {
            all_ok = false;
        }
    }

    if (out_ok_mask) {
        *out_ok_mask = mask;
    }
    return all_ok;
}

bool watchdog_gate_feed_if_all_within_deadline(const watchdog_gate_entry_t *entries, uint32_t count,
                                                uint32_t *out_ok_mask, void (*feed)(void))
{
    // Deliberately delegates rather than reimplementing the comparison: this
    // function owns WHEN a feed happens, watchdog_gate_all_within_deadline()
    // owns WHAT "within its own deadline" means, and there is exactly one of
    // each in this firmware.
    bool all_ok = watchdog_gate_all_within_deadline(entries, count, out_ok_mask);

    if (all_ok && feed != NULL) {
        feed();
    }

    return all_ok;
}
