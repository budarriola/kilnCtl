// stack_margin_poller.h -- the SINGLE low-priority round-robin sampler
// behind SAFETY_CMD_GET_STACK_MARGIN (0x2B) / SAFETY_CMD_STACK_MARGIN
// (0x2C), KILNLINK_PROTOCOL_VERSION 13.
//
// docs/audits/saftyfw_live_stack_reporting_design_2026-09-11.md sec 1/3 is
// the reason this exists as a poller rather than measuring synchronously
// inside link_task's request handler: a per-checkin
// uxTaskGetStackHighWaterMark() call from EVERY task, on EVERY check-in, was
// added 2026-08-23, answered its question, and was REMOVED the same day
// because it distorted the exact timing this firmware exists to measure --
// watchdog reset cadence dropped from ~9s to ~1.1s
// (watchdog_task.c:290's own comment). This module is built to avoid
// repeating that shape: ONE call to uxTaskGetStackHighWaterMark(), for ONE
// target task, per invocation of stack_margin_poller_tick() -- never all
// nine in one go, and never called from inside watchdog_task_checkin() or
// any path the watchdog's own deadline logic depends on.
//
// stack_margin_poller_tick() is meant to be called from a single existing
// low-priority task's own normal loop iteration -- log_task_fn(), the
// lowest-priority task in this build with the most static headroom
// measured (docs/audits/saftyfw_bare_minimum_stack_measurement_2026-09-11.md)
// -- at that task's own ~500ms cadence, NOT from a dedicated new task. A
// full round-robin cycle over all 9 tasks therefore takes ~4.5s; this is
// intentionally slow, since the data changes on the order of minutes/boots,
// not something that needs sub-second freshness.
#ifndef SAFTYFW_TASKS_STACK_MARGIN_POLLER_H
#define SAFTYFW_TASKS_STACK_MARGIN_POLLER_H

#include "kilnlink/kilnlink_stack_margin.h"

#ifdef __cplusplus
extern "C" {
#endif

// Samples exactly ONE task's live uxTaskGetStackHighWaterMark() (the next
// one in round-robin order) and advances the internal index. Cheap and
// bounded: one FreeRTOS API call, no allocation, no locking (the internal
// state is only ever touched from the one task that calls this, plus
// stack_margin_poller_snapshot() below reading it from link_task's request
// handler -- see that function's own doc comment for why a lock is not
// needed here).
//
// Resolves each target task's TaskHandle_t by name via xTaskGetHandle()
// (INCLUDE_xTaskGetHandle is 1 in this build's FreeRTOSConfig.h) rather
// than requiring each task module to export its own handle -- this keeps
// every one of those nine modules untouched (several are isolation-
// restricted, see docs/ARCHITECTURE.md section 2). If a target task has not
// been created yet (an unlikely ordering issue -- every task in TASKS below
// is created during normal boot before the scheduler is likely to reach
// this poller's own first tick), that slot is silently skipped this round
// and retried next time its turn comes up; it stays
// KILNLINK_STACK_MARGIN_UNMEASURED until it succeeds at least once.
//
// The resolved handle is CACHED after the first successful lookup (see
// stack_margin_poller.c's s_slots[].cached_handle) -- xTaskGetHandle()
// suspends the scheduler and walks every task list by name, which is fine
// once per task but wasteful to repeat on every ~500ms tick forever. This
// is safe only because none of the nine target tasks is ever deleted or
// recreated for the life of this firmware (no vTaskDelete() call exists
// anywhere in firmware/SaftyFW/src, confirmed by grep) -- a cached handle
// therefore never outlives the TCB it points at. If a future change gives
// any of these nine tasks a restart/respawn path, that change must also
// invalidate this cache (clear cached_handle when the task is torn down),
// or this module must revert to resolving by name on every tick.
void stack_margin_poller_tick(void);

// Fills `out` with the current cached snapshot -- whatever
// stack_margin_poller_tick() has measured so far, NOT a fresh synchronous
// measurement. Safe to call from any task (link_task, when handling
// SAFETY_CMD_GET_STACK_MARGIN). See kilnlink_stack_margin.h's own "FLOOR,
// NOT WORST CASE" doc comment before treating any of this as a ceiling.
void stack_margin_poller_snapshot(kilnlink_stack_margin_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_STACK_MARGIN_POLLER_H
