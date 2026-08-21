// update_task.h -- Phase 10: the FreeRTOS task that actually receives
// UPDATE_BEGIN/UPDATE_DATA/UPDATE_END/UPDATE_ABORT frames over the isolated
// link and does the real flash I/O src/update/{image_header,received_ranges,
// update_receiver,confirm}.h's pure decision logic was built (and
// host-tested, 272/272) against but never itself performs. See
// firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 4 for the wire flow and
// docs/BOOTLOADER.md section 5 for the rollback/confirmation contract this
// task is the runtime half of.
//
// Not subject to link_task.{c,h}'s GPIO6/relay isolation rule
// (tools/check_isolation.ps1 only greps link_task.{c,h}) -- this file
// legitimately needs safety_core_get_output_status()/_get_diag_status() to
// gather Phase 10's own preconditions (relay open, no trip pending), the
// same legal channel link_task.c's own status frame already uses for the
// two bits it needs. This file must still never reference GPIO6 or
// relay_owner.h directly (safety_core is the only legal channel, same as
// link_task.c), even though nothing enforces that by machine here.
//
// --- Why this is a separate task from link_task, and how the two talk ---
//
// link_task owns UART1 RX/TX and must never be blocked (docs/ARCHITECTURE.md
// section 4: "link_task is the second-lowest priority... a busy, hostile,
// or noise-flooded link must never delay a trip"). Flash erase/program is
// real, multi-hundred-millisecond-at-a-time I/O that halts BOTH RP2040 cores
// for its duration (pico-sdk's flash_safe_execute(), see update_task.c's own
// header comment) -- running it inside link_task's own RX handler would mean
// a single UPDATE_DATA frame could stall frame assembly, the 500 ms Frame A
// telemetry cadence, and this task's own watchdog checkin, all at once.
// So link_task.c's switch in link_task_handle_raw_frame() only THIN-dispatches
// each UPDATE_* frame to one of the four update_task_handle_*() functions
// below, which copy the frame's payload into a small, bounded FreeRTOS queue
// (xQueueSend with a zero timeout -- never blocks the caller, drops and
// counts on a full queue, matching this codebase's "never block, never
// allocate after init" rule) and return immediately. update_task_fn()
// (below, this file's own task) drains that queue on its own schedule and
// does the actual parsing/flash I/O/status-reply work.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_H
#define SAFTYFW_TASKS_UPDATE_TASK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates update_task at SAFTYFW_PRIO_UPDATE_TASK, pinned to
// SAFTYFW_CORE_LINK_PATH (see task_priorities.h's own comment on that
// choice). Also performs the Phase 10 confirmation-gate startup check: reads
// the flash metadata log (XIP, same technique bootloader/main.c uses) and,
// if this boot's active slot is BOOTLOADER_SLOT_PENDING_VERIFY, arms the
// periodic confirm-gate tick that will eventually (see this file's header
// comment on config_crc_ok) mark it BOOTLOADER_SLOT_VALID. Returns false if
// task or queue creation failed.
bool update_task_start(void);

// The four frame handlers link_task.c's RX dispatch calls, one per
// LINK_FRAME_UPDATE_{BEGIN,DATA,END,ABORT}_CMD case. `payload`/`length` are
// the raw kilnlink frame payload/length exactly as link_task_handle_raw_frame()
// received them -- payload[0] is still the command byte (same convention
// link_task_handle_push_context() already uses for PUSH_CONTEXT), and
// update_task_fn()'s internal handlers strip it before interpreting the rest.
// Each function copies its argument into update_task's own bounded RX queue
// (xQueueSend, zero timeout) and returns immediately -- never blocks the
// caller (link_task), matching this codebase's non-blocking-enqueue
// discipline (see relay_owner.h's command queue for the established pattern
// on this project). A full queue drops the frame silently (counted
// internally, not yet exposed as a getter -- diagnostic-only for now,
// matching several other "exposed but unconsumed" counters elsewhere in
// this codebase).
void update_task_handle_begin(const uint8_t *payload, uint8_t length);
void update_task_handle_data(const uint8_t *payload, uint8_t length);
void update_task_handle_end(const uint8_t *payload, uint8_t length);
void update_task_handle_abort(const uint8_t *payload, uint8_t length);

// SAFETY_CMD_ROLLBACK (0x17) -- tools/PcTools/TODO.md's
// `ota_rollback(processor)` line, Pico half. Called SYNCHRONOUSLY from
// link_task_handle_rollback() (src/tasks/link_task.c), not queued like the
// four handlers above -- see this function's own doc comment in
// update_task.c for why a single metadata-record write is small enough to
// do inline, the same way config_store_write() already is.
//
// Refuses (returns false, fills `*out_reason` if non-NULL) if the relay is
// currently ARMED, or if bootloader/metadata.c's bootloader_decide_rollback()
// refuses because the OTHER bootloader slot is not currently VALID or
// PENDING_VERIFY -- the property that keeps a rollback from ever stranding
// the board with zero bootable slots. On success this function DOES NOT
// RETURN: it calls watchdog_reboot() and the RP2040 resets immediately.
bool update_task_request_rollback(const char **out_reason);

// True while an UPDATE_BEGIN...UPDATE_END/ABORT transfer is actively staged
// (s_transfer_active, update_task.c) -- i.e. between a UPDATE_BEGIN this
// task accepted and whichever of UPDATE_END/UPDATE_ABORT/an internal
// retransmit-cap abort ends it. Read from safety_core.c
// (safety_core_request_enable()) as this Pico's OWN, independent half of
// the mutual "heating is not allowed during updates" interlock (ROADMAP.md
// M8, docs/UPDATE_PROTOCOL.md section 1): the ESP is never consulted for
// this -- update_task's own s_transfer_active is the ground truth for
// whether THIS processor is mid-update, exactly the same
// "this file legitimately reaches relay state only through
// safety_core_get_output_status()" one-way channel this header's own top
// comment documents for the opposite direction (update_task reading relay
// state), now used in reverse (safety_core reading update state).
//
// s_transfer_active is written only by update_task_fn()'s own dispatch
// (single writer); this getter is called from other tasks (safety_core.c),
// so the backing variable is `volatile` -- a torn/stale read here can only
// ever be "reads not-yet-true for one more poll", never "reads true when it
// isn't", because the flag is set the instant UPDATE_BEGIN is accepted and
// safety_core_request_enable() only runs on an explicit ESP-initiated
// enable request, never on a fixed schedule that could race the flag's own
// transition in the unsafe direction.
bool update_task_transfer_active(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UPDATE_TASK_H
