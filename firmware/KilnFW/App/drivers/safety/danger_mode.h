// danger_mode -- the diagnostics page's explicit-accept relay-override
// section, requested by the repo owner: an operator who has read and
// accepted the risk can command any of the four ESP-owned heater relays
// (kiln_io_owner.c) even while the ESP's own relay-authority gate would
// normally refuse, for bench testing relays/contactors and thermocouples by
// hand.
//
// K4 (THE SAFETY RELAY) STAYS UNDER THE SAFETY PROCESSOR'S OWN SUPERVISION
// -----------------------------------------------------------------------
// This module does NOT give the ESP a way to force K4 closed. Entering this
// section (danger_mode_request_start()) by itself requests NOTHING from the
// safety processor -- it only unlocks the ESP's own gate on its four heater
// relays (see below). Sending SAFETY_CMD_REQUEST_ENABLE is a separate,
// explicit operator action (danger_mode_set_heat_enable_request(), owner
// request 2026-08-27: no auto-enable on entry, shown in the diagnostics page
// as one more toggle in the relay grid even though it is not actually one of
// the ESP's four relays) -- exactly what a normal firing already does: the
// same wire command uart_bridge.c forwards from the PC
// (safety_link_request_enable()), and SaftyFW's own, completely unmodified
// guard evaluation decides whether to honor it -- "the Pico may refuse, and
// its own interlocks always win" (safety_link.h's REQUEST_ENABLE doc
// comment). Every guard (S1-S13) keeps running exactly as it would during a
// real firing; if any of them would trip during a firing, they trip here
// too, and K4 opens regardless of anything this module did. What
// danger_mode_active() bypasses is ONLY kiln_io_owner.c's OWN gate on the
// ESP's four heater relays (relay_authority_on_blocked()/the OTA-update
// interlock) -- see kiln_io_owner.c's relay_on_blocked(). Any outstanding
// enable request is released (enable=false) on danger_mode_stop() and on
// timeout, whether or not one was ever actually sent.
//
// It does not persist across a reboot. State lives entirely in RAM; a
// reboot for any reason (including the auto-reboot this module itself
// triggers) starts back in "off," same convention as boot_button.h.
//
// THE AUTO-EXPIRE CONTRACT
// -----------------------------------------------------------------------
// The window auto-closes DANGER_MODE_WINDOW_MS after the last accept/touch
// with no further activity. Closing on TIMEOUT does the exact same thing an
// explicit danger_mode_stop() does -- release the heat-enable request
// (SAFETY_CMD_REQUEST_ENABLE(false)) and clear window_open, i.e. exit
// gracefully as though the firing this request stood in for had simply
// ended, not reboot the board. relay_on_blocked() (kiln_io_owner.c) reads
// danger_mode_active() fresh on every call, so the ESP's own relay-authority
// gate is back in force for the very next relay command with no reboot
// needed to get there -- an unattended board sitting idle in this section
// just falls back to normal gated behavior on its own.
#ifndef DANGER_MODE_H
#define DANGER_MODE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "safety_link.h" /* SafetyLinkClass -- an anonymous-struct typedef, so it cannot be
                           * forward-declared; danger_mode_init() below needs the real type. */

#ifdef __cplusplus
extern "C" {
#endif

// 5 minutes, per the owner's explicit spec -- long enough to run a bench
// test on one relay, short enough that walking away forgets nothing on for
// long.
#define DANGER_MODE_WINDOW_MS (5u * 60u * 1000u)

// Creates the poll task that watches for expiry and reboots on timeout.
// `safety` is the board's one SafetyLinkClass instance (main.c's `&safety`)
// -- used to send/release SAFETY_CMD_REQUEST_ENABLE; may be NULL if
// safety_link_start() failed this boot (every call below degrades to a
// clean refusal/no-op through safety_link_request_enable()'s own NULL
// tolerance, never a crash). Call once from app_main(), after
// safety_link_start() and before kiln_io_owner_start() (whose relay_on_
// blocked() calls danger_mode_active()) -- see main.c's call site.
void danger_mode_init(SafetyLinkClass *safety);

// Operator has ticked the accept-risk checkbox and clicked in. Opens the
// window (or, if already open, is equivalent to danger_mode_touch()) and
// sends SAFETY_CMD_REQUEST_ENABLE(true) -- whether K4 actually closes is
// entirely SaftyFW's own call, not this function's. Refuses -- returns
// false, opens nothing, requests nothing -- while a firing is RUNNING or
// PAUSED, same "no invisible mid-firing state change" reasoning as
// boot_button.h's state_refuses_bypass(): this section can force relays
// with no ESP-side gate, and a firing actively controlling those same
// relays must never be entered into unknowingly out from under a running
// zone.
bool danger_mode_request_start(void);

// Explicit operator action, separate from entering the section: sends (or
// releases) SAFETY_CMD_REQUEST_ENABLE, same as danger_mode_request_start()
// used to do automatically before the owner asked for it to be its own
// button. Refuses -- returns false, sends nothing -- unless the window is
// currently open (same "this section's own actions only" gate every other
// command here enforces). Extends the window on success, same as a relay
// command. Whether K4 actually closes is entirely SaftyFW's own call.
bool danger_mode_set_heat_enable_request(bool enable);

// This module's OWN outstanding request -- true only after a successful
// danger_mode_set_heat_enable_request(true), cleared by an explicit false,
// danger_mode_stop(), or timeout. This is what the diagnostics page's
// "Firing mode" tile must be driven from, NOT danger_mode_get_relay_status()'s
// out_heating_enabled -- that reflects SAFETY_FLAG_ENABLED, which means
// "SaftyFW's relay_owner is ARMED / not tripped" (true on any healthy Pico
// regardless of any request ever sent), not "K4 was granted." Confusing the
// two was the 2026-08-27 bug where the tile showed ON with K4 open and every
// click silently sent the opposite of what it displayed.
bool danger_mode_get_heat_requested(void);

// Resets the window's deadline to now + DANGER_MODE_WINDOW_MS. Call on every
// accepted relay command issued through this section (the diagnostics
// page's own relay endpoint calls this after a successful write) -- this is
// the "resets every time the user changes settings in this section" the
// owner asked for; a page merely open, doing nothing, does NOT extend it. A
// no-op (returns false) if the window is not currently open.
bool danger_mode_touch(void);

// True only while the window is currently open and has not yet expired.
// Evaluated fresh against the clock on every call, same non-cached
// contract as boot_button_ota_bypass_active() -- kiln_io_owner.c's
// relay_on_blocked() calls this on every single relay command, so it must
// be correct at read time.
bool danger_mode_active(void);

// Fail-CLOSED twin of danger_mode_active() for START gates (LCD review R3): true when the window is
// open OR when that cannot be determined (internal lock timeout), so a start is refused rather than
// let through while danger mode may be open. Reads false only when the module is not initialized
// (no window can exist) or the lock was taken and the window is closed/expired. Display callers keep
// using danger_mode_active().
bool danger_mode_blocks_start(void);

// 0 when the window is not open (or has already expired); otherwise the
// real number of milliseconds left before it auto-closes-and-reboots, for
// the diagnostics page's countdown. Computed from the same on-target
// deadline every call -- a page refresh re-reads this from the board, so
// the countdown is never client-side state that could drift or reset
// itself on reload.
uint32_t danger_mode_remaining_ms(void);

// Reads SaftyFW's own current K4/heat-enable state straight off the safety
// link's cached status (SAFETY_FLAG_RELAY/SAFETY_FLAG_ENABLED, safety_link.h)
// -- true iff a link is up and each out-param was written.
//
// *out_heating_enabled is SAFETY_FLAG_ENABLED, i.e. "SaftyFW's relay_owner
// state machine is currently ARMED (not tripped)" -- true on any healthy,
// past-its-grace-period Pico REGARDLESS of whether anyone ever sent
// REQUEST_ENABLE. It is NOT "was a heat-enable request granted" -- for
// that, see danger_mode_get_heat_requested() above (this module's own
// outstanding request) alongside *out_relay_energized here (whether K4
// actually closed). Mixing this flag up with "granted" was the 2026-08-27
// bug in the diagnostics page's "Firing mode" tile.
bool danger_mode_get_relay_status(bool *out_relay_energized, bool *out_heating_enabled);

// Operator-requested early exit: closes the window immediately, sends
// SAFETY_CMD_REQUEST_ENABLE(false), logs `source`, no reboot -- the same
// graceful release the timeout path (danger_mode.c's danger_mode_task) now
// performs on its own once the window expires. `source` distinguishes the
// two in the log ("operator" vs "timeout").
void danger_mode_stop(const char *source);

#ifdef __cplusplus
}
#endif

#endif // DANGER_MODE_H
