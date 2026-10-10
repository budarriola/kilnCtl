// trip_seq.h -- next trip-event sequence number (safety link review
// 2026-10-09 F4). Header-only and link-free on purpose: safety_core.c may not
// include any link/uart-named header (docs/ARCHITECTURE.md section 2,
// tools/check_isolation.ps1), and F4's first version put this helper in
// link_frame.h, which broke that rule. safety_core.c and the host tests use
// this function; link_frame.c no longer does (its copy was removed in cf3359523).
//
// 0 means "no trip yet this boot" (safety_core_get_trip_event() returns
// false, DIAG reports no trip, the ESP treats seq 0 as the unbound/none
// value), so the uint8_t counter must wrap 255 -> 1, never back to 0: a wrap
// to 0 made the 256th trip look like "never tripped" and its event was never
// reported.
#ifndef SAFTYFW_TASKS_TRIP_SEQ_H
#define SAFTYFW_TASKS_TRIP_SEQ_H

#include <stdint.h>

static inline uint8_t trip_seq_next(uint8_t current)
{
    uint8_t next = (uint8_t)(current + 1u);
    return next == 0u ? 1u : next;
}

#endif // SAFTYFW_TASKS_TRIP_SEQ_H
