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
#include <stdint.h>

#include "snapshots.h"

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

// Copies the newest well-formed SAFETY_CMD_PUSH_CONTEXT (0x07) snapshot into
// `*out`, mutex-guarded (same short-bounded-wait discipline as
// thermo_task_get_snapshot() -- see thermo_task.c). `*out` is always
// zero/false-initialised first (out->valid = false, out->zone_count = 0,
// etc.), then overwritten from the locked snapshot only if one has ever been
// published; the "nothing received yet" default is therefore always safe to
// read even on a false return. Returns false if `out` is NULL, no context
// frame has ever been successfully parsed this boot, or the lock could not
// be taken within its timeout.
//
// Has no caller yet in this codebase -- Phase 7's context-dependent guards
// (S2/S6/S10, TODO.md) are the intended future reader, once they exist; it
// is published now so that work has something to read from day one, the
// same "exposed but unconsumed" state several of Phase 4/6's own getters are
// already in.
bool link_task_get_context_snapshot(context_snapshot_t *out);

// Hands a pre-built LOG payload (byte0 = level, rest ASCII "TAG: message" --
// see CommonFW/docs/LINK_PROTOCOL.md section 6 "Frame F") to uart_owner as a
// BROADCAST addressed to task_id 5, the same wire shape KilnFW's own
// uart_log_bridge.c uses for its ESP_LOGx output. log_task is the only
// intended caller (docs/ARCHITECTURE.md section 1: log frames go through
// log_task, never direct from an arbitrary task into the link). Non-blocking,
// same contract as uart_owner_send(): returns false if the frame was dropped
// (TX ring had no room), true if it was queued. Does not itself count
// drops -- log_task owns that counter, see log_task.h.
bool link_task_send_log(const uint8_t *payload, uint8_t length);

// Fraction (0.0..1.0) of the TX ring currently in use -- a snapshot for
// log_task's TX-reserve watermark check (docs/ARCHITECTURE.md section 1:
// "reserve TX ring capacity for telemetry... log frames are dropped at
// enqueue once the ring is above a reserve watermark"). Wraps uart_owner's
// fill query so log_task does not need its own uart_owner.h dependency for
// one number -- link_task already owns the "how is the TX ring doing"
// question for everything else.
float link_task_get_tx_ring_fill_fraction(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_H
