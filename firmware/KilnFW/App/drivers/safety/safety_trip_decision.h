// safety_trip_decision -- the pure boolean logic behind
// safety_apply_trip_event()'s "is this trip_seq new, and if so is the
// fault-source snapshot trustworthy" decision, factored out of
// safety_link.c so a host test can drive it directly.
//
// 2026-08-28 (opus review): safety_apply_trip_event()'s
// trip_fault_sources_valid logic had NO automated test, because no host
// harness includes safety_link.c -- it is ~3900 lines, pulls in
// driver/gpio.h, driver/uart.h, esp_heap_caps.h, esp_random.h, esp_timer.h,
// a dozen kilnlink/* codecs, and safety_cfg_store.h (a file this task's
// FILES YOU OWN list excludes -- it belongs to another agent right now), so
// stub-linking the whole translation unit the way test_ota_http.c does for
// ota_http.c (commit da4918c) would mean either fabricating stub headers
// for most of ESP-IDF's driver layer or reaching into a file currently
// off-limits. The decision itself is three lines of plain boolean algebra
// over three plain values (was a trip ever received before, what trip_seq
// was cached, what trip_seq just arrived) with no I/O, no lock, and no
// FreeRTOS call anywhere in it -- exactly the shape that belongs in its own
// tiny, dependency-free translation unit instead. This is that unit.
//
// The flag this decides (safety_link.h's trip_fault_sources_valid) exists
// because an ESP reboot with a trip still latched on the Pico makes the
// FIRST post-boot copy of that trip look identical, from trip_event_ever_
// received's point of view, to a real new trip -- but this boot's
// link->fault_sources at that instant reflects THIS boot's fault lines, not
// whatever was actually asserted when the trip latched, possibly minutes or
// boots earlier. Only a trip_seq CHANGE witnessed while this boot was
// already tracking a previous one is something this boot actually watched
// happen live; only that case may claim the fault-source snapshot is
// trustworthy enough to show an operator as "this is what caused it" rather
// than "not captured" (see ui_page_diagnostics.c/dashboard_http.c's
// "(at trip)" renderers -- both outside this task's FILES YOU OWN list, so
// this header does not touch them, only the decision they read).
#ifndef SAFETY_TRIP_DECISION_H
#define SAFETY_TRIP_DECISION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Whether ANY trip event has ever been applied this boot before this
     * one -- safety_link.h's trip_event_ever_received, sampled BEFORE this
     * frame is applied. */
    bool trip_event_ever_received_before;
    /* The trip_seq of the most recently applied event before this one
     * (meaningless when trip_event_ever_received_before is false, same as
     * the real cached.trip_last_seq field before the first event ever
     * lands). */
    uint8_t cached_trip_last_seq;
    /* This frame's trip_seq (LINK_PROTOCOL.md sec 6 byte1). */
    uint8_t incoming_trip_seq;
} safety_trip_decision_input_t;

typedef struct {
    /* True when this trip_seq differs from what was already cached (or
     * nothing was cached yet) -- safety_apply_trip_event()'s is_new_event.
     * Gates whether the cached trip_* fields (reason, uptime, temperatures,
     * ...) are overwritten at all: a dedup resend of the same trip_seq must
     * NOT overwrite them, so a healthy resend burst does not look like the
     * trip's own recorded details are flapping. */
    bool is_new_event;
    /* True only when is_new_event is ALSO true AND a trip was already being
     * tracked this boot (trip_event_ever_received_before) -- the "genuinely
     * live" case. Meaningless/unused when is_new_event is false: the real
     * caller (safety_apply_trip_event()) only ever reads this inside its own
     * `if (is_new_event)` branch, exactly mirroring safety_link.h's
     * trip_fault_sources_valid field ("only meaningful together with
     * trip_event_ever_received", same shape one level up). */
    bool fault_sources_valid;
} safety_trip_decision_t;

/* Pure -- no I/O, no lock, deterministic in terms of its three inputs.
 * Mirrors safety_apply_trip_event()'s is_new_event/is_genuinely_live_event
 * computation exactly; that function calls this instead of reimplementing
 * it inline. */
safety_trip_decision_t safety_trip_decide_event(safety_trip_decision_input_t in);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_TRIP_DECISION_H
