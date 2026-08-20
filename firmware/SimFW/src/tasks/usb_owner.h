// usb_owner.h -- Single owner of the USB CDC (TinyUSB) interface (docs/
// PLAN.md section 4.1 task map): "Frame RX/TX, CRC, dispatch to command
// queue, telemetry TX." Nobody else may touch the USB peripheral --
// single-owner-per-peripheral doctrine, PLAN.md section 4's opening
// paragraph. Only this file and usb_descriptors.c include tusb.h; no other
// task file should include TinyUSB headers or call its API -- route
// everything through the two functions this header exports for cmd_task's
// use (usb_owner_register_task() / usb_owner_send_reply()) instead.
//
// Real body: SLIP-style frame RX/TX over TinyUSB CDC, feeding/draining
// `benchproto_frame`'s codec and `benchproto_link`'s reliability/task-
// registration state machine (firmware/CommonFW/docs/BENCHPROTO.md). This
// task is the sole owner of the one `benchproto_link_t` on SimFW's side of
// the link -- cmd_task never touches it directly, only through the two
// functions below.
#ifndef SIMFW_TASKS_USB_OWNER_H
#define SIMFW_TASKS_USB_OWNER_H

#include <stdbool.h>
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

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_USB_OWNER_H
