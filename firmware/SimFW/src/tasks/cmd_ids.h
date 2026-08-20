// cmd_ids.h -- SimFW's own benchproto task/device/command-id table (docs/
// PLAN.md section 5's command-group table, section 5.1's task-registration
// model). This is SimFW-specific numbering layered on top of `benchproto`
// (firmware/CommonFW/include/benchproto/), exactly the split
// BENCHPROTO.md section 7 describes: "SimFW's own task table ... is defined
// by SimFW itself, not by this library." The authoritative human-readable
// copy of this table is docs/PROTOCOL.md -- keep the two in sync in the same
// commit, the same discipline task_priorities.h's header comment asks of
// docs/PLAN.md section 4.1.
//
// Owned by cmd_task.c (the one place that registers/dispatches these ids);
// usb_owner.c includes it only for SIMFW_DEVICE_HOST/SIMFW_DEVICE_TARGET,
// needed to build outgoing NACK frames' own addressing.
#ifndef SIMFW_TASKS_CMD_IDS_H
#define SIMFW_TASKS_CMD_IDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// benchproto SRC_DEVICE/DST_DEVICE values -- BENCHPROTO.md section 3's frame
// layout table: "SRC_DEVICE | 1 | link-defined; SimFW uses HOST=0, TARGET=1."
#define SIMFW_DEVICE_HOST   0u
#define SIMFW_DEVICE_TARGET 1u

// Command-group task ids (PLAN.md sec 5's table, sec 5.1: "Each command
// group ... registers as an addressable task"). Unique within this device
// only (BENCHPROTO.md sec 6) -- the PC side's own task ids are a completely
// separate numbering (kilnsim's own concern, tools/PcTools, not this file).
#define SIMFW_TASK_ID_SYS   1u
#define SIMFW_TASK_ID_MODEL 2u
#define SIMFW_TASK_ID_TC    3u
#define SIMFW_TASK_ID_CT    4u
#define SIMFW_TASK_ID_RELAY 5u
#define SIMFW_TASK_ID_IO    6u
#define SIMFW_TASK_ID_FAULT 7u
#define SIMFW_TASK_ID_EVT   8u

// SYS group command ids (PLAN.md sec 5's row: PING, GET_VERSION, RESET_SIM,
// SET_TIMESCALE, SET_SEED, GET_CAPS). Only PING/GET_VERSION/GET_CAPS have a
// table entry in cmd_task.c today -- the other three are reserved numbers
// with no handler yet, so an incoming request for them falls through to the
// same "not implemented" default every other group's commands get (see
// cmd_task.c's dispatch table comment) rather than being silently unrouted.
#define SIMFW_CMD_SYS_PING          0x01u
#define SIMFW_CMD_SYS_GET_VERSION   0x02u
#define SIMFW_CMD_SYS_RESET_SIM     0x03u
#define SIMFW_CMD_SYS_SET_TIMESCALE 0x04u
#define SIMFW_CMD_SYS_SET_SEED      0x05u
#define SIMFW_CMD_SYS_GET_CAPS      0x06u

// Reply-payload status byte -- byte 0 of every SYS/MODEL/TC/CT/RELAY/IO/
// FAULT reply payload (docs/PROTOCOL.md "Reply convention"). This is
// entirely a SimFW-level convention layered on top of benchproto's own
// ACK/NACK (which only says whether the frame was *delivered*, per
// BENCHPROTO.md sec 4/5) -- a command can be delivered fine and still fail
// at the application level (bad args, not implemented yet), and that is what
// this byte reports.
#define SIMFW_CMD_STATUS_OK               0x00u
#define SIMFW_CMD_STATUS_ERR_NOT_IMPL     0x01u
#define SIMFW_CMD_STATUS_ERR_BAD_ARGS     0x02u
#define SIMFW_CMD_STATUS_ERR_INTERNAL     0x03u

// GET_CAPS's fixed capability numbers (PLAN.md sec 4.1's task map and sec
// 4.3's thermal-model section): zone count is a 1-4 runtime parameter,
// default 3; spi_emu_a/b give 3 main-side + 1 safety-side TC channels;
// wave_owner gives 3 CT channels; the board's relay/TC/CT channel counts
// match at 3 (sec 4.3: "3 matches the board's relay/TC/CT channels").
#define SIMFW_CAPS_ZONE_COUNT_MIN     1u
#define SIMFW_CAPS_ZONE_COUNT_MAX     4u
#define SIMFW_CAPS_ZONE_COUNT_DEFAULT 3u
#define SIMFW_CAPS_TC_MAIN_CHANNELS   3u
#define SIMFW_CAPS_TC_SAFETY_CHANNELS 1u
#define SIMFW_CAPS_CT_CHANNELS        3u
#define SIMFW_CAPS_RELAY_CHANNELS     3u
// Reserved for future bits (MODEL/TC/CT/RELAY/IO/FAULT group availability,
// once those groups have real handlers); 0 today -- see docs/PROTOCOL.md's
// GET_CAPS section for the bit table this will grow into.
#define SIMFW_CAPS_FEATURE_BITMASK    0x00000000u

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_CMD_IDS_H
