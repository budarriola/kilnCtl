// link_watchdog_decide -- the pure decision core of uart_bridge.c's PC-link
// watchdog (link_watchdog_task, see uart_bridge.h's "PC link watchdog" doc
// comment for the full policy), pulled out so it can be host-tested without
// FreeRTOS/kiln_io/lvgl/panel_spi/etc, which the rest of uart_bridge.c needs
// and which the host test harness has no stubs for. Same pattern as
// profile_executor.h's profile_executor_wd_decide() / test_safety_watchdog.c.
//
// TODO.md ("The PC-link watchdog drops all relays every 5 s of host
// silence"): the watchdog's job is narrower than its name suggests. It is
// authority ONLY over relays nobody on the board has claimed -- a relay
// under a PROFILE or AUTOTUNE owner has its own watchdogs (profile_executor's
// guard 9, the safety-link silence abort) and the serial link's health says
// nothing about it. An idle PC observing a running firing must never cause a
// relay drop; a PC that took manual control (RELAY_OWNER_NONE/MANUAL) and
// then went silent must have that control expire. This split is enforced by
// relay_authority_manual_blocked_by_owner() below, which link_watchdog_decide_
// unowned_mask() consults per relay -- see relay_authority.h's relay_owner_t.
#ifndef LINK_WATCHDOG_DECIDE_H
#define LINK_WATCHDOG_DECIDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Deliberately not KILN_IO_RELAY_COUNT -- this module stays free of a
// kiln_io.h/SX1509.h dependency (same minimal-include reasoning as
// relay_authority.c's own RELAY_AUTHORITY_MAX_RELAYS); 4 is that constant's
// actual value today.
#define LINK_WATCHDOG_MAX_RELAYS 4u

// True while the link counts as up: `ever_seen` is false before the host has
// ever spoken (link counts as lost, matching uart_bridge.h's "before the
// host has ever spoken the link counts as lost" doc); otherwise up iff less
// than `timeout_ticks` have elapsed since `last_activity_ticks`. Unsigned
// subtraction, so this is correct across a tick-counter wrap without a
// special case -- same reasoning link_watchdog_task's inline version had
// before this extraction, now shared instead of duplicated.
static inline bool link_watchdog_decide_link_up(uint32_t now_ticks, uint32_t last_activity_ticks,
                                                 bool ever_seen, uint32_t timeout_ticks)
{
    return ever_seen && ((uint32_t)(now_ticks - last_activity_ticks) < timeout_ticks);
}

// Which relays (bit N-1 = relay N, 1-based, matches kiln_io_set_relay_mask's
// convention) the link watchdog has authority to force off right now: every
// relay NOT owned by an on-board subsystem (PROFILE/RULE/AUTOTUNE, per
// relay_authority_manual_blocked_by_owner()), unless `danger_mode_is_active`
// is true, in which case the operator is deliberately holding relays closed
// from the browser with no serial traffic expected and the mask is empty.
//
// Calls the REAL relay_authority_manual_blocked_by_owner() (declared in
// relay_authority.h, defined in relay_authority.c) rather than taking a
// pre-computed owner array, so a host test exercising this function is
// exercising the same ownership-lookup code path the board runs, not a
// hand-set mirror of it -- see relay_authority.c's claim/release API.
uint8_t link_watchdog_decide_unowned_mask(bool danger_mode_is_active);

#ifdef __cplusplus
}
#endif

#endif // LINK_WATCHDOG_DECIDE_H
