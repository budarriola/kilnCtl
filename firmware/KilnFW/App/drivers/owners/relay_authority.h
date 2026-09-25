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

/* The latched per-zone block on its own, without the global fault-source
 * check relay_authority_zone_blocked() folds in. Needed because the latch
 * survives the run that set it. Two things clear it, and only two:
 * clear_this_runs_faults() (profile_executor_halt()'s own cleanup, which
 * only clears a zone that was marked ACTIVE in the run being halted --
 * skipping a zone that faulted in an earlier run but isn't part of the run
 * being halted now), and clear_stale_zone_latches_for_new_run() (called from
 * profile_executor_run() when a NEW run starts, releasing the latch for
 * every zone THAT run activates). Before the latter existed (fixed
 * 2026-09-25, HP-02), a zone whose guard tripped in a run that reached
 * DONE/FAULTED and was never explicitly halted/dismissed -- profile_
 * executor_run()'s own state guard only refuses starting over RUNNING/
 * PAUSED/FAULTED, not DONE -- stayed latched blocked across every
 * subsequent run, including ones that did reactivate it, computing duty
 * normally while every relay command was silently refused here: an
 * autotune run on it once settled for three minutes, drove nothing, and
 * reported "response too small to fit (trace flat or noise-dominated)",
 * blaming the kiln for a latch left behind by an earlier trip; a 3-zone
 * firing (HP-02) left one zone at +1.9C while its siblings rose normally.
 * Reboot was the only other way out before this fix. */
bool relay_authority_zone_latched_blocked(uint8_t zone_index);

/* Bit N (0-based zone index) set for each zone currently latched-blocked --
 * so the block can be SHOWN, which is what it never was. */
uint8_t relay_authority_latched_blocked_mask(void);

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
    /* RETIRED 2026-08-27: rules_task.c/rules_http.c (the "Relays & Rules"
     * engine) were deleted -- relay/IO control moved into firing profiles as
     * segments (profile_executor.c's io_seg_* machinery), per the owner's
     * "instead of the relays and rules section I want them to be part of the
     * profile" request. Nothing claims this tag any more. The enumerator is
     * kept in place, unrenumbered, rather than removed: a stale
     * RELAY_OWNER_RULE value already written to NVS or sent over a wire
     * before this change must not be silently reinterpreted as a different
     * owner (e.g. AUTOTUNE below) after a firmware update. Safe to actually
     * delete only once nothing on a live board can still be holding an old
     * value -- not this pass. */
    RELAY_OWNER_RULE,
    /* TODO.md 6A.6 (2026-08-21): claimed by autotune_engine.c for the
     * duration of a running step-test/relay-test (begin_run_locked() through
     * force_relays_off()), same claim/release shape as RELAY_OWNER_PROFILE.
     * Without this, a manual SET_RELAY/SET_RELAY_MASK arriving mid-test was
     * refused by nothing -- relay_authority_zone_blocked() only stops a
     * *safety fault*, not an unrelated manual command racing the test. */
    RELAY_OWNER_AUTOTUNE,
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
 * profile (or an in-progress autotune test), or both. */
bool relay_authority_manual_blocked_by_owner(uint8_t relay_index);

/* ---- The single shared heat claim (opus reviews, 2026-08-28) ----
 *
 * Three subsystems can drive the mains-contactor relays: profile_executor.c,
 * autotune_engine.c, and zones_http.c's per-zone current sweep. Each already
 * REFUSES to start while it observes one of the others is active
 * (zones_current_sweep_is_active(), autotune_engine_is_active_on_zone(),
 * profile_executor_zone_is_active()) -- but every one of those is a plain
 * read of another module's state, with no lock spanning the read and the
 * reader's own commit. check-then-start is atomic on none of them, and they
 * run on genuinely different tasks (LVGL UI, the httpd worker, the UART
 * bridge task, and their own control tasks), so a start on one can land in
 * the window between another's check and its commit: bounded (the losing
 * side's own watchdog/force-off eventually wins) but not correct-by-
 * construction -- both sets of relays can be live at once for the length of
 * that window, and the loser's terminal all-relays-off can drop the
 * winner's relays for a control tick.
 *
 * This is the fix: one mutex-protected (portMUX, see relay_authority.c)
 * claim that all three test-and-set under, so "is someone else already
 * heating" and "I am now the one heating" happen as a single atomic step
 * instead of two reads and a write racing across tasks. It is layered IN
 * FRONT of everything else in this file -- kiln_io_owner.c's per-command
 * ERR_OWNED check, relay_authority_claim_mask()'s per-relay ownership tags,
 * relay_authority_on_blocked()'s safety-fault gate -- none of which this
 * replaces: those still arbitrate who owns which RELAY once a subsystem is
 * legitimately running. This claim only answers "may a subsystem legitimately
 * START running heat at all right now," at the coarser whole-engine-instance
 * granularity the three subsystems' mutual refusals already operate at.
 *
 * Deliberately NOT a single 3-way mutex: profile_executor.c and
 * autotune_engine.c are legitimately concurrent with each other (a firing on
 * one zone alongside an autotune run on a different zone,
 * profile_executor_zone_is_active()'s per-zone arbitration already allows
 * and this claim must not break) -- only the sweep is mutually exclusive
 * against BOTH of them, since it is a whole-board commissioning operation
 * that assumes nothing else is driving any relay. So there are two claim
 * families sharing one lock: zone-heat claims (PROFILE, AUTOTUNE -- either or
 * both may hold theirs at once) and the sweep's own exclusive claim (refused
 * whenever either zone-heat claim is held, and blocks both from being taken
 * while it holds its own).
 *
 * Each subsystem calls _begin() at the point it has already decided, under
 * its OWN lock (s_exec.lock / s_at.lock), that it is not already running --
 * so a _begin() call is always a genuine first acquisition, never a
 * reentrant one, and the matching _end() is safe to call unconditionally
 * from that subsystem's single centralized "this run is over" cleanup
 * (release_profile_relay_claim() / force_relays_off() / the sweep task's own
 * completion) without a separate "did I actually hold it" bookkeeping bit. */
typedef enum {
    RELAY_HEAT_ZONE_CLAIM_PROFILE = 0,
    RELAY_HEAT_ZONE_CLAIM_AUTOTUNE,
} relay_heat_zone_claimant_t;

/* Claims zone-heat authority for `who`. Fails (returns false) only while the
 * sweep's exclusive claim is held; profile and autotune never refuse each
 * other here (see this header's doc comment above -- that arbitration is
 * per-zone, and is the job of relay_authority_zone_claim_begin() below, not
 * this function). Call only once state has already been
 * confirmed (under the caller's own lock) to be a genuine start, not a
 * reentrant call on an already-running instance -- see this header's doc
 * comment for why that makes the matching _end() safe to call
 * unconditionally. */
bool relay_authority_heat_zone_claim_begin(relay_heat_zone_claimant_t who);

/* Releases `who`'s zone-heat claim. Safe to call even if `who` never held
 * it (a no-op in that case) -- callers are expected to call this
 * unconditionally from their single "run is over" cleanup path. */
void relay_authority_heat_zone_claim_end(relay_heat_zone_claimant_t who);

/* Per-zone arbitration between a profile and an autotune session, closing a
 * race the review of 933a7eec found (docs/audits/profile_executor_panic_2026-
 * 09-24.md follow-up): profile_executor_zone_is_active()/autotune_engine_
 * is_active_on_zone() are each a plain, non-atomic peek made BEFORE the
 * caller's OWN module lock (s_exec.lock / s_at.lock) is taken -- two starts on
 * the same zone, each passing its own peek before the other has committed,
 * could both proceed. This pair is the atomic close, keyed per zone bit
 * (bit i = zone i) rather than sharing the whole-board RELAY_HEAT_ZONE_CLAIM_*
 * pair above -- deliberately NOT the same arbiter: profile and autotune are
 * legitimately concurrent with each other on DIFFERENT zones (that pair only
 * ever arbitrates the whole-board current sweep, see this header's doc
 * comment above), so folding this into it would wrongly serialize every
 * profile/autotune pair board-wide instead of only a genuinely double-owned
 * zone. Backed by the same leaf portMUX spinlock as the pair above -- no
 * blocking call inside either function, so this adds no new lock ordering.
 * Call only once the caller's own lock has confirmed this is a genuine start
 * (never a reentrant call on an already-running instance), same convention as
 * relay_authority_heat_zone_claim_begin() -- so the matching _end() below is
 * safe to call unconditionally from that subsystem's single "run is over"
 * cleanup path.
 *
 * relay_authority_zone_claim_begin() fails (returns false) only when at least
 * one bit of `zone_mask` is already claimed by the OTHER claimant; on failure,
 * if `conflict_mask_out` is non-NULL, it is filled with exactly those
 * conflicting bits (0 on success) so the caller can name the offending zone
 * in its own refusal message without re-reading shared state outside this
 * lock. */
bool relay_authority_zone_claim_begin(relay_heat_zone_claimant_t who, uint8_t zone_mask,
                                       uint8_t *conflict_mask_out);
void relay_authority_zone_claim_end(relay_heat_zone_claimant_t who, uint8_t zone_mask);

typedef enum {
    RELAY_HEAT_SWEEP_CLAIM_OK = 0,
    RELAY_HEAT_SWEEP_CLAIM_REFUSE_PROFILE, /* a profile currently holds a zone-heat claim */
    RELAY_HEAT_SWEEP_CLAIM_REFUSE_AUTOTUNE, /* autotune currently holds a zone-heat claim */
} relay_heat_sweep_claim_result_t;

/* Claims the sweep's exclusive claim. Fails if either zone-heat claim above
 * is currently held, naming which one so the caller can report the SAME
 * specific refusal reason (ZONE_SWEEP_REFUSE_PROFILE_RUNNING /
 * ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING) its own earlier, non-atomic,
 * informational check already produces for the common (non-race) case --
 * this call is the last-moment atomic re-check that actually closes the
 * race, not a replacement for that earlier check's operator-facing
 * message. */
relay_heat_sweep_claim_result_t relay_authority_heat_sweep_claim_begin(void);

/* Releases the sweep's exclusive claim. Safe to call even if the sweep
 * never held it. */
void relay_authority_heat_sweep_claim_end(void);

/* True/false for whether profile_executor / autotune_engine currently hold
 * the shared heat claim (RUNNING or PAUSED for profile -- claim_end() is
 * only called from a terminal transition, see profile_executor_status.c's
 * halt(), never from pause()). A leaf read under s_heat_claim_mux only --
 * safe to call from any task, including one that must never block on
 * profile_executor.c's or autotune_engine.c's own locks (kiln_io_owner.c's
 * owner_task, in particular -- see relay_authority.c's doc comment above
 * this function). Either out-pointer may be NULL. */
void relay_authority_heat_run_active(bool *profile, bool *autotune);

#ifdef __cplusplus
}
#endif

#endif // RELAY_AUTHORITY_H
