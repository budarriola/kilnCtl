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
// CommonFW/docs/LINK_PROTOCOL.md section 6, "Frame B": additive -- a KilnFW
// build that has never heard of 0x08 discards it (LINK_PROTOCOL.md's own
// "a peer that has never heard of it discards it" rule), so this can ship
// without any ESP-side change.
#define LINK_FRAME_DIAG_CMD 0x08u
#define LINK_FRAME_DIAG_LEN 26u

// boot_reason byte (offset 10). bit2 (brownout) is always 0 in this build --
// boot_reason.c only latches watchdog_caused_reboot/watchdog_enable_caused_
// reboot (RP2040 SDK has no separate brownout-detect API this codebase reads
// from) -- see link_task.c's call site for how the other two bits are
// derived from what IS available.
#define LINK_DIAG_BOOT_POWERON  0x01u
#define LINK_DIAG_BOOT_WATCHDOG 0x02u
#define LINK_DIAG_BOOT_BROWNOUT 0x04u // never set in this build -- no source

// flags byte (offset 25).
#define LINK_DIAG_FLAG_SIM_CONTEXT_SEEN      0x01u // always 0 -- no context-frame parsing yet (Phase 7)
#define LINK_DIAG_FLAG_CALIBRATION_MISSING   0x02u // always 1 -- no config_store yet (Phase 9)
#define LINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT 0x04u // always 0 -- no detection heuristic specified/built

// Packs the 26-byte DIAG payload into `out` (must have room for
// LINK_FRAME_DIAG_LEN bytes). Pure passthrough of caller-supplied values,
// same discipline as link_frame_pack_status() -- this function does not
// decide what any field means, only how it is laid out on the wire.
// `trip_reason` is a plain uint8_t here (not safety_trip_t) so this header
// stays free of a safety_guards.h dependency, matching this whole file's
// "no non-host-buildable dependency" rule -- link_task.c does the cast at
// the call site.
void link_frame_pack_diag(uint8_t out[LINK_FRAME_DIAG_LEN], uint8_t trip_reason,
                           uint16_t warn_mask, uint16_t trip_mask, uint32_t uptime_ms,
                           uint8_t boot_reason, uint8_t context_age_100ms,
                           uint32_t context_frames_ok, uint32_t context_frames_bad,
                           uint32_t tx_frames_dropped, uint8_t state, uint8_t flags);

// --- Mutual version compatibility --------------------------------------------
// LINK_PROTOCOL.md section 4's exact formula, both directions:
//   peer.protocol >= self.min_compatible  &&  self.protocol >= peer.min_compatible
// Pure, no side effects, no notion of which side is "self" beyond the
// arguments passed -- callable identically from either firmware.
bool link_frame_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                     uint16_t peer_protocol, uint16_t peer_min_compatible);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_FRAME_H
