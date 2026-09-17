// pico_auto_update_state -- the single live verdict readiness_gate.c and
// readiness_http.c both read for the "pico_update" readiness item
// (docs/PICO_AUTO_UPDATE_PLAN.md), so the ENFORCEMENT (readiness_gate.h's
// firing block) and the DISPLAY (/api/readiness) can never drift apart --
// same "one shared predicate" reasoning as safety_ceiling_sync_is_diverged().
//
// STUB, deliberately: docs/PICO_AUTO_UPDATE_PLAN.md's boot-time glue --
// the code that actually queries the Pico's running version, calls
// pico_auto_update_decide() (pico_auto_update.h) and consults the
// persisted attempt budget (pico_update_attempts.h) -- is a separate,
// later commit (step 3 of that plan). Until that lands, this always
// answers "not blocking": there is no live verdict yet to report, and
// "not yet evaluated" must read as OK, never as a permanent false block on
// every board's very first boot after this readiness item ships. This
// keeps the readiness-gate wiring itself independently buildable,
// testable and revertable ahead of the code that will feed it real data.
#ifndef PICO_AUTO_UPDATE_STATE_H
#define PICO_AUTO_UPDATE_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True only once the boot-time auto-update path (not yet wired) has
 * exhausted its attempt budget on one of pico_auto_update.h's unrecoverable
 * ABANDONED_* causes. Always false today -- see this file's own top
 * comment. Safe to call from any context; reads no hardware itself. */
bool pico_auto_update_state_is_blocking(void);

#ifdef __cplusplus
}
#endif

#endif // PICO_AUTO_UPDATE_STATE_H
