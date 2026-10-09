// pico_auto_update_state -- the single live verdict readiness_gate.c and
// readiness_http.c both read for the "pico_update" readiness item
// (docs/PICO_AUTO_UPDATE_PLAN.md), so the ENFORCEMENT (readiness_gate.h's
// firing block) and the DISPLAY (/api/readiness) can never drift apart --
// same "one shared predicate" reasoning as safety_ceiling_sync_is_diverged().
//
// NO LONGER A STUB. The boot-time evaluator (net/pico_auto_update_boot.h)
// now writes this exactly once per boot, with the outcome of
// pico_auto_update_decide(). Until it does -- and on any boot where the
// feature is inert, deferred, or undecidable -- the answer stays false:
// "not yet evaluated" and "nothing to decide" must both read as OK, never as
// a false block on a board that has done nothing wrong.
//
// CONCURRENCY. One writer (the boot task, once), many readers (the readiness
// gate on a firing start, the HTTP status route). The verdict is a plain
// bool and the reason a fixed buffer written strictly before the bool is
// set, so a reader either sees the previous verdict with its previous reason
// or the new verdict with its new reason. No lock is taken here on purpose:
// readers include paths that must not block, and this module holds no other
// state worth a mutex.
#ifndef PICO_AUTO_UPDATE_STATE_H
#define PICO_AUTO_UPDATE_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PICO_AUTO_UPDATE_STATE_REASON_MAX 96

/* True once the boot-time auto-update path has settled on one of
 * pico_auto_update.h's unrecoverable ABANDONED_* causes -- the outcomes plan
 * sec 4/10.3 says must refuse the next firing start. Safe to call from any
 * context; reads no hardware and takes no lock. */
bool pico_auto_update_state_is_blocking(void);

/* The reason behind a true verdict, for logging and /api/readiness. Never
 * NULL; an empty string when nothing is blocking. Points at static storage
 * that only the boot evaluator writes. */
const char *pico_auto_update_state_reason(void);

/* Publishes this boot's verdict. Called only by pico_auto_update_boot.c,
 * which reaches exactly one of these calls per boot. `reason` may be NULL
 * (required to be, in spirit, when `blocking` is false). */
void pico_auto_update_state_set_blocking(bool blocking, const char *reason);

/* Review finding F2, owner decision (option c), 2026-09-20: a SEPARATE,
 * non-blocking verdict for PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT. Unlike
 * the blocking verdict above, this one never refuses a firing start --
 * readiness_gate.h reads only pico_auto_update_state_is_blocking(), never
 * this -- it exists purely so /readiness (readiness_http.c) can show the
 * operator "the last few auto-update attempts failed and the budget is
 * spent" as a WARNING rather than silently saying nothing, matching the
 * "advisory, not gating" treatment several other readiness items already
 * get. Same single-writer-per-boot convention as set_blocking(). */
bool pico_auto_update_state_is_warning(void);

/* The reason behind a true pico_auto_update_state_is_warning() -- naming the
 * attempt count and the commit that could not be matched. Never NULL; an
 * empty string when there is no warning. Points at static storage that only
 * the boot evaluator writes. */
const char *pico_auto_update_state_warning_reason(void);

/* Publishes this boot's non-blocking warning verdict. `reason` may be NULL
 * (or empty) to mean "no warning". Called by pico_auto_update_boot.c only
 * for the ABANDONED_BUDGET_SPENT outcome; every other outcome leaves this at
 * its zero-initialized default (no warning), which is correct since this
 * state is not persisted across boots and each boot's task decides its own
 * verdict exactly once. */
void pico_auto_update_state_set_warning(const char *reason);

/* Bug found 2026-09-21 (bench triage): readiness_http.c's "pico_update" item
 * used to render the hardcoded sentence "the safety processor's firmware
 * version matches" for EVERY non-blocking, non-warning outcome -- which
 * includes LINK_DOWN (no FW_VERSION arrived yet), DEFER_FIRING (a firing is
 * in the way), a NEEDED attempt that stood down locally, and the boot task
 * never running at all this boot -- none of which ran a comparison that
 * matched anything. Only a genuine PICO_AUTO_UPDATE_MATCH earns that
 * sentence.
 *
 * pico_auto_update_state_set_last_decision() records the human-readable
 * `why` string pico_auto_update_decide() already produces (or a boot-task
 * -specific "inert" reason for the no-image-and-no-manifest case, which
 * never reaches decide() at all), and pico_auto_update_state_decision_is_
 * match() says whether that decision was actually MATCH. readiness_http.c
 * uses these instead of assuming a match by default. Same single-writer-
 * per-boot convention as the blocking/warning flags above; the default
 * (boot task has not run yet, or this boot never reached a decision) is an
 * empty string / false, which readiness_http.c renders as "not yet
 * evaluated this boot" rather than a false "matches". */
void pico_auto_update_state_set_last_decision(const char *reason, bool is_match);

/* The most recent decision string set above. Never NULL; empty when nothing
 * has been decided yet this boot. */
const char *pico_auto_update_state_last_decision(void);

/* True only when the most recent decision recorded via
 * pico_auto_update_state_set_last_decision() was a genuine identity match. */
bool pico_auto_update_state_decision_is_match(void);

/* TEST ONLY. Resets every static in this module back to its zero-init
 * default (not blocking, not warning, no decision recorded). Never called
 * from firmware -- this module's real "reset" is a fresh boot, which
 * zero-inits its statics for free. Exists solely so host tests can observe
 * the true default (test_default_decision_is_not_a_match()) without relying
 * on function ordering within a single test process to never have mutated
 * it first. */
void pico_auto_update_state_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif // PICO_AUTO_UPDATE_STATE_H
