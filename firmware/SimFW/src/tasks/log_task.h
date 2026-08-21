// log_task.h -- REAL BODY (docs/DESIGN_NOTES.md section 4.1 task map): "Deferred
// logging, drop-counting, never blocks producers." Lowest priority in the
// system, always droppable -- mirrors ../../SaftyFW/src/tasks/log_task.h's
// doctrine: a log line must never be able to displace a telemetry frame,
// and no producer task ever blocks waiting for this one.
//
// --- Transport decision (DESIGN_NOTES.md section 5.3 does not pin this down; this
// pass's own call, documented per the pass instructions) -------------------
// SaftyFW's log_task sends each line out over its own wire as its own frame
// type (link_task_send_log(), a fixed kilnlink task id). SimFW does NOT do
// that here: log lines are drained into a small retained ring
// (log_task_get_entries() below) that a future LOG-style query command can
// read, and are never sent unsolicited over USB CDC.
//
// Reasoning: DESIGN_NOTES.md 5.3 gives `telemetry` (default 2 Hz, EVT frames
// immediate) sole ownership of SimFW's unsolicited-broadcast bandwidth --
// benchproto's BROADCAST frames are also the only unacknowledged, unretried
// wire traffic this link has (BENCHPROTO.md sec 4), so anything sent that
// way is exactly the traffic DESIGN_NOTES.md 4.5's drop policy is built to protect
// telemetry/EVT from having to compete with. A bench-tool's debug log is
// not on that protected path (unlike SaftyFW's Frame A/B, which the safety
// link genuinely needs both to succeed) -- a poll-based query is simpler
// (no new frame kind, no TX-reserve watermark to size against telemetry's
// own budget, no interleaving to reason about on the wire) and, by
// construction, can never compete with telemetry for USB bandwidth: it
// consumes zero bytes until a client explicitly asks. The retained ring
// below is exactly what such a future query command would read; wiring the
// actual LOG command group (a new SIMFW_TASK_ID_*, cmd_ids.h) is left for
// when one is needed, per this pass's own file-scope limits.
#ifndef SIMFW_TASKS_LOG_TASK_H
#define SIMFW_TASKS_LOG_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors SaftyFW's log_task.h LOG_LEVEL_* values (same numeric meaning,
// deliberately re-declared rather than shared -- SimFW does not depend on
// SaftyFW headers, same reasoning SaftyFW's own link_task.c gives for not
// depending on KilnFW's).
#define LOG_LEVEL_ERROR   0u
#define LOG_LEVEL_WARN    1u
#define LOG_LEVEL_INFO    2u
#define LOG_LEVEL_DEBUG   3u
#define LOG_LEVEL_VERBOSE 4u

// "TAG: message" bytes retained per entry, both in the inbound queue entry
// and the retained ring below.
#define LOG_TASK_ENTRY_MSG_MAX 96u

// Creates log_task at SIMFW_PRIO_LOG_TASK, pinned to SIMFW_CORE_ELASTIC_PATH
// (task_priorities.h). Also creates the inbound bounded queue and the
// retained-entries ring's guard. Returns false if task/queue creation
// failed.
bool log_task_start(void);

// Non-blocking enqueue -- callable from ANY task, on either core (the whole
// point: a producer on SIMFW_CORE_RT_PATH must never risk a wait on this
// lowest-priority task, DESIGN_NOTES.md 4.5's "nothing ever blocks" rule). Formats
// "tag: message" (tag may be NULL to omit it) into a fixed-size entry and
// posts it to log_task's internal queue with a zero timeout
// (xQueueSend(..., 0)), the same non-blocking-producer shape every other
// command queue in this codebase uses (e.g. i2c_owner.c's).
//
// Lines at a level less severe than the current runtime filter
// (log_task_get_level()) are discarded here, before ever reaching the
// queue -- not counted as a "drop" (log_task_get_dropped()), since that
// counter is for capacity losses, not lines that were never meant to go
// out (same distinction SaftyFW's log_task.h draws).
//
// Returns false if the line was filtered OR the queue was full (dropped,
// counted). True only means "queued", not "retained" -- see
// log_task_get_entries()'s own doc comment for the (uncounted, by design)
// case where a retained entry is overwritten before a client reads it.
bool log_task_log(uint8_t level, const char *tag, const char *msg);

// Runtime log-level filter: entries at a level less severe than this are
// dropped at log_task_log(), never queued. Default LOG_LEVEL_WARN (errors
// and warnings only), matching SaftyFW's own default. Safe to call from any
// task -- backed by a single volatile read/write, same pattern
// i2c_owner.h's own single-word cross-task state uses.
void log_task_set_level(uint8_t level);
uint8_t log_task_get_level(void);

// Total count of log lines that never reached the retained ring: queue-full
// at log_task_log() (capacity exhausted) is the only source today (there is
// no wire send to fail the way SaftyFW's log_task has, per this file's
// transport decision above). One counter, matching SaftyFW's own "one
// counter covering all loss causes" reasoning -- a consumer wants "did I
// lose log lines and how many," not a breakdown.
uint32_t log_task_get_dropped(void);

// One retained log line, returned by log_task_get_entries() below.
typedef struct {
    uint32_t seq;   // monotonic, gap = a query client's own loss detection,
                     // same seq/gap idiom i2c_owner.h's edge log uses
    uint8_t  level;
    uint8_t  len;   // bytes of msg actually used
    char     msg[LOG_TASK_ENTRY_MSG_MAX];
} log_task_entry_t;

// Retained-ring capacity. Sized like i2c_owner.h's edge-log ring (64):
// generous headroom for a burst of log lines between two poll-query calls
// without wrapping past what a slow-polling client can still read, small
// next to the RP2040's RAM budget.
#define LOG_TASK_RETAIN_CAPACITY 64u

// Copies up to max_out entries with seq > since_seq (0 = "from the oldest
// entry still retained") into out, oldest-first. Returns the number of
// entries copied. Mirrors i2c_owner_get_relay_edges()'s own contract
// exactly, including its loss note: once the ring has wrapped past an
// entry a client never read, that entry is gone -- not counted in
// log_task_get_dropped() (it WAS logged and drained, just aged out of the
// retained window), but visible to a careful caller as a seq gap in the
// returned run, same as i2c_owner's edge log.
size_t log_task_get_entries(log_task_entry_t *out, size_t max_out, uint32_t since_seq);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_LOG_TASK_H
