// safety_core.h -- assembles snapshots, will call safety_guards_tick() (Phase
// 4's pure evaluator, already ported in src/safety_guards.c but NOT yet wired
// in here -- that integration is separate follow-on work, not this phase's),
// and commands relay_owner.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): this file, and
// safety_core.c, must never #include a link or uart header. Checked by
// tools/check_isolation.ps1 as well as by inspection -- see that script.
#ifndef SAFTYFW_TASKS_SAFETY_CORE_H
#define SAFTYFW_TASKS_SAFETY_CORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates safety_core at SAFTYFW_PRIO_SAFETY_CORE, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool safety_core_start(void);

// Telemetry pull for link_task's status frame (LINK_PROTOCOL.md's Frame A):
// "K4 energized" and "heating currently permitted". link_task.c is
// structurally forbidden from including relay_owner.h or naming the relay at
// all (tools/check_isolation.ps1), so this is the one legal channel -- a
// plain data pull through safety_core, which already legitimately depends on
// relay_owner (it commands it), not a new coupling. relay_energized comes
// from relay_owner_is_energized() (is GPIO6 actually high right now);
// heating_enabled from relay_owner_get_state() == ARMED (would a genuine
// energize command be honoured right now) -- matching safety_link.h's own
// documented bit semantics ("heating currently permitted", not "currently
// heating"). Safe to call from any task.
void safety_core_get_output_status(bool *out_relay_energized, bool *out_heating_enabled);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_SAFETY_CORE_H
