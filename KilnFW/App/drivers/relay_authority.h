// relay_authority -- the one chokepoint every caller that can turn a relay ON
// must go through, per docs/SAFETY_MODEL.md and TODO.md section 0's "who's
// allowed to turn this relay on right now" requirement.
//
// This exists so that when a second and third caller show up alongside the
// UART bridge (the web UI's manual override, and the profile-execution
// engine that will drive relays from a firing schedule -- TODO.md sections 2
// and 6), none of them can quietly grow their own copy of the gate logic and
// drift from it. There is exactly one implementation of "is it safe to
// energize a relay right now," and every caller asks it the same question.
//
// This module does not itself track relay state, own a SafetyLinkClass, or
// call kiln_io -- it only answers the ON/blocked question from a
// SafetyLinkClass the caller already has. Turning a relay OFF is never
// gated: the safe direction must always be reachable, including to recover
// from the very fault that's blocking ON.
#ifndef RELAY_AUTHORITY_H
#define RELAY_AUTHORITY_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether a command that would turn a relay ON should be refused right now.
 *
 * safety being NULL (e.g. safety_link_start failed at boot, so the isolated
 * fault line and its bookkeeping do not exist) blocks every relay-ON
 * command and reports SAFETY_FAULT_SRC_APP: an absent safety subsystem
 * cannot prove the kiln is safe to energize, so the conservative reading --
 * refuse -- is the only one that doesn't quietly trade the safety model away
 * because one driver didn't come up.
 *
 * out_sources, if non-NULL, is set to the fault-source bitmask backing the
 * decision (for logging/reporting) regardless of whether the result is
 * blocked or not. */
bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources);

/* Per-zone extension (TODO.md 6A.6): a thermal_guard trip on zone A should
 * be able to stop *that zone's* relays without also stopping a healthy
 * zone B's firing. Layered strictly on top of relay_authority_on_blocked():
 * the global check runs first and its answer is final if it blocks (a
 * link-loss/PC-fault/manual fault stops everything, same as always) --
 * only when the global check does NOT block does the per-zone mask get
 * consulted. This means the UART bridge and the dashboard's manual
 * override, which never call this function, see no behavior change at all
 * from its existence.
 *
 * The mask is owned and published by profile_executor.c (the only caller
 * that has a "which zone is this relay command for" concept); this module
 * just stores and reports it, same "mechanism here, policy at the caller"
 * split as the global check's SafetyLinkClass. zone_index 0-based, must be
 * < MAX31856_CHANNEL_COUNT or the mask write is ignored. */
bool relay_authority_zone_blocked(SafetyLinkClass *safety, uint8_t zone_index, uint32_t *out_sources);

/* Sets/clears whether zone_index's relays are blocked by
 * relay_authority_zone_blocked() -- called by profile_executor.c when a
 * thermal_guard trips or clears on that zone. Not persisted; resets to
 * "not blocked" on reboot like every other in-RAM safety-state bit in this
 * codebase (the guard itself re-evaluates from a clean state after a
 * reboot, same as everything else here). */
void relay_authority_set_zone_blocked(uint8_t zone_index, bool blocked);

/* Per-relay ownership (TODO.md section 0's "does the web UI's manual relay
 * override fight a running profile over the same relay" decision, closing
 * the open gap TODO.md's "Manual relay control is not blocked during a
 * firing" bullet flagged). NONE and MANUAL both leave a relay reachable by
 * a manual command; PROFILE and RULE do not. This module only stores the
 * tag and answers "is a manual command against this relay refused" -- the
 * policy of *when* to claim/release (profile start/pause/resume/halt) lives
 * in the caller (profile_executor.c today; a rule evaluator, once one
 * exists, would claim RULE the same way). */
typedef enum {
    RELAY_OWNER_NONE = 0,
    RELAY_OWNER_MANUAL,
    RELAY_OWNER_PROFILE,
    RELAY_OWNER_RULE,
} relay_owner_t;

/* relay_index is 1-based (matches kiln_io_set_relay's convention), 1..4. */
relay_owner_t relay_authority_get_owner(uint8_t relay_index);

/* mask follows the project's existing "bit N-1 = relay N" convention
 * (zone_cfg_t.relay_mask, kiln_io_set_relay_mask). Claiming/releasing a
 * relay not in the mask is a no-op for that relay. */
void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner);

/* Shorthand for relay_authority_claim_mask(relay_mask, RELAY_OWNER_NONE). */
void relay_authority_release_mask(uint8_t relay_mask);

/* True when relay_index is owned by something other than NONE/MANUAL, i.e.
 * a manual command (dashboard /api/relay, UART SET_RELAY/SET_RELAY_MASK/
 * SX_WRITE_REG) against it must be refused. Independent of, and checked in
 * addition to, relay_authority_on_blocked()/relay_authority_zone_blocked()
 * -- a relay can be refused for a safety fault, for being owned by a
 * profile, or both. */
bool relay_authority_manual_blocked_by_owner(uint8_t relay_index);

#ifdef __cplusplus
}
#endif

#endif // RELAY_AUTHORITY_H
