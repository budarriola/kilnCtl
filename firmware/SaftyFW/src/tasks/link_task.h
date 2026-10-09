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

// Hands a pre-built payload to uart_owner as a BROADCAST addressed to task_id
// 7 (SAFETY), the same destination Frame A/B/C already use -- exposed for
// update_task.c's UPDATE_STATUS (0x14) replies (Phase 10), which need to
// reach the ESP's SAFETY task the same way telemetry does, but from a task
// other than link_task itself (flash I/O does not belong on link_task's own
// priority -- see update_task.c's header comment). Same non-blocking,
// drop-on-full contract as link_task_send_log(): returns false if the TX
// ring had no room, true if the frame was queued. update_task.c owns
// whatever counter it wants for its own drops; this function does not count
// them, matching link_task_send_log()'s same division of responsibility.
bool link_task_send_safety(const uint8_t *payload, uint8_t length);

// Total number of times link_task_send_status()'s underlying uart_owner_send()
// call has returned success since link_task_start() -- i.e. a Frame A status
// payload was handed to the TX ring and NOT dropped. This is the closest
// available evidence this link's design has for "telemetry is actually being
// sent" (src/update/confirm.h's telemetry_sent_ok, Phase 10's confirmation
// gate): the Pico never receives or waits for an ACK on anything it sends
// (CommonFW/docs/LINK_PROTOCOL.md section 2), so there is no stronger signal
// to report than "queued for transmission", and confirm.h's own header
// comment is explicit that the gap between "sent" and "received by a healthy
// ESP" stays open here, honestly. Safe to call from any task.
uint32_t link_task_get_status_tx_ok_count(void);

// Fraction (0.0..1.0) of the TX ring currently in use -- a snapshot for
// log_task's TX-reserve watermark check (docs/ARCHITECTURE.md section 1:
// "reserve TX ring capacity for telemetry... log frames are dropped at
// enqueue once the ring is above a reserve watermark"). Wraps uart_owner's
// fill query so log_task does not need its own uart_owner.h dependency for
// one number -- link_task already owns the "how is the TX ring doing"
// question for everything else.
float link_task_get_tx_ring_fill_fraction(void);

// link_task_link_up() and link_task_get_relay_on_continuous_ms() are declared
// (with their full doc comments) in snapshots.h, not here -- safety_core.c,
// their real consumer, is structurally forbidden from #include-ing any header
// naming "link"/"uart" (tools/check_isolation.ps1), and snapshots.h is
// already the shared, isolation-legal home this header comment's own
// link_task_get_context_snapshot()/link_task_get_degraded_no_context() are
// mirrored into for the same reason. Both are implemented in link_task.c.

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_H
