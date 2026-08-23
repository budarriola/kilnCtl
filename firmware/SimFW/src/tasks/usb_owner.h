// usb_owner.h -- Single owner of the USB CDC (TinyUSB) interface (docs/
// DESIGN_NOTES.md section 4.1 task map): "Frame RX/TX, CRC, dispatch to command
// queue, telemetry TX." Nobody else may touch the USB peripheral --
// single-owner-per-peripheral doctrine, DESIGN_NOTES.md section 4's opening
// paragraph. Only this file and usb_descriptors.c include tusb.h; no other
// task file should include TinyUSB headers or call its API -- route
// everything through the functions this header exports (cmd_task's
// usb_owner_register_task() / usb_owner_send_reply(), and now
// drivers/console_sink.c's usb_owner_console_write()) instead.
//
// Real body: SLIP-style frame RX/TX over TinyUSB CDC, feeding/draining
// `benchproto_frame`'s codec and `benchproto_link`'s reliability/task-
// registration state machine (firmware/CommonFW/docs/BENCHPROTO.md). This
// task is the sole owner of the one `benchproto_link_t` on SimFW's side of
// the link -- cmd_task never touches it directly, only through the two
// functions below.
//
// --- Dual CDC (2026-08-23) ---------------------------------------------
// usb_descriptors.c's config descriptor now declares TWO CDC-ACM instances
// on one composite device: instance 0 is everything above (unchanged --
// plain `tud_cdc_*()` calls in this file's TX/RX code all resolve to
// instance 0), instance 1 is a console/log sink (usb_owner_console_write()
// below), added because interleaving human-readable log text into the
// benchproto binary stream would corrupt its framing -- these had to be two
// genuinely separate CDC functions, not two uses of one.
//
// **1200-baud bootloader touch is honoured on instance 0 ONLY.** This task's
// tud_cdc_line_coding_cb()/tud_cdc_line_state_cb() callbacks receive an
// `itf` argument (the CDC instance the line-coding/line-state change
// happened on) and now check it before ever calling
// usb_owner_check_bootloader_touch(). Deliberate, not an oversight: CDC1 is
// the port a bench operator's ordinary terminal program (PuTTY, screen,
// TeraTerm, a serial monitor) opens just to watch log text, and terminal
// programs are exactly the class of tool that fiddles with line coding and
// DTR on open/close for reasons that have nothing to do with asking this
// fixture to reboot -- some default to legacy baud rates, and DTR toggling
// on port open/close is near-universal. Honouring the touch on CDC1 too
// would mean "a human plugs in a log viewer" could occasionally, silently,
// reboot the fixture into its bootloader -- exactly the accidental-trigger
// class PROTOCOL.md's "gated on baud AND DTR together" reasoning already
// exists to prevent, just one interface over. The protocol CDC (instance 0)
// is where PC-side tooling that legitimately wants to trigger a reflash
// actually connects (kilnsim, picotool-alikes), so that is the one interface
// this convention needs to work on.
//
// usb_owner_console_write() is deliberately non-blocking and drops (never
// buffers, never blocks the calling task) whenever CDC1 isn't connected or
// its TX FIFO can't take the whole write right now -- a bench tool that
// stalls its own tasks because no terminal is attached to the log port would
// be worse than one that silently drops a line of text nobody was reading
// anyway. Safe to call from ANY task, on either core, before or after
// usb_owner_start() has run (returns false, does nothing, if usb_owner's own
// TinyUSB init hasn't completed yet).
#ifndef SIMFW_TASKS_USB_OWNER_H
#define SIMFW_TASKS_USB_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates usb_owner at SIMFW_PRIO_USB_OWNER, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Initializes the underlying
// benchproto_link_t synchronously (before returning), NOT inside the task
// body -- so a caller (cmd_task_start(), in main.c's start order) can call
// usb_owner_register_task() immediately afterward, before the scheduler
// (and therefore usb_owner's own RX loop) ever runs. Returns false if task
// creation failed.
bool usb_owner_start(void);

// Registers task_id (SIMFW_TASK_ID_* , cmd_ids.h) as a valid destination on
// usb_owner's benchproto_link_t -- thin wrapper around
// benchproto_link_register_task() (BENCHPROTO.md sec 6) so cmd_task never
// touches the link struct itself. Intended to be called once per group id,
// synchronously from cmd_task_start(), which main.c already calls after
// usb_owner_start() -- see that function's own doc comment for why the
// ordering is safe. Returns false if task_id is already registered or no
// slot remains (BENCHPROTO_MAX_TASKS, benchproto_link.h).
bool usb_owner_register_task(uint8_t task_id);

// Sends the reply to a command cmd_task just finished handling, as the ACK
// for the DATA frame that carried it (benchproto's request/reply model --
// see BENCHPROTO.md sec 3/5: this build piggybacks the actual reply payload
// in the ACK itself rather than UnitTestFw's older two-frame
// ACK-then-separate-DATA INFO pattern, documented in docs/PROTOCOL.md's
// "Reply convention" section).
//
// `responding_task_id` is the SIMFW_TASK_ID_* the request was addressed to
// (also this reply's own src_task) -- used to key usb_owner's small
// per-task last-ACK cache, so a BENCHPROTO_LINK_ACTION_DUPLICATE_REACK (the
// host's retry outran this ACK) can resend the exact same bytes rather than
// recomputing a possibly-different answer. `dst_device`/`dst_task`/
// `msg_index` are echoed from the original request frame's src_device/
// src_task/msg_index (BENCHPROTO_LINK_ACTION_DELIVER's own contract:
// "send an ACK built from this frame's (src_device, src_task, msg_index)").
// `payload`/`length` is the reply body cmd_task built (length may be 0).
//
// Thread-safe: called from cmd_task (a different task than usb_owner's own),
// guarded internally by a mutex around the shared CDC TX path and the ACK
// cache. Returns false if the reply couldn't be encoded/sent (buffer
// too small, CDC not connected) -- cmd_task has no retry of its own for
// this; the host's own retry (resending the original DATA frame) is what
// eventually gets a DUPLICATE_REACK resend once the link recovers.
bool usb_owner_send_reply(uint8_t responding_task_id, uint8_t dst_device, uint8_t dst_task, uint16_t msg_index,
                           const uint8_t *payload, uint8_t length);

// Sends an unsolicited BROADCAST frame (BENCHPROTO_MSG_BROADCAST,
// BENCHPROTO.md sec 4: "never ACKed, never NACKed, never deduped") --
// docs/DESIGN_NOTES.md section 5.3's TELEMETRY/EVT frames. Added alongside
// usb_owner_send_reply() (this pass, telemetry.c's real body): usb_owner
// remains the CDC's sole owner (this header's file comment), so the
// "actual outbound BROADCAST path" docs/PROTOCOL.md section 6 calls
// "telemetry's own work" still has to be handed off through here rather
// than telemetry.c touching tud_cdc_write()/benchproto_link_t itself.
//
// `src_task` is the SIMFW_TASK_ID_* (cmd_ids.h) this broadcast is FROM --
// telemetry.c passes SIMFW_TASK_ID_EVT for both its periodic TELEMETRY
// frames and its per-event EVT frames, disambiguated by a frame-kind byte
// telemetry.c/docs/PROTOCOL.md define in the payload itself (BROADCAST's
// own dst_task has no specific registered receiver on this side to route
// to, so it is sent as 0 -- benchproto's addressing model is built for
// DATA/ACK/NACK's request/reply pairing, not a specific requirement for
// unsolicited traffic, BENCHPROTO.md sec 4).
//
// `payload`/`length` is the frame body (length may be 0). Never queued or
// retried: if the CDC TX path is busy (mutex contention) or the write is
// short, this returns false immediately and the caller counts it as a
// dropped frame rather than blocking or retrying (DESIGN_NOTES.md 4.5's drop
// policy: "nothing ever blocks... a full queue toward them is a counted
// drop"). Not cached for DUPLICATE_REACK resend the way usb_owner_send_reply()'s
// ACKs are -- BROADCAST frames are never acknowledged or retried by
// definition (BENCHPROTO.md sec 4), so there is nothing to resend.
bool usb_owner_send_broadcast(uint8_t src_task, const uint8_t *payload, uint8_t length);

// Writes `len` bytes of console/log text to CDC1 (the console CDC instance
// -- see this header's "Dual CDC" section above), for drivers/console_sink.c's
// stdio_driver_t to call from its out_chars callback. NEVER blocks and NEVER
// buffers/retries: returns false immediately (no bytes written) if CDC1 has
// no host attached, if the write would not fit CDC1's TX FIFO right now, or
// if usb_owner's own TinyUSB init hasn't completed yet -- a caller (the
// stdio layer, ultimately printf()/simfw_fatal()) must treat a dropped
// console line the same way it already treats a line that never made it to
// the retained log ring: not an error worth propagating, just "nobody was
// listening on that channel this time." UART0 output (pico_stdio_uart) is
// completely independent of this function and keeps working whether or not
// this returns true -- see main.c's console_sink_register() call site for
// why both channels stay live rather than one replacing the other.
bool usb_owner_console_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_USB_OWNER_H
