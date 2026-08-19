// link_frame.h -- pure application-layer payload packing for the two Pico->ESP
// frames this pass implements: the existing 23-byte status frame
// (SAFETY_CMD_GET_STATUS = 0x01, firmware/KilnFW/App/drivers/safety_link.h)
// and SAFETY_CMD_FW_VERSION (0x0B, CommonFW/docs/LINK_PROTOCOL.md section 6
// Frame C), plus the pure ANNOUNCE_VERSION compatibility check both sides
// must agree on.
//
// Deliberately free of FreeRTOS, pico-sdk and kilnlink: every function here
// is a pure transform on caller-supplied buffers/values, so it can be built
// and tested on the host the same way safety_guards.c and kilnlink_frame.c
// already are (see docs/ARCHITECTURE.md section 3's description of
// safety_guards.c: "no RTOS, no SDK ... host-testable" -- this file follows
// the same discipline for the same reason: a one-byte offset error in the
// wire contract is exactly the kind of bug worth catching before a bench
// visit). link_task.c calls these to build the bytes it hands to
// kilnlink_frame_encode_raw() + kilnlink_stuff() + uart_owner_send().
#ifndef SAFTYFW_TASKS_LINK_FRAME_H
#define SAFTYFW_TASKS_LINK_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "safety_guards.h" // safety_trip_t -- pure, no RTOS/SDK, same as this file
#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Frame A: SAFETY_CMD_GET_STATUS (0x01), 23 bytes ------------------------
// Byte-for-byte the layout firmware/KilnFW/App/drivers/safety_link.h already
// parses -- see that file's header comment for the authoritative offsets.
#define LINK_FRAME_STATUS_CMD 0x01u
#define LINK_FRAME_STATUS_LEN 23u

// flags byte (offset 1): bits 0/1 (LINK_UP, FAULT) are the ESP's to own --
// this module never sets them, they simply are not parameters below.
#define LINK_FLAG_ESTOP      0x04u
#define LINK_FLAG_RELAY      0x08u
#define LINK_FLAG_ENABLED    0x10u
#define LINK_FLAG_TEMP_VALID 0x20u

// Packs the 23-byte status payload into `out` (must have room for
// LINK_FRAME_STATUS_LEN bytes). `safety_tc_c`/`cj_c` should already be NaN
// when `temp_valid` is false -- this function passes them through unchanged
// rather than substituting 0, matching LINK_PROTOCOL.md's "send NaN, never
// 0" rule; it does not itself decide validity.
void link_frame_pack_status(uint8_t out[LINK_FRAME_STATUS_LEN], bool estop, bool relay_energized,
                             bool heating_enabled, bool temp_valid, float safety_tc_c, float cj_c,
                             uint8_t tc_fault_bits, float amps1, float amps2, float amps3);

// --- Frame C: SAFETY_CMD_FW_VERSION (0x0B) -----------------------------------
// Also the reply to, and identical command byte as, SAFETY_CMD_GET_FW_VERSION
// -- LINK_PROTOCOL.md section 4/6 use 0x0B for both the (1-byte, no-args)
// request and this (multi-byte) reply, distinguished by direction and length,
// not by a different id.
#define LINK_FRAME_FW_VERSION_CMD 0x0Bu
#define LINK_FRAME_ANNOUNCE_VERSION_CMD 0x0Fu

// Packs a SAFETY_CMD_FW_VERSION payload into `out` (capacity `out_cap`).
// Returns the number of bytes written, or 0 if `out_cap` is too small for
// the requested commit_len/datetime_len (nothing is written in that case).
// `commit`/`datetime` are copied verbatim, ASCII, NOT null-terminated on the
// wire -- exactly `commit_len`/`datetime_len` bytes are read from each.
size_t link_frame_pack_fw_version(uint8_t *out, size_t out_cap, uint16_t protocol_version,
                                   uint16_t min_compatible, uint8_t dirty, const char *commit,
                                   uint8_t commit_len, const char *datetime, uint8_t datetime_len,
                                   uint8_t boot_id, uint8_t config_version, uint16_t config_crc);

// --- Frame B: SAFETY_CMD_DIAG (0x08), 26 bytes -------------------------------
// Was hand-packed here (link_frame_pack_diag()); ROADMAP.md M5 migrated
// link_task.c's send path onto the shared kilnlink_diag_encode() codec
// (firmware/CommonFW/include/kilnlink/kilnlink_diag.h, host-tested in
// CommonFW/test/test_diag.c) once that codec existed, removing this file's
// duplicate implementation of the identical 26-byte layout rather than
// leaving both around. If a pure/host-buildable DIAG packer is ever needed
// again independent of kilnlink, look there first before reintroducing one
// here.

// --- ESP -> Pico: SAFETY_CMD_PUSH_CONTEXT (0x07) -----------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Untrusted-wire input: every
// length and count is validated before any field is read, and nothing is
// written to `*out` unless the whole frame is well-formed (a caller must
// never merge a partially-unpacked snapshot with the previous one).
#define LINK_FRAME_PUSH_CONTEXT_CMD  0x07u
#define LINK_FRAME_CONTEXT_ZONE_LEN  14u
// Header only (offsets 0..14 inclusive), zone_count == 0.
#define LINK_FRAME_CONTEXT_MIN_LEN   15u

// Unpacks a SAFETY_CMD_PUSH_CONTEXT payload of `length` bytes (payload[0] is
// expected to already be LINK_FRAME_PUSH_CONTEXT_CMD -- callers dispatch on
// that before calling this). Returns true iff:
//   - length >= LINK_FRAME_CONTEXT_MIN_LEN,
//   - the header's zone_count (offset 14) is <= CONTEXT_SNAPSHOT_MAX_ZONES,
//   - length == LINK_FRAME_CONTEXT_MIN_LEN + zone_count * LINK_FRAME_CONTEXT_ZONE_LEN
//     exactly (no trailing garbage, no truncation).
// On success, `out->valid` is set true and every other field is populated;
// `out->timestamp_ms` is left untouched (the caller stamps local receive
// time, which this pure function -- deliberately RTOS/SDK-free, host-
// testable like the rest of link_frame.c -- has no way to know). On failure,
// `*out` is left completely unmodified.
bool link_frame_unpack_context(const uint8_t *payload, uint8_t length, context_snapshot_t *out);

// --- ESP -> Pico: SAFETY_CMD_CLEAR_TRIP (0x0A) -------------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_CLEAR_TRIP_CMD (kilnlink/kilnlink_clear_trip.h) -- redefined here
// as a local dispatch id, same convention as LINK_FRAME_PUSH_CONTEXT_CMD/
// LINK_FRAME_FW_VERSION_CMD above rather than pulling the kilnlink codec
// header into this file's own namespace. The payload itself is decoded by
// kilnlink_clear_trip_decode() in src/tasks/link_task.c, not unpacked here --
// it's a fixed 3-byte frame with no variable-length fields, unlike
// PUSH_CONTEXT, so there is no bespoke unpack helper to add in this file.
#define LINK_FRAME_CLEAR_TRIP_CMD 0x0Au

// --- Update frames: SAFTYFW Phase 10, CommonFW/docs/UPDATE_PROTOCOL.md
// section 4's frame table. Plain #define ids, same convention as every other
// command byte in this file -- these are dispatched in src/tasks/link_task.c's
// switch and handled in src/tasks/update_task.c (flash I/O does not belong on
// link_task's own priority/stack, see that file's header comment), not packed/
// unpacked here: UPDATE_BEGIN's payload is src/update/image_header.h's frozen
// 36-byte layout (that file's header comment explains why it, not this
// section's original wording, is the real source of truth for the field
// list), UPDATE_DATA/_END/_ABORT are simple enough to parse inline in
// update_task.c, and UPDATE_STATUS's wire layout is update_task.c's own
// invention (UPDATE_PROTOCOL.md section 4 names the frame but never specifies
// its payload) -- see that file's header comment for the chosen layout.
#define LINK_FRAME_UPDATE_BEGIN_CMD  0x10u
#define LINK_FRAME_UPDATE_DATA_CMD   0x11u
#define LINK_FRAME_UPDATE_END_CMD    0x12u
#define LINK_FRAME_UPDATE_ABORT_CMD  0x13u
#define LINK_FRAME_UPDATE_STATUS_CMD 0x14u

// --- Mutual version compatibility --------------------------------------------
// LINK_PROTOCOL.md section 4's exact formula, both directions:
//   peer.protocol >= self.min_compatible  &&  self.protocol >= peer.min_compatible
// Pure, no side effects, no notion of which side is "self" beyond the
// arguments passed -- callable identically from either firmware.
bool link_frame_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                     uint16_t peer_protocol, uint16_t peer_min_compatible);

// --- Single-bit trip_mask synthesis (Frame B / CLEAR_TRIP shared logic) -----
// Both SAFETY_CMD_DIAG's trip_mask field (link_task_send_diag()) and
// SAFETY_CMD_CLEAR_TRIP's mismatch check (link_task_handle_clear_trip())
// need "the wire trip_mask for the one reason this build currently tracks",
// so it is factored here once rather than duplicated -- see
// link_task_send_diag()'s own comment in link_task.c for why this is a
// documented single-bit degraded approximation of LINK_PROTOCOL.md's
// "one bit per guard" wording, not the real 13-bit mask. Returns 0 for
// SAFETY_TRIP_NONE, else 1 << (reason - 1) matching safety_trip_t's own
// numbering.
uint16_t link_frame_trip_mask_for_reason(safety_trip_t reason);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_FRAME_H
