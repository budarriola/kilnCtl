// ota_state.h -- the one OTA state query non-http layers are allowed to
// depend on: "is an update in progress on either processor, and if so
// refuse this heat-causing action". Split out of ota_http.h (an http-layer
// header) per docs/HW_ABSTRACTION_PLAN.md's "drivers/ layering" item 2 --
// kiln_io_owner.c, profile_executor.c and profile_executor_run.c only ever
// needed this one accessor, not the whole HTTP-handler surface. ota_http.h
// includes this header so existing http-layer callers are unaffected; the
// implementation stays in ota_http.c, which already owns the update-mutex
// state this reads.
#ifndef OTA_STATE_H
#define OTA_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Heat interlock, the OTHER direction (TODO.md 9.4/ROADMAP.md M8's
// mutual interlock: "updates are not allowed while the heaters are on or a
// profile is running" AND "heating is not allowed during updates") --------
//
// Reads the single cross-processor update mutex (ota_http.h's
// ota_http_update_in_progress()) into a heat_interlock_snapshot_t and calls
// heat_interlock_check() (heat_interlock.h), the pure, host-tested half.
// Every heat-causing entry point (profile_executor_run(), autotune_engine.c's
// begin_run_locked(), kiln_io_owner.c's relay_on_blocked()) calls THIS
// function rather than re-deriving a snapshot itself, so the decision lives
// in exactly one shared predicate, mirroring ota_http_check_interlocks()'s
// own role for the opposite direction.
//
// Returns true (and fills reason_out/reason_cap, same NULL/0-tolerant
// contract as ota_http_check_interlocks()) if a heat-causing action should
// be refused because an update is in progress on either processor; false
// if it may proceed.
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // OTA_STATE_H
