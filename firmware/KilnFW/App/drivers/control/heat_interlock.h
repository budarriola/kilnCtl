// heat_interlock.h -- the pure, host-testable half of the OWNER's mutual
// interlock requirement, direction B: "updates are not allowed while the
// heaters are on or a profile is running" (ota_interlock.h/.c) already
// exists; this is the OTHER direction -- "heating is not allowed during
// updates" -- refuse to START a profile, refuse a direct relay/heater
// command, and refuse to START an autotune, while an update is in progress
// on EITHER processor.
//
// Same pure/impure split ota_interlock.h documents in its own header
// comment: the actual fact this needs (is an update claimed right now, and
// on which processor) lives behind ota_http.c's s_update_claim, which is
// ESP-IDF/FreeRTOS-coupled (esp_http_server.h) and not host-buildable. So
// this file takes a plain snapshot -- just "is one in progress" plus which
// processor, for a specific reason string -- and answers one pure question:
// does this snapshot permit the heat-causing action, and if not, name which
// processor is being updated. The snapshot-taking glue
// (ota_http_heat_blocked_by_update(), ota_http.c) reads
// ota_http_update_in_progress() -- the SAME single cross-processor mutex
// direction A's ota_http_check_interlocks() reads -- and calls
// heat_interlock_check() below, mirroring exactly how ota_http.c is the
// ESP-IDF glue around ota_interlock.c's pure state.
//
// Callers (profile_executor_run(), autotune_engine.c's begin_run_locked(),
// kiln_io_owner.c's relay_on_blocked()) call ota_http_heat_blocked_by_update()
// directly rather than this file, the same way ota_http_check_interlocks()
// is the one glue function every OTA entry point calls rather than each
// re-deriving an ota_interlock_snapshot_t itself -- this keeps the decision
// in exactly one shared predicate instead of an "if (updating)" scattered
// at each call site.
#ifndef KILNCTL_HEAT_INTERLOCK_H
#define KILNCTL_HEAT_INTERLOCK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which processor holds the single cross-processor update mutex
// (ota_http.h's ota_http_context_t), reduced to just the two heat-relevant
// possibilities -- OTA_HTTP_CONTEXT_ESP_ROLLBACK counts as an ESP update
// here too (a rollback reboots into different code exactly like a plain
// update does, per ota_esp_rollback_post_handler()'s own comment), so the
// glue collapses it onto HEAT_INTERLOCK_UPDATE_ESP rather than adding a
// third case this file would have no different behavior for.
typedef enum {
    HEAT_INTERLOCK_UPDATE_NONE = 0,
    HEAT_INTERLOCK_UPDATE_ESP,
    HEAT_INTERLOCK_UPDATE_PICO,
} heat_interlock_update_context_t;

typedef struct {
    bool update_in_progress;
    // Meaningful only when update_in_progress is true.
    heat_interlock_update_context_t update_context;
} heat_interlock_snapshot_t;

typedef enum {
    HEAT_INTERLOCK_OK = 0,      // no update in progress; the heat-causing action may proceed
    HEAT_INTERLOCK_REFUSED,    // reason_out names which processor is updating
} heat_interlock_result_t;

#define HEAT_INTERLOCK_REASON_MAX 96 // same precedent as OTA_INTERLOCK_REASON_MAX (ota_interlock.h)

// Checks `snap` and returns HEAT_INTERLOCK_OK or HEAT_INTERLOCK_REFUSED. On
// refusal, writes a specific, human-readable reason into reason_out (e.g.
// "an ESP firmware update is in progress -- heat cannot be commanded until
// it finishes"), always null-terminated, truncated if reason_cap is too
// small. reason_out/reason_cap may be NULL/0 if the caller only cares about
// the pass/fail result.
heat_interlock_result_t heat_interlock_check(const heat_interlock_snapshot_t *snap, char *reason_out,
                                              size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HEAT_INTERLOCK_H
