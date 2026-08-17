// link_task.h -- frame RX -> context snapshot; telemetry TX (Phase 7/8).
// docs/ARCHITECTURE.md section 2, "the one rule that matters": link_task
// NEVER touches the relay, and never references GPIO6 by name or number.
// tools/check_isolation.ps1 greps this file (and link_task.c) for exactly
// that.
//
// link_task is a PRODUCER of data, never a service anything waits on
// (section 2: "never called BY safety_core"). It publishes; safety_core
// pulls, on its own schedule, and can run correctly with link_task dead,
// hung, or never started.
#ifndef SAFTYFW_TASKS_LINK_TASK_H
#define SAFTYFW_TASKS_LINK_TASK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates link_task at SAFTYFW_PRIO_LINK_TASK, pinned to
// SAFTYFW_CORE_LINK_PATH -- core 0, deliberately the opposite core from
// everything that can trip the relay (docs/ARCHITECTURE.md section 4).
// uart_owner_init() must already have been called (main.c, before this).
// Returns false if task creation failed.
bool link_task_start(void);

// True once an ANNOUNCE_VERSION (0x0F) from the ESP has been evaluated as
// incompatible (CommonFW/docs/LINK_PROTOCOL.md section 4's "What each side
// does about a mismatch": the Pico enters DEGRADED_NO_CONTEXT and does NOT
// latch a trip -- a version mismatch means the two processors were flashed
// out of step, not that the kiln is unsafe). False before the first
// ANNOUNCE_VERSION is ever seen, and false again once a compatible one
// arrives. Nothing consumes this yet -- Phase 7's context-frame parsing and
// its context-dependent guards (S2/S3/S4/S10/S13) are the intended future
// reader, once they exist; it is published now so that work has something to
// read from day one.
bool link_task_get_degraded_no_context(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_H
