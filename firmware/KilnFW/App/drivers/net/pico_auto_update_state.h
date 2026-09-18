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

#ifdef __cplusplus
}
#endif

#endif // PICO_AUTO_UPDATE_STATE_H
