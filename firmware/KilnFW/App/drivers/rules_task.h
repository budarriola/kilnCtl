// rules_task -- the impure half of the rule evaluator TODO.md section 6 (via
// rules_page.html's former disclaimer) said did not exist: a FreeRTOS task
// that periodically samples live inputs (zone temperatures, profile
// running/elapsed state, other relays' commanded state, the safety link,
// and the OTA heat interlock), runs rules_eval.c's pure logic against
// whatever rules_http.c currently has saved, and drives the result through
// kiln_io_owner -- never by writing the SX1509 expander directly.
//
// ---- Ownership: how this cannot fight manual control or a running profile ----
//
// relay_authority.h already reserves a RELAY_OWNER_RULE tag ("a rule
// evaluator, once one exists, would claim RULE the same way" as
// profile_executor.c claims RELAY_OWNER_PROFILE) -- this file is that
// evaluator. Each tick, for every relay whose rules_cfg_t.rule_driven flag
// is true, this task claims RELAY_OWNER_RULE via
// relay_authority_claim_mask(); for every relay whose flag has gone back to
// false, it releases its own prior claim. relay_authority_manual_blocked_by_
// owner() (already checked by kiln_io_owner.c's MANUAL producers, used by
// the dashboard's manual override and the UART bridge) then refuses a
// manual command against a RULE-owned relay exactly the way it already
// refuses one against a PROFILE-owned relay -- no new arbitration code was
// needed for that half.
//
// The other direction -- a relay a running profile already owns (zone
// relay_mask) also marked rule_driven -- is refused at claim time: this
// task only claims a relay currently RELAY_OWNER_NONE or already
// RELAY_OWNER_RULE (i.e. its own prior claim); if profile_executor.c got
// there first (RELAY_OWNER_PROFILE) or autotune_engine.c did
// (RELAY_OWNER_AUTOTUNE), the claim is refused, logged once, and this task
// does not drive that relay at all until the other owner releases it.
// Operators should keep a relay's zones_cfg_t relay_mask and its
// rule_driven flag disjoint -- nothing currently validates that they are,
// so a relay assigned to both loses to whichever of PROFILE/RULE claims it
// first.
//
// Net precedence, relay by relay: PROFILE/AUTOTUNE ownership (if claimed
// first) > RULE ownership > MANUAL. Two callers in the same MANUAL family
// (the dashboard and, before this file existed, nothing else) still have no
// arbitration between each other -- unaffected by this change.
//
// ---- Fail-safe: this cannot turn a relay on and leave it that way ----
//
// Every tick this task itself samples relay_authority_on_blocked() (global
// safety-fault/link-down check) and ota_http_heat_blocked_by_update() (the
// OTA mutual interlock) and folds both into rules_eval_decide() BEFORE
// calling kiln_io_owner_command_set_relay_mask_authorized() -- the
// AUTHORIZED producer skips kiln_io_owner's own gate specifically because
// the caller is expected to have already applied it (profile_executor.c's
// apply_relay() does the identical thing via relay_authority_zone_blocked(),
// which itself calls relay_authority_on_blocked()). A relay this task wants
// OFF is never gated by either check (turning off is always allowed, per
// relay_authority.h's own rule) -- so a safety fault or an in-progress
// update can only ever remove heat from a rule-driven relay, never add it.
//
// Failure modes this task is built to survive without leaving a relay
// stuck on:
//   - Safety link down/unhealthy at task start, or goes down mid-run: every
//     ON decision is refused (relay_authority_on_blocked() with safety==NULL
//     or a fault source asserted always blocks); the relay is force-OFF'd
//     the same tick it stops qualifying.
//   - An OTA update starts mid-run: ota_http_heat_blocked_by_update() blocks
//     every ON decision from the moment the update is accepted.
//   - kiln_io_owner's queue/task is down (never started, or wedged): every
//     kiln_io_owner_command_*() call fails closed on its own (documented in
//     kiln_io_owner.h) -- this task does not need to detect that itself.
//   - This task's own tick stalls or the task dies: it holds no ownership
//     claim that defaults to ON -- relay_authority_manual_blocked_by_owner()
//     merely refuses OTHERS from writing a RULE-owned relay; it does not
//     drive the relay itself. A wedged rules_task simply stops refreshing
//     the commanded state (last write stands, whatever it was) rather than
//     forcing anything on. profile_executor.c's own guard-9 watchdog
//     pattern (a second, independent task) was considered and rejected here
//     specifically because there is no continuously-running "run" for a
//     watchdog to detect the absence of ticking on the way profile_executor
//     has one -- a future improvement could add a stale-tick timeout that
//     force-releases RULE ownership (falling back to manual control) if
//     this task hasn't ticked in N seconds; not built here (see rules_task.c
///    top comment for the exact gap this leaves).
//   - rules_http.c's config is mid-write when this task reads it: no lock
//     (documented in rules_http.h) -- worst case is one tick evaluated
//     against a torn config, self-corrects next tick, never a crash.
#ifndef RULES_TASK_H
#define RULES_TASK_H

#include <stdbool.h>

#include "esp_err.h"
#include "safety_link.h"

#include "rules_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the rule-evaluator task. `safety` may be NULL (mirrors
 * kiln_io_owner_start()'s own convention) -- every ON decision fails closed
 * in that case, same as every other relay-on path in this codebase.
 * Must be called after kiln_io_owner_start() (this task is only ever a
 * PRODUCER into that queue, never a direct SX1509 writer) and after
 * rules_http_start() (this task reads rules_http_get_cfg() every tick). */
esp_err_t rules_task_start(SafetyLinkClass *safety);

/* Live status for the Relay & Rules page -- one entry per relay, 0-based
 * (relay N is index N-1). rules_http.c's GET /api/rules/status serves this
 * as JSON. */
typedef struct {
    bool rule_driven;     /* rules_cfg_t.relays[i].rule_driven as of the last tick */
    bool rule_wants_on;   /* rules_eval_relay_wants_on()'s answer, BEFORE the safety/
                           * interlock gate -- "would fire if nothing were blocking it" */
    bool commanded_on;    /* the actual state this task commanded this relay to,
                           * AFTER the gate -- what the relay is really doing */
    bool owned_by_rules;  /* true if this task currently holds RELAY_OWNER_RULE for
                           * this relay (false if some other owner got there first --
                           * see this header's top comment) */
} rules_task_relay_status_t;

typedef struct {
    bool safety_link_ok;     /* relay_authority_on_blocked() was NOT blocking, as of
                              * the last tick */
    bool heat_interlock_ok;  /* ota_http_heat_blocked_by_update() was NOT blocking,
                              * as of the last tick */
    rules_task_relay_status_t relays[RULES_EVAL_RELAY_COUNT];
} rules_task_status_t;

/* Always succeeds even if rules_task_start() was never called or failed --
 * *out is zero-initialized in that case (every field reads as "not driven,
 * not firing, not owned, gates clear"), same fail-safe-reads-as-inert
 * convention as the rest of this codebase's status getters. */
void rules_task_get_status(rules_task_status_t *out);

#ifdef __cplusplus
}
#endif

#endif // RULES_TASK_H
