// cmd_task.h -- Per docs/DESIGN_NOTES.md section 4.1 task map: "Decode/validate
// commands, route to owners, build replies." Owns no peripheral itself
// (single-owner-per-peripheral doctrine, DESIGN_NOTES.md section 4's opening
// paragraph) -- it is the one place that turns a decoded, delivered
// benchproto DATA frame (handed to it by usb_owner via the queue exposed
// below) into a dispatch against its own command table, and hands the reply
// back to usb_owner (usb_owner_send_reply(), usb_owner.h) to go out as that
// frame's ACK. No owner task's state is ever touched directly from here
// (DESIGN_NOTES.md section 4.5: "No command touches another task's state
// directly") -- true today by construction: every group but SYS is still a
// stub (see cmd_task.c's dispatch table), so nothing here reaches into
// spi_emu_a/b, wave_owner, i2c_owner, or sim_engine yet.
#ifndef SIMFW_TASKS_CMD_TASK_H
#define SIMFW_TASKS_CMD_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "benchproto/benchproto_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// One delivered benchproto DATA frame, queued from usb_owner's RX path to
// cmd_task's inbox (cmd_task_get_inbox()). `dst_task` is the SIMFW_TASK_ID_*
// (cmd_ids.h) the frame was addressed to -- cmd_task's own dispatch key,
// alongside payload[0] (the command's subcommand byte, PROTOCOL.md's "Reply
// convention"). `src_device`/`src_task`/`msg_index` are carried through
// unmodified so cmd_task can hand them back to usb_owner_send_reply()
// (usb_owner.h) to address the reply frame -- cmd_task never builds a
// benchproto_frame_t itself, that stays inside usb_owner.c.
typedef struct {
    uint8_t dst_task;
    uint8_t src_device;
    uint8_t src_task;
    uint16_t msg_index;
    uint8_t length;
    uint8_t payload[BENCHPROTO_FRAME_MAX_PAYLOAD];
} cmd_task_request_t;

// Depth 4: matches BENCHPROTO_DEDUP_DEPTH (benchproto_link.h) -- there is no
// reason for more requests to be in flight toward this single-threaded
// dispatcher than the host's own dedup ring could plausibly have
// outstanding, and usb_owner's own delivery rule (withhold the ACK, per
// BENCHPROTO.md sec 4, when this queue is full) means a deeper queue would
// only delay that natural backpressure signal, not avoid it.
#define CMD_TASK_INBOX_DEPTH 4u

// Creates cmd_task at SIMFW_PRIO_CMD_TASK, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Creates the inbox queue
// (cmd_task_get_inbox()) and registers every SIMFW_TASK_ID_* command group
// (cmd_ids.h) with usb_owner (usb_owner_register_task(), usb_owner.h) before
// returning -- safe to call right after usb_owner_start() (main.c's
// existing order) because usb_owner_start() initializes its
// benchproto_link_t synchronously, before its own task body (and therefore
// any RX traffic) ever runs; see that function's own doc comment. Returns
// false if the queue or the task itself failed to create.
bool cmd_task_start(void);

// The queue usb_owner's RX path posts delivered DATA frames to
// (xQueueSend() with a zero timeout -- BENCHPROTO.md sec 4's backpressure
// rule: a full queue means usb_owner withholds the ACK rather than blocking
// its own RX loop). Returns NULL if cmd_task_start() hasn't run yet.
QueueHandle_t cmd_task_get_inbox(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_CMD_TASK_H
