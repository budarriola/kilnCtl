// log_task.h -- drains the log ring into the link (Phase 8). Lowest priority
// in the system, always droppable (docs/ARCHITECTURE.md section 1: "A log
// frame must never be able to displace a telemetry frame").
//
// Wire shape: KilnFW's existing LOG payload (CommonFW/docs/LINK_PROTOCOL.md
// section 6, "Frame F"; firmware/KilnFW/App/drivers/common/uart_task_ids.h:429-443)
// -- byte0 = level, the rest ASCII "TAG: message", not null-terminated,
// truncated rather than split. No new task id, no new frame type: log_task
// hands entries to link_task_send_log() (link_task.h), which addresses them
// to task_id 5 the same way KilnFW's own uart_log_bridge.c does.
#ifndef SAFTYFW_TASKS_LOG_TASK_H
#define SAFTYFW_TASKS_LOG_TASK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Same numeric values as KilnFW's UART_LOG_LEVEL_* (uart_task_ids.h) --
// deliberately mirrored rather than included, same "SaftyFW must not depend
// on KilnFW headers" reasoning link_task.c's own header comment gives for
// LINK_DEVICE_ESP/LINK_TASK_ID_SAFETY.
#define LOG_LEVEL_ERROR   0u
#define LOG_LEVEL_WARN    1u
#define LOG_LEVEL_INFO    2u
#define LOG_LEVEL_DEBUG   3u
#define LOG_LEVEL_VERBOSE 4u

// Creates log_task at SAFTYFW_PRIO_LOG_TASK, pinned to
// SAFTYFW_CORE_LINK_PATH (it drains into link_task's TX ring, so it belongs
// on the same core). Returns false if task creation failed.
bool log_task_start(void);

// Non-blocking enqueue -- callable from ANY task, on either core (this is
// the whole point: a guard on core 1 can log without ever risking a wait on
// core 0's link path). Formats "tag: message" (tag may be NULL to omit it)
// into a fixed-size entry and posts it to log_task's internal queue with a
// zero timeout (xQueueSend(..., 0)), exactly like relay_owner's own command
// queue (relay_owner.c) -- never blocks the caller, no-hang rule 3
// (docs/ARCHITECTURE.md section 1: "best-effort and droppable... never
// blocking the task that logged").
//
// Lines at a level less severe than the current runtime filter
// (log_task_get_level()) are discarded here, before ever reaching the queue
// -- not counted as a "drop" (log_task_get_dropped()), since that counter is
// for capacity/reserve losses, not lines that were never meant to go out.
//
// Returns false if the line was filtered OR the queue was full (dropped,
// counted). True only means "queued", not "transmitted" -- the TX-reserve
// watermark (docs/ARCHITECTURE.md section 1) can still drop it later inside
// log_task itself, also counted.
bool log_task_log(uint8_t level, const char *tag, const char *msg);

// Runtime log-level filter: entries at a level less severe than this are
// dropped at log_task_log(), never queued. Default LOG_LEVEL_WARN (errors
// and warnings only) per docs/ARCHITECTURE.md section 1, "Isolated-path log
// emission": "Default: errors and warnings only, with verbose levels enabled
// on demand." Safe to call from any task -- backed by a single volatile
// read/write, same pattern discrete_task.h/relay_owner.h use for their own
// cross-task flags.
//
// ARCHITECTURE.md section 1 also calls for this to be settable over the link
// at runtime ("This one IS runtime-configurable"; TODO.md Phase 2's "runtime
// log-level command" bullet) -- that RX command handler in link_task.c is
// NOT built in this pass (see TODO.md); log_task_set_level() exists so that
// work has a real function to call once it lands, rather than needing to
// invent one then. Until it exists, only the compiled-in default applies.
void log_task_set_level(uint8_t level);
uint8_t log_task_get_level(void);

// Total count of log lines that never reached the wire: queue-full at
// log_task_log() (capacity exhausted), the TX-reserve watermark refusing to
// hand a line to link_task (docs/ARCHITECTURE.md section 1's "count the
// drops... so 'the log went quiet' is distinguishable from 'nothing was
// logged'"), or link_task_send_log() itself reporting the frame dropped
// (uart_owner's TX ring had no room at the moment of send). One counter
// covering all three, not three separate ones -- from a consumer's
// perspective ("did I lose log lines and how many") the cause matters less
// than the fact, and uart_owner_get_tx_dropped() already exists separately
// for telemetry's own drop accounting if that finer distinction is ever
// wanted. Diagnostic only; not yet folded into Frame B (SAFETY_CMD_DIAG,
// kilnlink/kilnlink_diag.h) -- that frame's byte layout is fixed by
// CommonFW/docs/LINK_PROTOCOL.md and has no spare field for it (see
// KILNLINK_DIAG_LEN's own doc comment); this counter is built and exposed
// for a future consumer, not yet wired into any frame.
uint32_t log_task_get_dropped(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LOG_TASK_H
