// heat_enable -- the ONE place the normal firing paths ask the safety
// processor to permit heating (SAFETY_CMD_REQUEST_ENABLE, which is what
// closes K4 on the Pico), and the one place they give that permission back.
//
// Why this file exists (bug found 2026-08-29). Until now the ONLY callers of
// safety_link_request_enable() in the whole ESP firmware were
// danger_mode.c's manual diagnostics bypass and uart_bridge.c's raw
// safety_request_enable wire command. profile_executor.c and
// autotune_engine.c -- the two paths an operator actually fires or tunes a
// kiln with -- never called it at all. Both closed their own zone relay (K1
// via kiln_io_owner) and never asked SaftyFW to close K4, so no element
// current ever flowed on a normal run no matter how correct everything else
// was. Every live firing/autotune test in this repo's history measured a
// heater that was never energized, and read as "the kiln barely responds".
//
// The discipline this module enforces, copied deliberately from
// danger_mode.c (which got each of these right, one bug at a time):
//
//   1. CHECK the result. safety_link_request_enable(enable=true) returns
//      ESP_ERR_INVALID_STATE and sends NOTHING when the link is down
//      (safety_link.c). Treating that as success is how a module ends up
//      believing it asked for heat when it never did.
//   2. Track THIS side's own outstanding request. SAFETY_FLAG_ENABLED in the
//      cached status means "SaftyFW's relay_owner is ARMED", which is 1 on a
//      healthy Pico whether or not anyone ever asked for heat -- it is not an
//      answer to "did I request this".
//   3. Release unconditionally, best-effort, on EVERY exit. enable=false is
//      the fail-safe direction and safety_link.c always attempts it, link up
//      or down.
//
// And one property this module adds on top, because two independent
// claimants now exist: the request is refcounted by claimant, so an autotune
// releasing does not drop heat out from under a firing (they cannot overlap
// today -- relay_authority's heat-zone claim is exclusive -- but nothing in
// this file depends on that staying true).
//
// SAFETY ORDERING RULE, non-negotiable: releasing K4 is best-effort cleanup
// that happens AFTER the zone relay has already been de-energized. Nothing
// in here may ever gate, delay or fail a heater_output_force_off() /
// kiln_io_owner relay drop. Every caller calls force-off first and this
// second, and no caller checks this module's return value on the release
// path.
#ifndef KILNCTL_HEAT_ENABLE_H
#define KILNCTL_HEAT_ENABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Who is asking. Refcounted so the aggregate request is only released to the
// safety processor when the LAST holder lets go.
typedef enum {
    HEAT_ENABLE_CLAIMANT_PROFILE = 0,  // profile_executor.c, a firing
    HEAT_ENABLE_CLAIMANT_AUTOTUNE,     // autotune_engine.c, a step/relay test
    HEAT_ENABLE_CLAIMANT_COUNT
} heat_enable_claimant_t;

// Hands over the safety link handle. Call once at boot, next to
// danger_mode_init(). `safety_or_null` may be a link that failed to
// initialize -- safety_link_request_enable() refuses cleanly on one, same
// guarantee danger_mode_init() already relies on. Calling every other
// function in this file before this one is safe and answers "nothing
// requested".
void heat_enable_init(SafetyLinkClass *safety_or_null);

// Ask the safety processor to permit heating on behalf of `who`. Call at the
// moment a run actually commits to commanding heat -- not speculatively, and
// not once per control tick: a second acquire by an already-holding claimant
// while the request is granted sends nothing.
//
// Returns true only if a request is actually standing on the wire (either
// this call sent one and the link accepted it, or one was already granted).
// Returns false when the request could not be sent -- the link is down, or
// the safety handle is NULL. IMPORTANT: `who`'s claim is recorded EITHER
// WAY. That is deliberate:
//   - the matching heat_enable_release(who) on the caller's exit path still
//     tears the request down, rather than the failure leaving a claim the
//     caller thinks it does not hold;
//   - heat_enable_reconcile() below retries, so a link that comes back
//     mid-run gets the request it should have had.
// A false return is not a reason to refuse the run: both callers already
// refuse (autotune) or visibly report (profile_executor's
// heat_blocked/heat_block_sources, per zone) a down safety link through
// relay_authority, which is the same condition. It IS a reason to log
// loudly, which this function does.
bool heat_enable_acquire(heat_enable_claimant_t who);

// Give back `who`'s claim. Best-effort and unconditional: safe to call when
// `who` never held a claim (no-op, sends nothing), safe to call repeatedly
// (only the first one after a real claim sends anything), and safe to call
// with the link down (enable=false is always attempted -- see safety_link.h).
// Only the release of the LAST claimant sends REQUEST_ENABLE(false).
//
// Call AFTER the relays are already off. Never before, never instead.
//
// 2026-09-15: the actual SAFETY_CMD_REQUEST_ENABLE(false) wire exchange is
// NOT sent synchronously on this call's own stack any more (see
// heat_enable_service_pending_release() below) -- it is flagged here and
// drained off a task with real stack headroom. heat_enable_is_granted()/
// heat_enable_is_held() still flip synchronously, before this call returns,
// exactly as before; only the deep UART send is deferred.
void heat_enable_release(heat_enable_claimant_t who);

// Drains one release owed to the wire, if any (no-op otherwise). Called once
// per loop iteration by safety_poll_task (safety_link_poll.c) -- the same
// pattern safety_link.h's reannounce_pending/boot_clear_pending use -- so the
// deep safety_link_request_enable(false) call this used to make on
// heat_enable_release()'s OWN caller's stack (profile_executor's 4096 B task,
// implicated in four recurring stack-smash panics, see
// docs/audits/profile_executor_coredump_2026-09-15.md) instead runs on
// safety_poll_task's 8192 B stack.
//
// A pending release is attempted, not silently dropped: (2026-09-15 review
// of 1c8d7f6e, finding LOW-5) an earlier version of this comment claimed a
// pending release "is never dropped" because the flag was cleared right
// before the send; that was true only in the sense of "attempted exactly
// once" -- a send that then FAILED left nothing queued to retry. The flag is
// now cleared only once the send actually succeeds, so a failure keeps the
// release queued and it is retried the next time this function runs (on
// target: safety_poll_task's next loop iteration, plus a second, independent
// chance from heat_enable_reconcile() -- see its own comment).
//
// heat_enable_acquire()/send_enable() also call this first, and then wait
// (bounded; see he_flush_release_blocking() in heat_enable.c) for both this
// flag and the in-flight-send window to clear before sending enable=true, so
// a re-enable can never race ahead of a release that has not gone out yet --
// including the window where a release has been PICKED UP by a servicer but
// its send has not yet returned (finding HIGH-1; the earlier version of this
// fix only guarded the "still flagged" case, not the "already picked up,
// mid-send" case, which is the one the review found a real reorder through).
// Safe to call with no module lock held (and this function never acquires
// any lock but its own); safe to call from a host test in place of a real
// safety_poll_task.
void heat_enable_service_pending_release(void);

// True if `who` currently holds a claim (whether or not it was granted).
bool heat_enable_is_held(heat_enable_claimant_t who);

// True if a request is currently standing AND the safety link accepted it.
// This is what "we actually asked for K4" means; whether K4 then closed is
// SaftyFW's decision, read back from SAFETY_FLAG_RELAY in the cached status.
bool heat_enable_is_granted(void);

// True if someone holds a claim but the request has not been accepted --
// i.e. heat was asked for and the ask did not land. Surfaced so the UI/logs
// can say so rather than showing a run that looks normal and heats nothing.
bool heat_enable_retry_pending(void);

// Retries a claim whose request never landed (link down at start, link
// dropped and came back). Cheap and a no-op unless retry_pending is true, so
// it can be called from a slow periodic task -- profile_executor.c's
// watchdog task calls it every WATCHDOG_CHECK_PERIOD_MS. Deliberately does
// NOT re-assert an already-granted request: a granted request is sent once
// per run, not once per tick.
//
// 2026-09-15 (review of 1c8d7f6e, findings HIGH-1/MEDIUM-4): also drives a
// stuck release drain (heat_enable_service_pending_release()) -- a no-op
// when nothing is owed, and this module's second, independent chance to
// send an owed release if safety_poll_task (the normal drainer) is wedged.
void heat_enable_reconcile(void);

// Diagnostics/host-test counters: how many REQUEST_ENABLE(true) frames were
// accepted by the link, and how many REQUEST_ENABLE(false) frames were sent.
// A run that starts and stops once, on a healthy link, moves each by exactly
// one.
uint32_t heat_enable_enable_send_count(void);
uint32_t heat_enable_release_send_count(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HEAT_ENABLE_H
