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
#include "../config_store.h" // config_store_record_t -- pure, host-testable (its own header
                              // comment), same as this file; link_frame_apply_set_config()/
                              // link_frame_apply_set_ct_cal() below need the full record shape

#ifdef __cplusplus
extern "C" {
#endif

// --- Frame A: SAFETY_CMD_GET_STATUS (0x01), 23 or 24 bytes ------------------
// Byte-for-byte the layout firmware/KilnFW/App/drivers/safety_link.h already
// parses -- see that file's header comment for the authoritative offsets.
#define LINK_FRAME_STATUS_CMD 0x01u

// V1 (original, bytes 0..22) and V2 (V1 + byte 23, tx_dropped_sat) lengths.
// 2026-08-23: V2 added to surface uart_owner's TX-ring drop counter on a
// channel that (unlike Frame B/DIAG, which carries the real, non-saturating
// count) is provably still arriving when DIAG itself has gone dark -- that
// was the entire point of putting it here rather than widening Frame B.
// LINK_FRAME_STATUS_LEN kept as an alias of the V1 length: every existing
// caller/test that names it wants "the original, always-valid prefix",
// which is still true of a V2 frame's first 23 bytes too.
#define LINK_FRAME_STATUS_LEN_V1 23u
#define LINK_FRAME_STATUS_LEN_V2 24u
// V3 (V2 + bytes 24/25, BORROWED status) -- 2026-09-03. Extends the frame
// with a NEW byte, following the V1->V2 precedent this header cites, rather
// than reassigning or overloading either of byte 1's two remaining bits
// (LINK_FLAG_TC_NOT_INSTALLED/LINK_FLAG_TC_INJECTED, already spent -- see
// their own comment below). Aliasing "borrowed reading" onto "TC not
// installed" would put two different safety-relevant conditions on one bit,
// defeating the point of a distinct flag. See link_frame_pack_status()'s own
// doc comment for the byte 24/25 layout and the same skew-safety argument the
// V1->V2 step made, applied here a second time.
#define LINK_FRAME_STATUS_LEN_V3 26u
#define LINK_FRAME_STATUS_LEN    LINK_FRAME_STATUS_LEN_V1

// Byte 23 (V2 only): uart_owner_get_tx_dropped(), saturating -- 254 is the
// largest real count this byte can carry, 255 means "254 or more", same
// sentinel discipline kilnlink_diag.h's KILNLINK_DIAG_CONTEXT_AGE_NEVER (255)
// already uses. Not the same information as Frame B's tx_frames_dropped
// (that one is a real, non-saturating uint32_t) -- this is a coarser
// same-counter view chosen specifically to fit in the one spare byte a V2
// Frame A has room for, and to be readable over a channel Frame B's own
// numbers cannot describe themselves through while THEY are the ones going
// missing.
#define LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX 254u

// Saturates a real (uint32_t) drop count into byte 23's wire representation.
// Pure -- host-tested (test_link_frame_wire.c) same as every other function
// in this file.
uint8_t link_frame_saturate_tx_dropped(uint32_t tx_dropped);

// Peer-protocol_version floor a receiver must have announced (via
// ANNOUNCE_VERSION, kilnlink_version.h's KILNLINK_PROTOCOL_VERSION field)
// before link_task_send_status() ever emits a V2 (24-byte) frame at it --
// see link_frame_pack_status()'s own doc comment below for the full
// skew-safety argument this constant is the linchpin of.
#define LINK_FRAME_STATUS_V2_MIN_PROTOCOL 6u

// Pure wrapper around the comparison above -- named and host-tested
// (test_link_frame_wire.c) same as link_frame_versions_compatible() just
// below is for its own, larger compatibility question, so the ">=" itself,
// and its boundary, is pinned down by a test rather than left as an inline
// comparison only ever exercised indirectly through link_task_send_status().
bool link_frame_status_v2_supported(uint16_t peer_protocol_version);

// Same gate, mirrored for the V3 (26-byte, BORROWED) status frame extension
// below -- link_task_send_status() must not emit byte 24/25 at a peer that
// has not positively confirmed protocol_version >= this floor, exactly the
// same skew-safety reasoning LINK_FRAME_STATUS_V2_MIN_PROTOCOL's own comment
// gives for byte 23.
#define LINK_FRAME_STATUS_V3_MIN_PROTOCOL 10u
bool link_frame_status_v3_supported(uint16_t peer_protocol_version);

// Sentinel for byte 25 (borrowed_zone_index) meaning "not commissioned /
// unknown" -- config_store.h's borrowed_zone_index is only ever 0..2
// (CONFIG_STORE_SET_BORROWED_ZONE_INDEX gates whether it has been
// commissioned at all, config_params.c's CHECK_U8_MAX(2u)), so 0xFF can never
// collide with a real value. Mirrors LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX's
// "255 is never a real saturating count either" sentinel discipline.
#define LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN 0xFFu

// Same pure peer-version gate, mirrored for SAFETY_CMD_ROLLBACK_RESULT
// (0x25, kilnlink_rollback_result.h) instead of the V2 status field --
// link_task_handle_rollback() uses this to decide whether it may EVER emit
// a ROLLBACK_RESULT frame at the current peer, exactly the same "only after
// this boot's own ANNOUNCE_VERSION has positively named a peer new enough"
// discipline link_frame_status_v2_supported() above enforces.
//
// LINK_FRAME_ROLLBACK_RESULT_MIN_PROTOCOL duplicates the numeric value of
// kilnlink_rollback_result.h's KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL rather
// than including that header -- same "deliberately free of ... kilnlink"
// discipline this file's own top comment states, and the same duplication
// LINK_FRAME_STATUS_V2_MIN_PROTOCOL (6u) above already makes for
// KILNLINK_PROTOCOL_VERSION's 5->6 bump. If this ever drifts from
// kilnlink_version.h's 8->9 history entry, that is a real bug -- there is
// no compile-time link between the two, only this comment and the mirrored
// value.
#define LINK_FRAME_ROLLBACK_RESULT_MIN_PROTOCOL 9u
bool link_frame_rollback_result_supported(uint16_t peer_protocol_version);

// flags byte (offset 1): bits 0/1 (LINK_UP, FAULT) are the ESP's to own --
// this module never sets them, they simply are not parameters below.
#define LINK_FLAG_ESTOP      0x04u
#define LINK_FLAG_RELAY      0x08u
#define LINK_FLAG_ENABLED    0x10u
#define LINK_FLAG_TEMP_VALID 0x20u
// Bits 6/7 -- added for the "safety TC not physically installed" declared
// state (config param 0x0211, SAFETY_MODEL.md section 4 S5) and its bench
// TC-injection dev switch (thermo_task.h). Additive: the frame stayed
// LINK_FRAME_STATUS_LEN_V1 (23) bytes when these landed, only the flags byte
// gained two previously-unused bits -- an ESP build that predates these two
// bits simply never sets/reads them and keeps working exactly as before
// (bits 0/1 already establish that "some bits are reserved and unused by
// this side" is a normal, already-relied-upon state for this byte). Both
// bits are now spent -- byte 1 has no room left, which is exactly why the
// tx_dropped_sat addition above needed a whole new byte (23) rather than a
// ninth flag bit.
#define LINK_FLAG_TC_NOT_INSTALLED 0x40u /* safety_tc_installed == 0 -- heat is refused (safety_core_request_enable()) */
#define LINK_FLAG_TC_INJECTED      0x80u /* thermo_task_injection_active() -- reading is synthetic, not from the part */

// flags2 byte (offset 24, V3 only) -- 2026-09-03. Byte 1 has no room left
// (comment above), so this is a NEW byte (V1->V2 precedent), not a ninth bit
// squeezed somewhere. Only bit 0 defined so far; bits 1-7 are reserved for
// future flags rather than this byte being sized for exactly one bit.
#define LINK_FLAG2_BORROWED 0x01u /* tc_source is BORROWED_ZONE or BOTH -- this reading is (partly) sourced
                                    * from another zone's probe, not this board's own J7 input
                                    * (SAFETY_MODEL.md sec 3, THERMOCOUPLE.md's tc_source table). */

// Packs the status payload into `out` (must have room for
// LINK_FRAME_STATUS_LEN_V2 bytes, whether or not this call ends up using all
// of them). `safety_tc_c`/`cj_c` should already be NaN when `temp_valid` is
// false -- this function passes them through unchanged rather than
// substituting 0, matching LINK_PROTOCOL.md's "send NaN, never 0" rule; it
// does not itself decide validity. `tc_not_installed`/`tc_injected` set
// LINK_FLAG_TC_NOT_INSTALLED/LINK_FLAG_TC_INJECTED above, independent of
// temp_valid -- a declared-absent sensor still reports temp_valid accurately
// (false while blind, per S5's existing contract); these two bits are
// additional context, not a replacement for it.
//
// `peer_supports_status_v2`/`tx_dropped_sat` control byte 23 (2026-08-23,
// the DIAG-frame-went-dark investigation). Returns LINK_FRAME_STATUS_LEN_V2
// (24) and writes tx_dropped_sat into byte 23 iff `peer_supports_status_v2`
// is true; otherwise returns LINK_FRAME_STATUS_LEN_V1 (23) and touches
// nothing past byte 22 -- the caller must send back exactly the returned
// length, not a fixed constant, or a V1-peer receiver's exact-length check
// will reject the frame outright.
//
// `peer_supports_status_v3`/`is_borrowed`/`borrowed_zone_index` control bytes
// 24/25 (2026-09-03, the BORROWED status flag). Only ever considered when
// `peer_supports_status_v2` is ALSO true -- byte 24/25 sit right after byte
// 23, so a peer this function has not confirmed can even receive a V2 frame
// must not be handed a V3 one either (LINK_FRAME_STATUS_V3_MIN_PROTOCOL (10)
// is numerically above LINK_FRAME_STATUS_V2_MIN_PROTOCOL (6), so any peer
// that has announced itself new enough for V3 has necessarily also announced
// itself new enough for V2, but this function does not assume the caller
// enforces that ordering -- it enforces it itself). When both gates pass,
// returns LINK_FRAME_STATUS_LEN_V3 (26): byte 24 is a flags2 byte (bit 0
// LINK_FLAG2_BORROWED, set iff `is_borrowed`) and byte 25 is
// `borrowed_zone_index` verbatim (pass LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN
// when the caller does not know it, e.g. borrowed_zone_index is not
// commissioned -- see link_task.c's call site for how it decides this).
// `is_borrowed` derives from `tc_source` (config_store.h): true iff tc_source
// is CONFIG_STORE_TC_SOURCE_BORROWED_ZONE or _BOTH, i.e. the safety
// processor's own reported reading is (at least partly) another zone's
// probe, not this board's own J7 input -- this is the exact condition guard
// S13 polices and that an operator seeing an unlabelled reading would
// otherwise have no way to tell apart from an honest own-sensor reading.
//
// SKEW SAFETY, both directions -- this is why the length is chosen HERE, by
// the sender, rather than the receiver simply tolerating either length
// unconditionally:
//   - Newer ESP (accepts both 23 and 24) + older Pico (never calls this with
//     peer_supports_status_v2 true, or predates the parameter entirely):
//     sends 23. The newer ESP's receiver already accepts 23 explicitly (see
//     safety_link.c's updated safety_apply_status()) -- no regression,
//     tx_dropped simply reads as "not reported" on that ESP.
//   - Newer Pico + older ESP (fixed, already-compiled exact msg->length !=
//     23 check -- cannot be changed after the fact; this is not a case
//     "handling" can fix, only avoiding can): if this function were called
//     with peer_supports_status_v2 unconditionally true, it would send 24
//     bytes at an ESP that rejects anything but exactly 23, and Frame A
//     itself -- the one channel PROVEN to still work while DIAG is dark --
//     would go dark too. That is why the caller (link_task_send_status())
//     is REQUIRED to gate peer_supports_status_v2 on having positively
//     received an ANNOUNCE_VERSION from this peer with protocol_version >=
//     LINK_FRAME_STATUS_V2_MIN_PROTOCOL (6) -- see link_task.c's
//     s_peer_protocol_version. Before any ANNOUNCE_VERSION has ever arrived
//     (a fresh boot, or a peer that predates ANNOUNCE_VERSION entirely) the
//     safe default is false: send the 23-byte frame every old receiver
//     already accepts, and only grow to 24 once the peer has proven it can
//     take it. A newer ESP + older Pico, and a newer Pico + an ESP it has
//     not yet confirmed as new enough, are the SAME safe state (23 bytes)
//     from this function's point of view -- the asymmetry is deliberately
//     all on "do I know the peer is new", never on which side has which
//     build.
size_t link_frame_pack_status(uint8_t out[LINK_FRAME_STATUS_LEN_V3], bool estop, bool relay_energized,
                               bool heating_enabled, bool temp_valid, float safety_tc_c, float cj_c,
                               uint8_t tc_fault_bits, float amps1, float amps2, float amps3,
                               bool tc_not_installed, bool tc_injected,
                               bool peer_supports_status_v2, uint8_t tx_dropped_sat,
                               bool peer_supports_status_v3, bool is_borrowed,
                               uint8_t borrowed_zone_index);

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

// --- ESP -> Pico: SAFETY_CMD_SET_CONFIG (0x16) -------------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_SET_CONFIG_CMD (kilnlink/kilnlink_set_config.h) -- redefined here
// as a local dispatch id, same convention as LINK_FRAME_CLEAR_TRIP_CMD above.
// The payload is decoded by kilnlink_set_config_decode() in
// src/tasks/link_task.c, not unpacked here -- a fixed 2-byte frame with no
// variable-length fields, same reasoning as CLEAR_TRIP.
#define LINK_FRAME_SET_CONFIG_CMD 0x16u

// Builds the record link_task_handle_set_config() should write, by
// read-modify-write against `committed` (the currently-committed record,
// e.g. from config_store_get_full_record()) -- extracted so the fix for a
// real bug can be host-tested. The ORIGINAL handler built `*out` from
// config_store_default() and set only tc_type/calibration_missing on top,
// then handed the whole thing to config_store_write() (which replaces the
// ENTIRE stored record, no merge -- config_store.h's own doc comment). That
// meant every SET_CONFIG silently factory-reset every OTHER commissioned
// field: every fields_set bit, every section 2-5 threshold, all three
// ct_cal channels, and safety_tc_installed back to its "installed" default.
// Worse, it fires unattended: KilnFW's safety_link.c resends SET_CONFIG
// automatically on every link down->up transition (tc_type_last_sent =
// 0xFF forces a resync), so on a board that was ever commissioned, an
// ordinary link reconnect -- routine on this project, not hypothetical --
// silently wiped the safety processor's tuned limits back to compiled
// defaults with no operator action and no wire-visible warning beyond
// calibration_missing flipping true and config_crc changing.
//
// `out` is `*committed` with ONLY tc_type mutated. calibration_missing is
// re-armed ONLY when tc_type actually differs from committed->tc_type --
// config_store.h's doc comment on calibration_missing ("a new tc_type
// invalidates any prior calibration") is still honored for a REAL change,
// but an idempotent resend of the value already committed (exactly what
// every automatic reconnect-resend is, when nobody re-commissioned in
// between) must not distrust calibration that a real type change never
// touched -- treating every resend as "the type changed" would keep
// re-breaking commissioning on every reconnect even after this fix, just
// through a narrower door (calibration_missing flipping instead of the
// whole record resetting) instead of a wide one.
//
// SAFETY_CMD_SET_CONFIG (0x16) itself is a leftover from before the v2
// staged-commissioning model (config_store.h's own header comment; docs/
// COMMISSIONING.md documents only SET_PARAM/COMMIT_CONFIG, never 0x16) --
// this function fixes its one real caller's shape, it does not endorse a
// single wire command that rewrites a whole record as the right design
// going forward. See docs/CONFIG_REFERENCE.md / docs/COMMISSIONING.md for
// the recommendation on 0x16's future.
void link_frame_apply_set_config(const config_store_record_t *committed, uint8_t tc_type,
                                  config_store_record_t *out);

// --- ESP -> Pico: SAFETY_CMD_ROLLBACK (0x17) ---------------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_ROLLBACK_CMD (kilnlink/kilnlink_rollback.h) -- redefined here as a
// local dispatch id, same convention as LINK_FRAME_CLEAR_TRIP_CMD/
// LINK_FRAME_SET_CONFIG_CMD above. The payload is decoded by
// kilnlink_rollback_decode() in src/tasks/link_task.c, not unpacked here --
// a fixed 1-byte (cmd only, no fields) frame, same reasoning as CLEAR_TRIP/
// SET_CONFIG. tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico
// half: reverts to the previously-running bootloader slot, refused unless
// that slot is currently VALID/PENDING_VERIFY (bootloader/metadata.c's
// bootloader_decide_rollback()) and refused while ARMED (same gate
// SAFETY_CMD_SET_CONFIG uses) -- see link_task_handle_rollback().
#define LINK_FRAME_ROLLBACK_CMD 0x17u

// --- ESP -> Pico: SAFETY_CMD_ANNOUNCE_REBOOT (0x18) --------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_ANNOUNCE_REBOOT_CMD (kilnlink/kilnlink_announce_reboot.h) --
// redefined here as a local dispatch id, same convention as
// LINK_FRAME_ROLLBACK_CMD above. The payload is decoded by
// kilnlink_announce_reboot_decode() in src/tasks/link_task.c, not unpacked
// here -- a fixed 1-byte (cmd only, no fields) frame, same reasoning as
// ROLLBACK. KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP
// reboots" line: an unsolicited courtesy notice sent immediately before an
// OTA self-update's esp_restart(), recorded by link_task_handle_announce_
// reboot() as a timestamp that safety_core.c reads to compute a bounded
// "within the post-announce grace window" fact for safety_guards.c's S6b
// block (SAFETY_MODEL.md section 4) -- suppresses ONLY S6b's own trip
// condition, never link_up itself, never any other guard, and never grants
// or extends heating permission. If the window expires with the link still
// down, S6b trips exactly as if this frame had never arrived.
#define LINK_FRAME_ANNOUNCE_REBOOT_CMD 0x18u

// --- ESP -> Pico: SAFETY_CMD_SET_CT_CAL (0x19) -------------------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_SET_CT_CAL_CMD (kilnlink/kilnlink_set_ct_cal.h) -- redefined here
// as a local dispatch id, same convention as LINK_FRAME_SET_CONFIG_CMD
// above. The payload is decoded by kilnlink_set_ct_cal_decode() in
// src/tasks/link_task.c, not unpacked here -- a fixed 11-byte frame with no
// variable-length fields, same reasoning as SET_CONFIG. Sets one channel's
// CT amps calibration (config_store.h's ct_cal) -- firmware/SimFW/tools/
// ct_calibration/'s bench sweep-and-fit tool's PC-side path to actually
// pushing constants into SaftyFW's own flash, TODO.md's documented gap.
#define LINK_FRAME_SET_CT_CAL_CMD 0x19u

// Builds the record link_task_handle_set_ct_cal() should write, same
// read-modify-write fix as link_frame_apply_set_config() above, applied to
// the milder existing form of the same bug in that handler: it already
// preserved tc_type/calibration_missing/ct_cal via the live getters before
// this fix, but still started from config_store_default() underneath that,
// so every section 1-5 threshold and every fields_set bit was still wiped
// on every SET_CT_CAL. `out` is `*committed` with ONLY ct_cal[channel]
// mutated (calibrated/gain/offset); tc_type, calibration_missing, and every
// other field -- including the other two ct_cal channels -- pass through
// byte-for-byte. No channel bounds check here (link_task.c's own
// `channel >= CONFIG_STORE_CT_CAL_NUM_CHANNELS` refusal happens before this
// is ever called); `channel` past range makes this a no-op (`*out` left as
// `*committed`) rather than an out-of-bounds write, as a second line of
// defense.
void link_frame_apply_set_ct_cal(const config_store_record_t *committed, uint8_t channel,
                                  bool calibrated, float gain, float offset,
                                  config_store_record_t *out);

// --- ESP -> Pico: SAFETY_CMD_GET_CT_CAL (0x22), request only ----------------
// CommonFW/docs/LINK_PROTOCOL.md section 4/6. Same value as
// KILNLINK_GET_CT_CAL_CMD (kilnlink/kilnlink_get_ct_cal.h) -- redefined here
// as a local dispatch id.
//
// Was 0x1A, sharing that wire id with its own reply (kilnlink_ct_cal.h's
// SAFETY_CMD_CT_CAL, link_task_send_ct_cal()), distinguished only by
// direction and length. Moved to its own id (KILNLINK_PROTOCOL_VERSION
// 6 -> 7) because that scheme structurally blocks a length-different
// refusal reply -- see kilnlink_get_ct_cal.h's header comment.
// 0x1A is now used ONLY by the reply.
#define LINK_FRAME_GET_CT_CAL_CMD 0x22u

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

// --- CLEAR_TRIP validation (link_task_handle_clear_trip() shared logic) ----
// Factored out of link_task_handle_clear_trip() (src/tasks/link_task.c) so
// this session's guard-test-matrix pass (commit 9a6d3e9) can host-test the
// refuse-if-nothing-tripped / refuse-on-mask-mismatch decision added in
// 62ce6bf, the same way link_frame_trip_mask_for_reason() was already
// extracted for DIAG/CLEAR_TRIP's shared trip_mask math. This function only
// covers link_task's OWN validation, checked before safety_core is even
// asked -- it does not re-implement safety_core_request_clear_trip()'s
// separate "refused while the tripping condition still holds" retick logic,
// which is a different check, already host-tested where it lives.
typedef enum {
    LINK_CLEAR_TRIP_ACCEPT = 0,             // proceed to safety_core_request_clear_trip()
    LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED, // current_trip_reason == SAFETY_TRIP_NONE
    LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH,   // wire_trip_mask doesn't match the latched reason's mask
    LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE,     // current_trip_reason == SAFETY_TRIP_INEFFECTIVE (S9) --
                                             // unclearable, see this function's own comment
} link_clear_trip_decision_t;

// `current_trip_reason` is read fresh from safety_core_get_diag_status()
// immediately before this call (link_task.c, same as before extraction);
// `wire_trip_mask` is the CLEAR_TRIP frame's own trip_mask field, already
// decoded by kilnlink_clear_trip_decode(). The "nothing tripped" check is
// deliberately evaluated first and separately from the mask comparison --
// LINK_PROTOCOL.md documents both as refusals, but with SAFETY_TRIP_NONE the
// mask math below would also fail to match (a mask of 0 can't equal any real
// trip's mask by construction, see link_frame_trip_mask_for_reason()), so this
// preserves the original code's distinct log line rather than merging the two
// into one bucket by chance of the math working out.
//
// Audit 2026-08-27: SAFETY_TRIP_INEFFECTIVE (S9, welded contactor) is checked
// before the mask-mismatch comparison, for the same reason NOTHING_TRIPPED is
// checked before it -- without this, a wire_trip_mask that happens to match
// S9's mask would fall through to LINK_CLEAR_TRIP_ACCEPT and reach
// safety_core_request_clear_trip() at all, relying entirely on
// safety_guards_try_clear()'s own unconditional S9 refusal (safety_guards.c)
// to fail it silently deeper in the stack. ARCHITECTURE.md section 9 and
// SAFETY_MODEL.md section 4 both call S9 "no exit except power removal at the
// breaker" -- that has to be visible here, at the wire layer, with its own
// reason code the ESP can log and surface to the operator, not just a refusal
// that looks identical to any other still-tripped guard.
link_clear_trip_decision_t link_frame_decide_clear_trip(safety_trip_t current_trip_reason,
                                                          uint16_t wire_trip_mask);

// --- ESP -> Pico: SAFETY_CMD_SET_FIRING_CEILING (0x09) -----------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as KILNLINK_CEILING_CMD
// (kilnlink/kilnlink_ceiling.h) -- redefined here as a local dispatch id, same
// convention as LINK_FRAME_CLEAR_TRIP_CMD above. The payload is decoded by
// kilnlink_ceiling_decode() in src/tasks/link_task.c, not unpacked here -- a
// fixed 5-byte frame with no variable-length fields, same reasoning as
// CLEAR_TRIP/SET_CONFIG.
#define LINK_FRAME_SET_FIRING_CEILING_CMD 0x09u

// Bounds check factored out of link_task_handle_set_firing_ceiling() so it is
// host-testable, same "extraction for the test matrix" reasoning as
// link_frame_decide_clear_trip() above. LINK_PROTOCOL.md/SAFETY_MODEL.md
// section 4, S1: "0 or NaN = no firing / no ceiling known", and the ceiling
// "can only ever tighten" S1 via safety_guards.c's own min() clamp -- but that
// clamp only defends against a firing_max_c that is too HIGH (isfinite() &&
// too-large collapses to abs_max_temp_c). A finite but NEGATIVE firing_max_c
// is not caught by that clamp at all: min(abs_max_temp_c, negative + 100) can
// come out far BELOW abs_max_temp_c, silently tightening S1 into constant
// nuisance trips from a single garbled or hostile byte. That is the "reject,
// don't clamp" case this function exists for: only a strictly positive,
// finite value is ever treated as an active ceiling; zero, negative, NaN, and
// +/-Infinity all fall back to "no ceiling" (equivalent to firing_max_valid ==
// false, i.e. S1 uses abs_max_temp_c alone), never a half-accepted number.
bool link_frame_ceiling_is_active(float firing_max_c);

// NOTE: the "should an already-active ceiling actually be applied to S1's cfg
// this tick" gate is NOT here. That combinator needs `context_valid`, which
// only safety_core.c computes, and safety_core.c is structurally forbidden
// from #include-ing any header whose name contains "link" or "uart"
// (docs/ARCHITECTURE.md section 2, tools/check_isolation.ps1) -- this header
// is named link_frame.h precisely because it IS link-shaped. See
// snapshots.h's link_firing_ceiling_should_apply() instead, the same
// isolation-legal home context_reduce_zones()/current_any_present() already
// use for pure helpers safety_core.c needs but link_frame.h cannot host.
//
// --- ESP -> Pico: SAFETY_CMD_SET_CLOCK (0x0C), optional ---------------------
// CommonFW/docs/LINK_PROTOCOL.md section 4. Same value as
// KILNLINK_SET_CLOCK_CMD (kilnlink/kilnlink_set_clock.h) -- redefined here as a
// local dispatch id, same convention as LINK_FRAME_SET_FIRING_CEILING_CMD
// above. Purely diagnostic (the Pico has no RTC): "no guard may ever read this
// clock" is the protocol doc's own words, and nothing added by this pass
// changes that -- see link_task_handle_set_clock()'s own comment.
#define LINK_FRAME_SET_CLOCK_CMD 0x0Cu

// Plausibility check factored out for host testing, same reasoning as
// link_frame_ceiling_is_active() above: an implausible epoch (0, or absurdly
// far from "now" in either direction) is rejected outright rather than stored
// and later confusing a trip-log correlation with a bogus wall-clock time.
// Bounds are generous software constants (2020-01-01 .. 2100-01-01 in Unix
// milliseconds), not measured physical values -- this field is diagnostic
// only, so "wide enough that no legitimate ESP clock is ever rejected" is the
// only property that matters, not precision.
bool link_frame_clock_epoch_is_plausible(uint64_t epoch_ms);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_FRAME_H
