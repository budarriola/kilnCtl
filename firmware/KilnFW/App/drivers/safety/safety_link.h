// Isolated link (ADuM1201WT digital isolator, U6) to the RP2040 safety
// processor (A1).
//
// ===========================================================================
// THIS IS THE CONTRACT THE PICO MUST IMPLEMENT.
// ===========================================================================
// The RP2040 firmware does not exist in this repository yet. Everything below
// is the wire behaviour this driver already speaks, written down so that
// firmware has something concrete to answer. Until it exists the link comes
// up, every poll times out, and safety_link_get_status() reports link_up = 0
// with age_ms = SAFETY_LINK_AGE_NEVER. That is the designed-for state, not an
// error: nothing here aborts startup because the far side is silent.
//
// Transport
//   ESP32-S3 UART1, 8N1, CONFIG_KILNCTL_SAFETY_BAUD_RATE (230400 by default),
//   carrying the *same* uart_protocol framing as the PC link (0x7E-delimited,
//   byte-stuffed, CRC16/CCITT-FALSE, indexed DATA frames with ACK/NACK). The
//   ESP identifies itself as UART_PROTO_DEVICE_ESP; the Pico must identify
//   itself as UART_PROTO_DEVICE_SAFETY (= 2). Both ends register task_id
//   UART_TASK_ID_SAFETY (= 7); everything below is that task's payload.
//
//   Neither direction is inverted any more. Until 2026-08-25 this link ran
//   through a TCMT1109 optocoupler pair (U2/U3, with R7/R12/R15) which
//   electrically inverted both directions, and this side cancelled that with
//   uart_set_line_inverse(TXD_INV | RXD_INV) -- see the "Isolation barrier"
//   section of docs/HARDWARE.md and safety_link.c. That pair was replaced by
//   U6, an ADuM1201WT digital isolator, which is NON-inverting, so neither
//   end applies TXD_INV/RXD_INV any more: applying either now would invert
//   an already-correct signal and break the link. The RP2040's plain
//   hardware UART works unmodified -- no PIO UART, no external inverter,
//   and no software inversion on either side.
//
// ESP -> Pico requests (payload byte0 = subcommand, from uart_task_ids.h)
//   0x01 SAFETY_CMD_GET_STATUS      no args. Answer with the status frame
//                                   below, addressed back to
//                                   (UART_PROTO_DEVICE_ESP, task 7).
//   0x02 SAFETY_CMD_REQUEST_ENABLE  byte1 = enable (0/1). Advisory: the Pico
//                                   may refuse, and its own interlocks always
//                                   win. A protocol-level ACK is the whole
//                                   reply -- there is no per-request outcome
//                                   frame. CORRECTION 2026-08-27 (this
//                                   comment used to claim "the ESP learns the
//                                   outcome from SAFETY_FLAG_ENABLED on the
//                                   next status", which is false and shipped
//                                   a real bug in danger_mode.c's
//                                   diagnostics-page tile: SAFETY_FLAG_ENABLED
//                                   means "relay_owner is ARMED / not
//                                   tripped" (see byte1 bit4 below), true on
//                                   any healthy Pico regardless of any
//                                   REQUEST_ENABLE ever sent. The only two
//                                   real outcome signals are SAFETY_FLAG_RELAY
//                                   (did K4 actually close) and, ESP-side,
//                                   whatever this driver's own caller chose
//                                   to remember it asked for (e.g.
//                                   danger_mode_get_heat_requested()).
//                                   A status frame pushed unsolicited right
//                                   after is accepted and refreshes the cache.
//   There is no separate PING on the wire: safety_link_ping() sends a
//   GET_STATUS immediately instead of waiting for the next poll tick. The
//   remaining SAFETY_CMD_* values (PING, GET_LINK_STATS, SET_POLL_PERIOD,
//   SET_FAULT_OUT) are PC->ESP only and are never sent across the barrier.
//
// Pico -> ESP status frame (the reply to GET_STATUS; may also be pushed
// unsolicited at any time -- this driver accepts either), 23 bytes:
//   byte0       = SAFETY_CMD_GET_STATUS (0x01)
//   byte1       = flags, SAFETY_FLAG_* from uart_task_ids.h:
//                   bit0 SAFETY_FLAG_LINK_UP    -- MUST be 0, ESP-owned
//                   bit1 SAFETY_FLAG_FAULT      -- MUST be 0, ESP-owned
//                   bit2 SAFETY_FLAG_ESTOP      -- estop input asserted
//                   bit3 SAFETY_FLAG_RELAY      -- safety relay K4 energized
//                   bit4 SAFETY_FLAG_ENABLED    -- relay_owner is ARMED / not
//                                                  tripped (true on any
//                                                  healthy Pico regardless of
//                                                  any REQUEST_ENABLE ever
//                                                  sent -- NOT "a heat
//                                                  request was granted")
//                   bit5 SAFETY_FLAG_TEMP_VALID -- the two temperatures below
//                                                  are real readings
//   bytes2..5   = safety thermocouple temperature, f32 LE, degC
//   bytes6..9   = safety cold-junction temperature, f32 LE, degC
//   byte10      = safety thermocouple fault status (THERMO_FAULT_* bits)
//   bytes11..14 = current sense 1, f32 LE, amps
//   bytes15..18 = current sense 2, f32 LE, amps
//   bytes19..22 = current sense 3, f32 LE, amps
// Bits 0 and 1 are cleared by this driver on receipt whatever the Pico sends,
// because they describe the *ESP's* view of the link and of the fault line it
// is itself driving -- a peer that reported them would only be reporting them
// back at us. When SAFETY_FLAG_TEMP_VALID is clear, send NaN for the two
// temperatures rather than 0: an explicit not-a-number is much harder to
// mistake for a cold kiln than a plausible-looking zero.
//
// The age field the PC sees (bytes23..24 of the GET_STATUS response in
// uart_task_ids.h) is *not* on this wire -- it is measured by the ESP, from
// when it received the frame. The Pico has no clock the ESP trusts.
//
// The isolated fault line (GPIO6) is a separate, out-of-band ESP output: high
// lights U1's LED and pulls the Pico's mainFault input LOW. It is not part of
// this protocol and keeps working with the UART completely dead, which is the
// entire point of it being a wire and not a message.
//
// Phase 7b -- mutual version compatibility (CommonFW/docs/LINK_PROTOCOL.md
// sec 4/6), the newer, BROADCAST-carried half of this contract, layered on
// top of everything above rather than replacing it:
//   0x0F SAFETY_CMD_ANNOUNCE_VERSION (ESP -> Pico, BROADCAST, unrequested)
//                                   Sent at ESP boot (a few repeats against
//                                   loss) and again whenever a Pico
//                                   FW_VERSION frame reports a new boot_id.
//                                   Same byte layout as FW_VERSION below,
//                                   truncated at boot_id (no config_version/
//                                   config_crc -- the ESP has none of its
//                                   own to report). Built by
//                                   safety_build_announce_version_payload().
//   0x0B SAFETY_CMD_FW_VERSION      (Pico -> ESP, BROADCAST, unsolicited at
//                                   Pico boot and on request). safety_poll_task()
//                                   sends the 0x0B request every poll period for
//                                   as long as peer_version_known stays false
//                                   (ROADMAP.md M6 "boot-time version request
//                                   with retry" -- closed 2026-08-19; a Pico
//                                   already running before this ESP boot has no
//                                   reason to volunteer FW_VERSION on its own,
//                                   so without this request its version would
//                                   never be learned). Reply bytes1..2 protocol,
//                                   bytes3..4 min_compatible (read before
//                                   anything else, per the wire spec's
//                                   floor rule), then dirty/commit/datetime/
//                                   boot_id. safety_apply_fw_version()
//                                   updates the tracked compatibility
//                                   verdict and re-announces on a boot_id
//                                   change.
// A version mismatch (peer_version_known && !peer_version_compatible) is
// folded into the *same* SAFETY_FAULT_SRC_SAFETY_LINK bit link staleness
// already uses -- LINK_PROTOCOL.md sec 4: "The ESP treats it exactly like a
// dead link." See safety_update_health() in safety_link.c.
//
// ROADMAP.md M5 -- SAFETY_CMD_PUSH_CONTEXT (ESP -> Pico, BROADCAST,
// unrequested, every poll period, LINK_PROTOCOL.md sec 4's 57-byte-at-3-zones
// layout): relay_now_mask/relay_recent_mask from kiln_io_get_relay_shadow()
// (context_io, set via safety_link_set_context_sources()), per-zone raw
// MAX31856 readings + configured tc_type from context_thermo_bus, and
// per-zone setpoint/active/relay-on/guard-tripped from
// profile_executor_get_status() (a free accessor, no pointer needed here).
// Built and sent by safety_build_and_send_context() from the poll task, right
// alongside the existing GET_STATUS exchange -- see that function in
// safety_link.c for the exact source of every field, including
// sample_counter's "increment only when a fresh conversion was actually
// read" rule and relay_recent_mask's rolling-window bookkeeping.
#ifndef SAFETY_LINK_H
#define SAFETY_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_ct_auto_zero_status.h"
#include "kilnlink/kilnlink_stack_margin.h"
#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_param_value.h"
#include "kilnlink/kilnlink_frame_a_offsets.h" /* KILNLINK_FRAME_A_* -- single source of truth for
                                                 * Frame A's byte offsets/lengths, ROADMAP.md M15
                                                 * "Frame A's field layout is hand-duplicated" */
#include "uart_owner.h"
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_io_t is an anonymous-struct typedef (kiln_io.h) and MAX31856BusClass
 * is a named one (MAX31856.h) -- rather than pull either header in here
 * (safety_link.h stays free of that hard dependency, same minimal-include
 * philosophy relay_authority.h documents), SafetyLinkClass below stores them
 * as void* and safety_link.c, which already needs both headers to build
 * ROADMAP.md M5's SAFETY_CMD_PUSH_CONTEXT frame (0x07, LINK_PROTOCOL.md sec
 * 4), casts back. safety_link_set_context_sources() is the only setter. */

/* Length of the Pico's status frame (see the contract above). V1 (original,
 * bytes 0..22) and V2 (V1 + byte 23, tx_dropped_sat -- uart_owner's TX-ring
 * drop counter, saturating, 255 = "254 or more") -- 2026-08-23, the
 * DIAG-frame-went-dark investigation: this byte was added specifically
 * because it needed to be readable on a channel PROVEN to still arrive when
 * Frame B (DIAG) itself has gone dark, which ruled out widening DIAG
 * instead. safety_apply_status() (safety_link.c) accepts EITHER length --
 * an older Pico sending 23 and a newer one sending 24 must both keep
 * working, in both directions of independent flashing (see
 * firmware/SaftyFW/src/tasks/link_frame.h's link_frame_pack_status() doc
 * comment for the full skew-safety argument the Pico side observes to make
 * that promise: it only ever sends 24 once it has positively confirmed,
 * via ANNOUNCE_VERSION, that the ESP peer is built against protocol_version
 * >= 6). SAFETY_LINK_STATUS_FRAME_LEN kept as an alias of the V1 length --
 * every existing reference wants "the original, always-valid prefix". */
#define SAFETY_LINK_STATUS_FRAME_LEN_V1 KILNLINK_FRAME_A_LEN_V1
#define SAFETY_LINK_STATUS_FRAME_LEN_V2 KILNLINK_FRAME_A_LEN_V2
/* V3 (V2 + bytes 24/25, the BORROWED status flag -- 2026-09-03). Byte 24 is a
 * flags2 byte (bit0 SAFETY_LINK_STATUS_FLAG2_BORROWED), byte 25 is
 * borrowed_zone_index verbatim (SAFETY_LINK_BORROWED_ZONE_UNKNOWN (0xFF) =
 * not commissioned / not known). Same skew-safety contract as V1/V2:
 * safety_apply_status() accepts V1, V2, OR V3 -- an older Pico (23 or 24
 * bytes) and a newer one (26 bytes) must both keep working regardless of
 * which side is flashed first. SaftyFW only ever sends V3 once it has
 * positively confirmed, via ANNOUNCE_VERSION, that this ESP peer is built
 * against protocol_version >= 10 (SaftyFW's link_frame.h
 * LINK_FRAME_STATUS_V3_MIN_PROTOCOL) -- see link_frame_pack_status()'s own
 * doc comment there for the full argument, applied a second time to this
 * same frame.
 *
 * ABSENT-BYTE SAFETY: whenever the applied frame is V1 or V2 length (no byte
 * 24/25 at all -- an older Pico, or a newer one not yet confirmed for V3),
 * this driver reports borrowed_known == false, NOT borrowed == false. A
 * caller that read the absence of the byte as "not borrowed" would be
 * reading a plausible-looking, confidently WRONG safe-sounding answer into a
 * field that was simply never sent -- the exact `min_compatible` precedent
 * (a too-short frame must read as UNKNOWN and fail closed, never as a zero
 * that silently means "fine") applied here: "no data yet" and "confirmed not
 * borrowed" are different facts, and only the peer's own V3 byte can tell
 * them apart. Every renderer of this field (safety_cfg_http.c) must check
 * borrowed_known first, exactly as tx_dropped_known already gates
 * tx_dropped_sat above. */
#define SAFETY_LINK_STATUS_FRAME_LEN_V3 KILNLINK_FRAME_A_LEN_V3
#define SAFETY_LINK_STATUS_FRAME_LEN    SAFETY_LINK_STATUS_FRAME_LEN_V1

/* flags2 byte (offset 24, V3 only) -- mirrors SaftyFW's link_frame.h
 * LINK_FLAG2_BORROWED exactly (same numeric value, same "byte 1 has no room
 * left" reasoning for why this is a whole new byte, not a reused bit). */
#define SAFETY_LINK_STATUS_FLAG2_BORROWED 0x01u

/* 2026-09-08 (safety_tc_warn_mask_disagreement audit): cold-junction validity,
 * mirrors SaftyFW's link_frame.h LINK_FLAG2_CJ_VALID exactly (same numeric
 * value, same reasoning). The MAX31856's cold junction is an on-chip sensor
 * independent of the external thermocouple probe SAFETY_FLAG_TEMP_VALID
 * describes -- this bit lets a probe-only fault (chip alive, cj_temp_c real)
 * be told apart from the chip itself not converting (both NaN), which was
 * previously indistinguishable on this wire (see diagnostics_http.h's
 * diag_safety_tc_state() "probe_fault" state). */
#define SAFETY_LINK_STATUS_FLAG2_CJ_VALID 0x02u

/* 2026-09-23: mirrors SaftyFW's link_frame.h LINK_FLAG2_TC_CONFIG_REASSERTED
 * exactly (same numeric value, same reasoning). Set iff thermo_task.c's
 * live-config mismatch counter is nonzero -- sticky for the rest of the
 * Pico's boot, not a one-shot pulse (that counter never resets). Answers
 * only "was the MAX31856's config ever reasserted this boot", nothing about
 * how many times or when most recently. */
#define SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED 0x04u

/* 2026-09-23: mirrors SaftyFW's link_frame.h LINK_FLAG2_ACTIVE_SLOT_KNOWN
 * exactly (same numeric value, same reasoning) -- docs/PICO_AUTO_UPDATE_PLAN.md:64's
 * named gap ("the ESP has no wire field for the Pico's active slot, so A/B
 * alternation is blind"). Set iff update_task_get_active_slot() (SaftyFW)
 * established this boot's running bootloader slot from a real flash
 * metadata record; SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_B is only
 * meaningful when this bit is set (same "_known" pairing convention as
 * CJ_VALID/TC_CONFIG_REASSERTED above -- a plain SLOT_B bit alone would
 * make "slot A" and "slot unknown" both read as 0). */
#define SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN 0x08u

/* 2026-09-23: mirrors SaftyFW's link_frame.h LINK_FLAG2_ACTIVE_SLOT_B
 * exactly (same numeric value, same reasoning). Clear = slot A, set = slot
 * B; meaningless unless SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN is also
 * set. */
#define SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_B 0x10u

/* borrowed_zone_index sentinel (byte 25) -- mirrors SaftyFW's link_frame.h
 * LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN exactly. A real value is always
 * 0..2 (config_store.h borrowed_zone_index's own range), so 0xFF can never
 * collide with one. */
#define SAFETY_LINK_BORROWED_ZONE_UNKNOWN 0xFFu

/* Length of the Pico's power frame (SAFETY_CMD_POWER / Frame E,
 * CommonFW/docs/LINK_PROTOCOL.md sec 6), byte-for-byte
 * KILNLINK_POWER_LEN from kilnlink_power.h. Pushed unsolicited, "no guard
 * reads any of this. It exists to be displayed." -- see
 * safety_apply_power() in safety_link.c for the field layout. */
/* 2026-09-06: the Pico's encoder always writes the V2 (61-byte) layout
 * (kilnlink_power.h's KILNLINK_POWER_LEN_V2, +3 x u16 counts_avg), but a
 * pre-11-protocol Pico still sends the V1 (55-byte) layout -- accept both,
 * mirroring kilnlink_power_decode()'s own tolerance. */
#define SAFETY_LINK_POWER_FRAME_LEN_V1 55u
#define SAFETY_LINK_POWER_FRAME_LEN_V2 61u
#define SAFETY_LINK_POWER_FRAME_LEN SAFETY_LINK_POWER_FRAME_LEN_V2
#define SAFETY_LINK_POWER_CHANNELS 3u

/* Power frame flags byte (offset 2), kilnlink_power.h's
 * kilnlink_power_flag_t mirrored here for the same reason SAFETY_CMD_POWER
 * is hand-parsed rather than calling that codec (see uart_task_ids.h). */
#define SAFETY_LINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED 0x01u
#define SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED       0x02u
#define SAFETY_LINK_POWER_FLAG_CALIBRATED                0x04u
#define SAFETY_LINK_POWER_FLAG_COUNTS_VALID              0x08u

/* Length of the Pico's DIAG frame (SAFETY_CMD_DIAG / Frame B,
 * CommonFW/docs/LINK_PROTOCOL.md sec 6), byte-for-byte KILNLINK_DIAG_LEN
 * from kilnlink_diag.h. Pushed unsolicited on the same 500 ms cadence as the
 * status frame -- see safety_apply_diag() in safety_link.c for the field
 * layout. */
#define SAFETY_LINK_DIAG_FRAME_LEN 30u

/* DIAG flags byte (offset 25), kilnlink_diag.h's kilnlink_diag_flag_t
 * mirrored here for the same reason SAFETY_CMD_POWER/DIAG are hand-parsed
 * rather than calling that codec (see uart_task_ids.h). */
#define SAFETY_LINK_DIAG_FLAG_SIM_CONTEXT_SEEN      0x01u
#define SAFETY_LINK_DIAG_FLAG_CALIBRATION_MISSING   0x02u
#define SAFETY_LINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT 0x04u
/* bit3 (CLEAR_TRIP_DIAG_PRESENT) is on the wire (kilnlink_diag.h) but has no
 * consumer here yet -- not this pass's scope, left as found.
 * bit4, mirrored 2026-09-16: thermo_task.c's MAX31856 tc_type reconfigure
 * retry has exhausted its bound with the type still unverified. See
 * kilnlink_diag.h's own bit4 comment for what this does and does not mean --
 * a configuration/verification failure, not necessarily a bad temperature
 * reading (CLAUDE.md's "safety TC invalid is one CR1 byte" note). */
#define SAFETY_LINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP   0x10u
/* bit5/bit6, mirrored 2026-09-22: S1 (abs_max_temp_c) and S8
 * (max_rate_c_per_min) ship disabled-by-zero on the Pico -- see
 * kilnlink_diag.h's bit5/bit6 comments. Distinct from the bundled
 * CALIBRATION_MISSING (bit1) commissioning summary above: that bit does not
 * by itself say whether S1/S8 specifically are armed. */
#define SAFETY_LINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED 0x20u
#define SAFETY_LINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED   0x40u
/* bit7, mirrored 2026-09-23: config_store_write_volatile() (RP2040,
 * KILN_PROFILES_PLAN.md item 15) bumps config_version/config_crc in RAM only
 * -- see kilnlink_diag.h's bit7 comment. True means a reboot right now would
 * NOT reproduce the config_version this same frame also carries. */
#define SAFETY_LINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY    0x80u

/* DIAG boot_reason byte (offset 10), kilnlink_diag.h's
 * kilnlink_diag_boot_flag_t mirrored here, same reasoning as above. */
#define SAFETY_LINK_DIAG_BOOT_POWERON  0x01u
#define SAFETY_LINK_DIAG_BOOT_WATCHDOG 0x02u
#define SAFETY_LINK_DIAG_BOOT_BROWNOUT 0x04u
/* 2026-09-09, RP2040 fatal-fault diagnosability pass -- see
 * kilnlink_diag.h's own comment on these bits. Mutually exclusive with each
 * other (SaftyFW's watchdog_hw->scratch[5] latch can hold only one fatal
 * event per boot); may accompany SAFETY_LINK_DIAG_BOOT_WATCHDOG since the
 * fault is exactly what caused that reset. */
#define SAFETY_LINK_DIAG_BOOT_STACK_OVERFLOW 0x08u
#define SAFETY_LINK_DIAG_BOOT_MALLOC_FAILED  0x10u
#define SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED  0x20u

/* DIAG state byte (offset 24), kilnlink_diag.h's kilnlink_diag_state_t
 * mirrored here, same reasoning as above. */
#define SAFETY_LINK_DIAG_STATE_INIT    0u
#define SAFETY_LINK_DIAG_STATE_GRACE   1u
#define SAFETY_LINK_DIAG_STATE_ARMED   2u
#define SAFETY_LINK_DIAG_STATE_WARN    3u
#define SAFETY_LINK_DIAG_STATE_TRIPPED 4u

/* DIAG context_age_100ms sentinel: "never received" -- kilnlink_diag.h's
 * KILNLINK_DIAG_CONTEXT_AGE_NEVER mirrored here, same reasoning as above. */
#define SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER 255u

/* Length of the Pico's TRIP_EVENT frame (SAFETY_CMD_TRIP_EVENT / Frame D,
 * CommonFW/docs/LINK_PROTOCOL.md sec 6), byte-for-byte KILNLINK_TRIP_LEN
 * from kilnlink_trip.h. Pushed immediately on trip, not on the poll
 * cadence -- see safety_apply_trip_event() in safety_link.c for the field
 * layout and the trip_seq dedup rule. */
#define SAFETY_LINK_TRIP_EVENT_FRAME_LEN 29u
#define SAFETY_LINK_TRIP_EVENT_CHANNELS 3u

/* Length of the PC-facing GET_STATUS payload -- the frame above plus the
 * ESP-measured age -- as specified in uart_task_ids.h.
 *
 * 2026-08-23: grew from 25 to 27 (byte25 tx_dropped_sat, byte26 extra-flags
 * bit0 tx_dropped_known) -- closing the last hop of the DIAG-frame-went-dark
 * investigation's drop counter, which reached SaftyFW's own Frame A (byte
 * 23 there) and safety_link_status_t's cache but stopped there, invisible to
 * the PC tool. Not version-gated the way Frame A itself had to be: this
 * payload only ever crosses the PC<->ESP link, which already hard-gates on
 * devices.FirmwareVersion.compatible (an EQUALITY check on
 * UART_PROTOCOL_VERSION) before the PC tool sends ANY command at all -- by
 * the time this specific reply is built, PC and ESP are already confirmed on
 * the same wire contract, so there is no skew window to protect against the
 * way there was between two independently-flashed processors on the
 * isolated link. */
#define SAFETY_LINK_STATUS_PAYLOAD_LEN_V1 25u
#define SAFETY_LINK_STATUS_PAYLOAD_LEN_V2 27u
#define SAFETY_LINK_STATUS_PAYLOAD_LEN    SAFETY_LINK_STATUS_PAYLOAD_LEN_V2

/* byte26 (V2 only): extra flags beyond the original byte1 (which has no
 * spare bits left -- see safety_link.h's SAFETY_LINK_STATUS_FRAME_LEN
 * comment, same exhaustion on the Pico-facing side). */
#define SAFETY_LINK_STATUS_EXTRA_FLAG_TX_DROPPED_KNOWN 0x01u

/* Length of the PC-facing GET_LINK_STATS payload (uart_task_ids.h).
 *
 * 2026-08-23: grew twice the same day, both times additive, neither
 * version-gated -- this payload only ever crosses the PC<->ESP link, already
 * hard-gated on UART_PROTOCOL_VERSION equality before any command is sent at
 * all (same reasoning SAFETY_LINK_STATUS_PAYLOAD_LEN's own comment gives).
 *   19 -> 23 (bytes19..22, broadcast_dropped u32 LE): the ESP-side half of
 *     the DIAG-frame-went-dark investigation's first "count what gets
 *     discarded" ask. Always real -- purely local to this ESP, never
 *     depends on what the Pico has sent -- so no "known" bit needed.
 *   23 -> 31 (bytes23..26 diag_applied, bytes27..30 power_applied, both u32
 *     LE): real "applied N times" counters for DIAG/POWER, added once
 *     broadcast_dropped==0 and tx_dropped==0 both measured clean on
 *     hardware while DIAG stayed permanently dark -- ruling out both TX-ring
 *     loss and inbox-full loss meant the open question became "has DIAG
 *     actually been applied more than once at all", which
 *     safety_link_status_t's diag_ever_received (a one-shot bool) cannot
 *     answer and diag_uptime_ms cannot answer reliably (an SWD halt of the
 *     Pico can freeze/perturb its own clock mid-investigation, but cannot
 *     un-increment this counter). Also always real, same reasoning as
 *     broadcast_dropped. */
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V1 19u
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V2 23u
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V3 31u
/* V3 -> V4 (51): bytes31..34 frames_deframed, bytes35..38
 * frames_routed_nowhere, bytes39..42 frame_length_mismatch, bytes43..46
 * frame_crc_mismatch, bytes47..50 frame_resync (all u32 LE) -- the
 * deframer/dispatch-level counters uart_protocol_t now keeps, see
 * safety_link_stats_t's own doc comment on these five fields for why they
 * were added. Same additive/not-version-gated reasoning as V2/V3. */
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V4 51u
/* V4 -> V5 (60): bytes51..54 dequeued_total, bytes55..58 unmatched_cmd_count
 * (both u32 LE), byte59 last_unmatched_cmd_byte (u8) -- safety_link_stats_t's
 * own doc comment on these three fields has the full "why" (the drain-level
 * follow-up once frames_deframed proved DIAG/POWER arrive CRC-valid and
 * broadcast_dropped proved the inbox isn't backing up). Same additive/
 * not-version-gated reasoning as V2/V3/V4. */
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V5 60u
/* V5 -> V6 (96): bytes60..95, nine u32 LE per-command dequeue counts (see
 * safety_link_stats_t's own doc comment on cmd_status_count etc. for the
 * exact field order/offsets and the "why now" reasoning) -- the measurement
 * that ends the DIAG-frame-went-dark investigation's inference phase: reads
 * exactly what every dequeued message's command byte actually was, rather
 * than continuing to rule hypotheses out one counter at a time. Same
 * additive/not-version-gated reasoning as V2-V5. */
#define SAFETY_LINK_STATS_PAYLOAD_LEN_V6 96u
#define SAFETY_LINK_STATS_PAYLOAD_LEN    SAFETY_LINK_STATS_PAYLOAD_LEN_V6

/* Length of the PC-facing GET_DIAG / GET_TRIP_EVENT payloads
 * (uart_task_ids.h). */
#define SAFETY_LINK_DIAG_PAYLOAD_LEN       31u
#define SAFETY_LINK_TRIP_EVENT_PAYLOAD_LEN 34u

/* Reserved age meaning "no status has ever been received". Distinct from a
 * merely old reading: 65534 ms of staleness is a link that died a minute ago,
 * 65535 is a Pico that has never spoken. */
#define SAFETY_LINK_AGE_NEVER 0xFFFFu

/* A reply is "recent enough to call the link up" for this many poll periods.
 * Three, so a single dropped poll (or one that lands just after a status read)
 * doesn't flap link_up -- it takes a sustained silence. */
#define SAFETY_LINK_UP_PERIODS 3u

/* ROADMAP.md M6 / LINK_PROTOCOL.md sec 8: SAFETY_FAULT_SRC_SAFETY_LINK means
 * "no telemetry frame within 1.5 s", full stop -- a fixed wall-clock ceiling,
 * not "N poll periods". At the default 500 ms poll period and
 * SAFETY_LINK_UP_PERIODS=3 the two numbers agree (3*500=1500), which is
 * deliberate, but this constant is what actually governs the fault: if the
 * poll period is ever reconfigured (SET_POLL_PERIOD) the safety guarantee
 * must not silently loosen along with it. safety_link_is_stale() below is
 * the pure, host-testable comparison against this constant. */
#define SAFETY_LINK_STALE_MS 1500u

/* LINK_PROTOCOL.md sec 8 / ROADMAP.md M6: 30 s of continued silence aborts a
 * running firing (distinct from, and much larger than, SAFETY_LINK_STALE_MS
 * above, which only blocks *new* relay-on -- "a single dropped telemetry
 * frame must not abort a twelve-hour firing"). Consumed by
 * profile_executor.c's watchdog task, not by this driver directly: this
 * driver has no notion of "a firing is running". */
#define SAFETY_LINK_FIRING_ABORT_SILENCE_MS 30000u

/* Pure timeout comparison, no locking/hardware -- host-testable. Mirrors
 * safety_age_ms_locked()'s SAFETY_LINK_AGE_NEVER convention: "never received"
 * is always stale. */
static inline bool safety_link_is_stale(uint16_t age_ms, uint32_t threshold_ms)
{
    return age_ms == SAFETY_LINK_AGE_NEVER || (uint32_t)age_ms > threshold_ms;
}

/* Bug fix (2026-08-23), pure decision -- host-testable, no locking/hardware.
 * Pulled out of safety_link.c's safety_drain_inbox_ex() loop: after one
 * inbox message has just been dispatched, should the next uart_protocol_
 * receive() still be allowed to block (this call is still waiting for a
 * specific out-of-band reply it asked for), or is it safe to degrade to a
 * zero-wait opportunistic drain?
 *
 * Each `want_*` is true iff the caller was handed a non-NULL out-param pair
 * for that reply type (KILNLINK_CT_CAL_CMD / KILNLINK_CONFIG_PAGE_CMD /
 * KILNLINK_COMMIT_CONFIG_REJECTED_CMD -- safety_link.c's out-of-band replies);
 * each matching `got_*` is true once that reply has actually been captured.
 * A caller with no out-params at all (the plain safety_drain_inbox()
 * wrapper -- the periodic poll's pre-drain / GET_STATUS wait) wants none of
 * the three, so this always returns false and the caller's loop degrades to
 * a zero-wait drain after the very first message, same as before this fix.
 *
 * The bug this closes: safety_link_get_config_page() (and its two siblings,
 * safety_link_get_ct_cal() and safety_link_send_commit_config()) used to
 * degrade to a zero-wait drain unconditionally after the FIRST message,
 * whatever it was. The Pico also sends periodic/unsolicited broadcasts
 * (GET_STATUS, DIAG, POWER, TRIP_EVENT, FW_VERSION) on this same inbox, and
 * CONFIG_PAGE is this link's slowest reply to produce (SAFETY_LINK_REPLY_
 * TIMEOUT_MS's own comment: "the Pico actually doing the work ... or walking
 * its config store") -- so an unrelated frame routinely arrived first,
 * degraded the wait to zero, and the very next non-blocking receive found
 * nothing (the real CONFIG_PAGE reply was still in flight) and exited the
 * loop having burned only a few ms of the ~1.2s budget. safety_link_get_
 * config_page() then reported ESP_ERR_TIMEOUT even though the Pico answered
 * every single request (observed live 2026-08-23: s_diag_get_config_page_
 * handled_count tracking every one, safety_cfg_store_refetch() timing out on
 * every one).
 *
 * This fix (round 1) let safety_drain_inbox_ex() block for its full declared
 * wait_ms (~1.2s) whenever this returns true, which is safe in isolation
 * (that ceiling already existed -- GET_STATUS's own exchange could already
 * legitimately spend it against a dead link). It did NOT cause the
 * subsequent task-watchdog panic scare (round 2) -- that turned out to be an
 * unrelated, pre-existing bug the fetch NOW SUCCEEDING exposed for the first
 * time: safety_cfg_store_refetch() calling nvs_save_store() from safety_
 * poll_task, whose stack lives in PSRAM, right after a successful fetch --
 * an NVS/flash write from a task with an external-RAM stack asserts inside
 * ESP-IDF's cache-disable path (esp_task_stack_is_sane_cache_disabled()).
 * See safety_cfg_store.c's deferred-flush mechanism for that fix; this
 * predicate and the ~1.2s-per-call budget it gates were never the problem. */
/* 2026-08-24, round 3: `want_status`/`got_status` added, because the round-1
 * fix above closed this hole for CT_CAL/CONFIG_PAGE/COMMIT_CONFIG_REJECTED
 * and left the FOURTH caller -- the ordinary GET_STATUS poll -- with no way
 * to say what it was waiting for. A plain safety_drain_inbox() passes no
 * out-params, so every `want_*` was false, this predicate returned false
 * after the first frame, and the wait degraded to zero exactly as the round-1
 * bug did. The Pico sends 4 STATUS frames per DIAG and per POWER push on this
 * same inbox, so whenever a DIAG or POWER arrived first the poll exited with
 * got_status false and counted stats.timeouts, milliseconds before the STATUS
 * it had asked for landed.
 *
 * Measured on the bench 2026-08-24 (commit 8d1b015, 9600 baud, 500 ms poll):
 * sent 14471, received 14472, crc/framing errors 0 -- and timeouts 3616,
 * against diag applied 3618 and power applied 3618. Those three tracking each
 * other 1:1, on a link with zero CRC errors and a STATUS count equal to the
 * send count, is the signature: a quarter of all polls were reported as
 * failures on a link that answered every single request.
 *
 * This is NOT a widened deadline. No timeout constant changes; the call's own
 * declared wait_ms is the same ceiling it always had. It only lets the poll
 * spend the budget it was already given instead of abandoning it after one
 * unrelated frame -- the identical argument round 1 made for the other
 * three. */
static inline bool safety_drain_still_waiting(bool want_status, bool got_status, bool want_ct_cal,
                                               bool got_ct_cal, bool want_config_page,
                                               bool got_config_page, bool want_commit_rejected,
                                               bool got_commit_rejected, bool want_rollback_result,
                                               bool got_rollback_result)
{
    if (want_status && !got_status) {
        return true;
    }
    if (want_ct_cal && !got_ct_cal) {
        return true;
    }
    if (want_config_page && !got_config_page) {
        return true;
    }
    if (want_commit_rejected && !got_commit_rejected) {
        return true;
    }
    if (want_rollback_result && !got_rollback_result) {
        return true;
    }
    return false;
}

/* Pure decision: given the total count of applied STATUS frames
 * (safety_link_stats_t::frames_received) at the start and end of one
 * safety_poll_task() iteration (~poll_period_ms, nominally matching the
 * Pico's own free-running STATUS push period), decide whether that whole
 * iteration observed a push "gap" -- zero new STATUS frames applied over an
 * entire push period, via ANY drain path, not just this iteration's own
 * GET_STATUS wait window. Extracted as its own pure, host-testable
 * predicate for the same reason safety_drain_still_waiting()/safety_link_
 * rollback_infer_outcome() are.
 *
 * Why this exists, 2026-09-10 (docs/audits/
 * safety_link_get_status_timeout_counter_2026-09-10.md and its follow-up
 * review): SAFETY_CMD_GET_STATUS is NOT a request/reply pair -- the Pico
 * never answers it, it just pushes STATUS on its own free-running 500 ms
 * clock -- so "no frame landed inside safety_exchange()'s own ~345 ms wait
 * window" is phase-dependent noise, not a link-health signal (a first
 * attempt at this fix gated that per-exchange miss on safety_link_up_
 * locked(), which just made the counter a strict, less-sensitive subset of
 * that existing 1500 ms boolean, blind to a link genuinely losing e.g. 2 of
 * every 3 pushes while still reading "up"). This predicate instead measures
 * real push throughput over elapsed wall time, independent of any single
 * request's phase: on a healthy link, frames_received should advance by at
 * least one every ~poll_period_ms via SOME drain (the exchange's own wait,
 * an opportunistic pre-drain, or an idle-tick drain) even when this
 * iteration's own GET_STATUS wait missed; a genuinely lossy link shows
 * whole iterations with zero growth. */
static inline bool safety_link_status_push_gap_observed(uint32_t frames_received_before,
                                                          uint32_t frames_received_after)
{
    return frames_received_after == frames_received_before;
}

/* Per-request ACK timeout handed to uart_protocol_send. Deliberately much
 * shorter than the PC link's 200 ms default: uart_protocol retries up to
 * UART_PROTO_MAX_RETRIES (10) times internally, so with no peer at all every
 * request costs ~10x this before it gives up, and that whole time is spent
 * inside the poll task. At 50 ms the dead-peer cost is ~500 ms, which stays
 * comparable to the default poll period instead of dwarfing it. A frame is
 * ~2 ms on the wire at 115200, so 50 ms is still ~25x the round trip. */
#define SAFETY_LINK_ACK_TIMEOUT_MS 50u

/* How long to wait for a reply frame. Separate from the ACK timeout because
 * this covers the Pico actually doing the work -- reading its thermocouple
 * and three ADC channels, or walking its config store -- not just its receive
 * interrupt.
 *
 * Derived from the configured baud rate rather than fixed, because it stopped
 * working when it was fixed. This was 250 ms flat, chosen when the link ran
 * at 115200, where the longest frame is about 46 ms on the wire. When this
 * link was capped at 9600 -- a ceiling that belonged to the now-removed
 * TCMT1109 optocouplers, not to either firmware or the UART peripheral --
 * that same frame took about 550 ms, so a large reply could never arrive
 * inside the fixed 250 ms window. The symptom was safety_cfg_store_refetch()
 * timing out forever on a link that was otherwise healthy: the Pico answered
 * every time, just not fast enough for a constant written for a wire eight
 * times quicker.
 *
 * Worst-case wire time is KILNLINK_FRAME_STUFFED_MAX bytes at 10 bits each
 * (8N1 plus start and stop). Doubled, because request and reply both cross
 * the same wire, plus a fixed margin for latency neither wire-time term
 * covers (see the note appended just below this comment for why it is now
 * 300, not the original 100). That
 * gave ~1.2 s at the old 9600 ceiling and ~190 ms at 115200; the formula
 * scales automatically with whatever KILNCTL_SAFETY_BAUD_RATE is currently
 * set to (KilnFW/App/drivers/Kconfig has the current measured value) now
 * that the optocoupler ceiling is gone.
 *
 * RAISED from 100 to 300 (2026-08-28, live-hardware commissioning defect).
 * The 100 ms figure was labelled "for the Pico's own task latency", but the
 * Pico's OWN measurement of that latency (SaftyFW's link_task.c,
 * s_page_reply_us_max, decode-to-send-return) never exceeded ~900 us live --
 * the 100 ms was never actually being spent there. What the margin has to
 * cover, and what the old value left zero slack for, is everything AFTER the
 * Pico hands bytes to its own TX ISR: physical wire time for the ACTUAL
 * (usually much smaller than worst-case) frame, this side's own RX task
 * reassembling it a chunk at a time (espInterfaces/uart_protocol.c's
 * uart_protocol_rx_task, itself raised off a 32-byte chunk the same day --
 * see that change's own comment), and ordinary FreeRTOS scheduling jitter on
 * a board also running WiFi and LVGL. At 100 ms fixed, GET_CONFIG_PAGE page 1
 * (the bigger of the safety config's two pages) timed out on essentially
 * every live fetch attempt (safety_get_link_stats(): sent 60, timeouts 54)
 * while page 0 only ever converged via safety_link.c's own stash-adoption of
 * a reply that had already missed its window -- not randomly, but on a
 * consistent margin too tight for ordinary jitter to survive.
 *
 * The overall multi-page wall-clock ceiling (safety_cfg_store.c's
 * SAFETY_CFG_STORE_REFETCH_BUDGET_MS, 2000 ms) is UNCHANGED by this -- it
 * still independently bounds how long safety_cfg_store_refetch_locked() may
 * block regardless of this constant, which is what keeps this a bounded,
 * contained change rather than a repeat of the two prior budget-growing
 * attempts that put the board into a panic-reboot loop
 * (safety_link_get_config_page()'s own comment): even at this new value, two
 * page fetches back to back cost at most ~700 ms (2 * ~345 ms + the 40 ms
 * inter-page pacing), comfortably inside that 2000 ms ceiling in a single
 * poll iteration rather than needing several. */
#define SAFETY_LINK_REPLY_TIMEOUT_MS \
    ((uint32_t)(((KILNLINK_FRAME_STUFFED_MAX * 10u * 1000u * 2u) \
                 / CONFIG_KILNCTL_SAFETY_BAUD_RATE) + 300u))

/* Floor on the sleep between polls, so a link whose requests already burn
 * most of the period (see SAFETY_LINK_ACK_TIMEOUT_MS) still yields. */
#define SAFETY_LINK_MIN_POLL_GAP_MS 20u

/* Sleep between checks while polling is switched off (poll period 0). Only
 * costs a wakeup; keeps SET_POLL_PERIOD responsive without a notification. */
#define SAFETY_LINK_IDLE_TICK_MS 200u

/* 2026-08-20 congestion fix: exponential backoff added on top of
 * poll_period_ms for the inter-poll *sleep* only -- poll_period_ms itself
 * (what GET_LINK_STATS reports, and what safety_link_up_locked() measures
 * staleness against) is left alone, so this is purely "poll less often while
 * nothing is answering", never a change to the configured cadence a caller
 * asked for via safety_set_poll_period().
 *
 * The retry storm this exists for (uart_protocol.c's "no reply for msg N to
 * dev2/task7") was already rate-limited to one log line per
 * RETRY_LOG_INTERVAL_US on its own -- see uart_log_bridge.c's
 * UART_LOG_BRIDGE_MAX_RETRIES comment for the actual PC-link congestion
 * mechanism that fix addresses. This backoff is the belt-and-suspenders
 * half: independent of the log-forwarding fix, cut CPU/UART1 traffic a
 * genuinely absent RP2040 causes, and cap how long the poll task spends
 * blocked inside safety_exchange (SAFETY_LINK_ACK_TIMEOUT_MS *
 * UART_PROTO_MAX_RETRIES per failed exchange, twice per cycle once
 * peer_version_known is also false) once it is clear nothing is out there to
 * answer soon.
 *
 * SafetyLinkClass::no_reply_streak counts consecutive failed GET_STATUS
 * exchanges (poll-task-only, no lock -- same reasoning as down_logged);
 * extra sleep is (2^(streak-1) - 1) * poll_period_ms, capped at
 * SAFETY_LINK_BACKOFF_MAX_EXTRA_MS, so streak 1 adds nothing (a single miss
 * is normal jitter, not absence), streak 2 doubles the effective period,
 * streak 3 quadruples it, and so on up to the cap -- reset to zero, and
 * therefore full normal cadence, on the very next successful reply with no
 * reboot required (ROADMAP.md M1: a Pico that boots later must be found
 * without a power cycle on the ESP side). */
#define SAFETY_LINK_BACKOFF_MAX_STREAK    6u
#define SAFETY_LINK_BACKOFF_MAX_EXTRA_MS  4500u

/* A down link is logged once on the transition, then at most this often, so a
 * permanently absent Pico leaves periodic evidence in the log without one
 * warning per poll (which at the default period would be two per second). */
#define SAFETY_LINK_DOWN_LOG_PERIOD_MS 60000u

/* How long the FW_VERSION broadcast may be missing before the quiet
 * boot-window message escalates to an ERROR. Comfortably longer than the
 * observed handshake (~7.5 s from reset on the bench board, most of which
 * is the Pico's own boot), short enough that a peer which never answers is
 * still reported promptly. Heating is blocked for this whole window either
 * way -- this constant changes only the log severity, never the gate. */
#define SAFETY_LINK_VERSION_GRACE_MS 15000u

/* ROADMAP.md M5 / LINK_PROTOCOL.md sec 4: "recent_window_s should be >= 150 s
 * (two heater windows plus decay margin)" -- HEATER_WINDOW_MS is 60000
 * (profile_executor.c), so 2*60 + 60 margin = 180 s clears that floor with
 * room to spare. Transmitted in the frame itself (byte 13), not hard-coded
 * on the Pico side -- this constant is what actually governs the window this
 * driver tracks against. */
#define SAFETY_LINK_CONTEXT_RECENT_WINDOW_S 180u

/* --- Phase 10 (SaftyFW) / TODO.md 9.5: Pico firmware-update relay ---------
 * CommonFW/docs/UPDATE_PROTOCOL.md section 4's UPDATE_STATUS (0x14) reply.
 * SaftyFW's src/tasks/update_task.c is the actual source of truth for this
 * wire layout (that file's own header comment: UPDATE_PROTOCOL.md "names
 * this frame ... but never specifies a byte layout"); mirrored here
 * byte-for-byte since KilnFW cannot #include SaftyFW's header (a separate
 * repository/build target). See uart_task_ids.h's SAFETY_CMD_UPDATE_* block
 * for the five command ids this section works with. */
#define SAFETY_LINK_UPDATE_STATUS_HEADER_LEN 16u
#define SAFETY_LINK_UPDATE_STATUS_MAX_GAPS   32u

/* update_task_wire_state_t, SaftyFW src/tasks/update_task.c -- mirrored,
 * same reasoning as above. */
typedef enum {
    SAFETY_LINK_UPDATE_STATE_IDLE      = 0,
    SAFETY_LINK_UPDATE_STATE_REFUSED   = 1,
    SAFETY_LINK_UPDATE_STATE_ERASING   = 2,
    SAFETY_LINK_UPDATE_STATE_RECEIVING = 3,
    SAFETY_LINK_UPDATE_STATE_VERIFYING = 4,
    SAFETY_LINK_UPDATE_STATE_COMPLETE  = 5,
    SAFETY_LINK_UPDATE_STATE_ABORTED   = 6,
    SAFETY_LINK_UPDATE_STATE_FAILED    = 7,
    /* Owner decision 2026-09-20: the Pico's update_task.c now returns this
     * wire state (reusing the CRC_MISMATCH error bit) when the image it just
     * verified was written into the wrong slot -- linked against the OTHER
     * slot's vector table/entry point. Mirrored here so
     * net/ota_pico_relay.c's relay_wait_for_states() terminal set recognizes
     * it as a distinct, nameable rejection instead of falling through to a
     * generic 10 s UPDATE_END timeout. */
    SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE = 8,
    /* 2026-09-21 (SaftyFW commit dbba6e71, update_task.c's running-image
     * flash-overlap guard): a DISTINCT wire state for "this board's own
     * running image occupies flash the requested erase/program would have
     * to touch" -- observed on a bench Pico running a flat, bootloader-less
     * image loaded at XIP_BASE, whose extent overlapped
     * BOOTLOADER_METADATA_FLASH_OFFSET and part of slot A, so any relay
     * attempt against it can never succeed. Mirrored here, same reasoning
     * as SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE immediately above
     * (reuses the last_error INTERNAL bit, no wire-layout change). Without
     * this, ota_pico_relay.c's relay_wait_for_states() would not recognize
     * it as terminal and would sit out the full RELAY_ERASE_TIMEOUT_MS
     * (120 s) waiting for a RECEIVING that will never come. */
    SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP = 9,
} safety_link_update_state_t;

/* UPDATE_STATUS_ERR_* bitmask, SaftyFW src/tasks/update_task.c -- mirrored,
 * same reasoning as above. */
#define SAFETY_LINK_UPDATE_ERR_RELAY_CLOSED         (1u << 0)
#define SAFETY_LINK_UPDATE_ERR_TRIP_PENDING         (1u << 1)
#define SAFETY_LINK_UPDATE_ERR_TOO_HOT              (1u << 2)
#define SAFETY_LINK_UPDATE_ERR_HEADER_INVALID       (1u << 3)
#define SAFETY_LINK_UPDATE_ERR_VERSION_INCOMPATIBLE (1u << 4)
#define SAFETY_LINK_UPDATE_ERR_RETRANSMIT_CAP       (1u << 5)
#define SAFETY_LINK_UPDATE_ERR_CRC_MISMATCH         (1u << 6)
#define SAFETY_LINK_UPDATE_ERR_INTERNAL             (1u << 7)

/* Parsed UPDATE_STATUS. `state`/`last_error` are the raw wire bytes (cast to
 * safety_link_update_state_t / SAFETY_LINK_UPDATE_ERR_* by the caller) --
 * kept as plain uint8_t here so an unrecognised future state/bit value from
 * a newer Pico build round-trips instead of being silently coerced. */
typedef struct {
    uint8_t  state;
    uint8_t  last_error;
    uint32_t bytes_received;
    uint32_t total_chunks;
    uint32_t received_chunks;
    uint8_t  gap_count; /* how many of gap_chunk_indices[] are valid, <= SAFETY_LINK_UPDATE_STATUS_MAX_GAPS */
    uint16_t gap_chunk_indices[SAFETY_LINK_UPDATE_STATUS_MAX_GAPS];
} safety_link_update_status_t;

/* Reasons the isolated fault line may be asserted. The line is driven high
 * (fault) whenever *any* source is set, so no source can clear another's
 * assertion -- see safety_link_set_fault_source(). */
typedef enum {
    SAFETY_FAULT_SRC_MANUAL          = 0x01u, /* SAFETY_CMD_SET_FAULT_OUT from the PC */
    SAFETY_FAULT_SRC_PC_LINK         = 0x02u, /* PC control link lost */
    SAFETY_FAULT_SRC_THERMO          = 0x04u, /* a main-board thermocouple faulted */
    SAFETY_FAULT_SRC_SAFETY_LINK     = 0x08u, /* this link itself went stale */
    SAFETY_FAULT_SRC_APP             = 0x10u, /* whatever else the app decides */
    /* profile_executor.c's direction/rate sanity monitor (TODO.md section 6):
     * a zone's actual temperature isn't moving the way the commanded relay
     * state implies it should -- the signature of a thermocouple that reads
     * a fault bit correctly when open, but not when merely detached from the
     * kiln body while still electrically connected. Distinct from
     * SAFETY_FAULT_SRC_THERMO, which is the MAX31856's own fault bits (a
     * genuinely open/shorted input) -- this catches the case where the part
     * is happily reporting a plausible-looking, just-wrong number. */
    SAFETY_FAULT_SRC_THERMAL_SANITY  = 0x20u,
} safety_fault_source_t;

/* 2026-09-24 (fault-source instrumentation, root-causing the 18:03-19:11Z
 * unattributed S6a trip): a small ring of fault_sources TRANSITIONS, kept
 * purely so a mainFault the Pico latched can be traced back to which ESP
 * source flipped, and when, after the fact -- the device log ring had
 * already rotated past that window the one time this mattered. Fixed size,
 * static, no heap, no growth tied to any capacity constant (internal DRAM is
 * tight, CLAUDE.md's DRAM notes). SAFETY_LINK_FAULT_SRC_BIT_COUNT is the
 * number of bits in safety_fault_source_t above (bit index 0..5); it must be
 * bumped by hand if that enum ever grows a bit past 0x20, same "must be kept
 * in sync by hand" contract as SAFETY_FAULT_SRC_ALL itself. */
#define SAFETY_LINK_FAULT_EDGE_RING_LEN   16u
#define SAFETY_LINK_FAULT_SRC_BIT_COUNT   6u

/* 2026-09-27 (starved-ack fix): the minimum time an isolated fault source
 * must stay asserted before a deassert request may actually release it.
 *
 * escalate_guard_trip() (profile_executor_relay_io.c) asserts this line via
 * safety_link_set_fault_source(..., true) on a guard trip; the very next
 * thing that happens is profile_executor_halt() -> clear_this_runs_faults(),
 * which can request the deassert milliseconds later. The RP2040 safety
 * processor only latches mainFault (S6a) after sampling the line
 * continuously high for SAFTYFW_MAIN_FAULT_DEBOUNCE_MS
 * (firmware/SaftyFW/src/debounce_policy.h, currently 200 ms, at 10 ms
 * sampling) -- a fast enough ack can release the line before the Pico's
 * debounce window has even finished counting, starving the backup
 * processor's own latch of the very trip the ESP just decided to escalate.
 * The backup path must not depend on how quickly the primary acks its own
 * trip.
 *
 * 300 ms = SAFTYFW_MAIN_FAULT_DEBOUNCE_MS (200 ms) + a 100 ms margin for
 * sampling/scheduling jitter on both sides. Kept in sync with the Pico
 * constant by firmware/KilnFW/App/test/safety_fault_hold_mirror_drift_
 * check.py (there is no shared header across the two independently-built
 * firmware targets -- CommonFW carries only the wire protocol, not this
 * timing constant -- so a numeric mirror-drift check is the enforcement
 * mechanism, same family as approach_rate_cap_mirror_drift_check.py). If
 * SAFTYFW_MAIN_FAULT_DEBOUNCE_MS ever changes, that check fails until this
 * constant is deliberately updated to match. */
#define SAFETY_FAULT_MIN_HOLD_MS 300u

typedef struct {
    uint32_t uptime_ms;          /* xTaskGetTickCount()-derived, monotonic, immune to SNTP steps */
    uint32_t unix_time_s;        /* wall clock at record time, 0 if SNTP never synced
                                   * this boot -- same "0 means unsynced, never a fake
                                   * near-1970 date" convention as profile_executor_run.c's
                                   * run_started_unix_s */
    uint8_t  source_mask_before; /* fault_sources immediately before this transition */
    uint8_t  source_mask_after;  /* fault_sources immediately after this transition */
    uint8_t  first_set_bit;      /* bit index (0..SAFETY_LINK_FAULT_SRC_BIT_COUNT-1) of the
                                   * lowest-numbered source that went 0->1 this edge, or
                                   * 0xFFu if this edge was a pure clear (no source rose) */
} safety_fault_edge_t;

/* One entry per bit of safety_fault_source_t: how many times that source has
 * transitioned 0->1 this boot, and when it last did. rising_count saturates
 * at UINT16_MAX rather than wrapping (a wrapped counter reading a small
 * number would look like "barely happened" for a source that actually
 * fired tens of thousands of times). */
typedef struct {
    uint16_t rising_count[SAFETY_LINK_FAULT_SRC_BIT_COUNT];
    uint32_t last_rising_uptime_ms[SAFETY_LINK_FAULT_SRC_BIT_COUNT];
    bool     last_rising_valid[SAFETY_LINK_FAULT_SRC_BIT_COUNT];
} safety_fault_edge_counts_t;

/* Snapshot handed to a caller by safety_link_get_fault_edges() -- a plain
 * copy taken under state_lock, same convention as safety_link_get_stats().
 * entries[0..count-1] are in OLDEST-to-NEWEST order regardless of where the
 * ring's write head currently sits (the getter rotates them before copying
 * out, so a caller never has to reason about the ring geometry itself).
 * total_recorded is the lifetime (this-boot) count of edges seen, which can
 * exceed SAFETY_LINK_FAULT_EDGE_RING_LEN -- compare it against count to tell
 * whether the ring has wrapped and older entries were overwritten. */
typedef struct {
    safety_fault_edge_t        entries[SAFETY_LINK_FAULT_EDGE_RING_LEN];
    uint32_t                   count;          /* valid entries, <= SAFETY_LINK_FAULT_EDGE_RING_LEN */
    uint32_t                   total_recorded; /* lifetime edge count this boot, may exceed count */
    safety_fault_edge_counts_t counts;
} safety_fault_edge_snapshot_t;

/* Snapshot of the last status the Pico sent, plus how old it is. Everything
 * here is a copy: reading it cannot block on, or be invalidated by, the far
 * side. */
typedef struct {
    bool     link_up;        /* a valid status within SAFETY_LINK_UP_PERIODS polls */
    uint16_t age_ms;         /* SAFETY_LINK_AGE_NEVER if nothing ever arrived */
    uint8_t  flags;          /* SAFETY_FLAG_* as they go out to the PC */
    float    tc_temp_c;      /* safety thermocouple, degC (NaN if never/invalid) */
    float    cj_temp_c;      /* safety cold junction, degC */
    uint8_t  tc_fault;       /* THERMO_FAULT_* bits from the safety MAX31856 */
    float    current_a[3];   /* current sense 1..3, amps */
    bool     fault_asserted; /* what this firmware is driving on GPIO6 */

    /* V2 status frame (byte 23, SAFETY_LINK_STATUS_FRAME_LEN_V2) --
     * 2026-08-23. tx_dropped_known is false (and tx_dropped_sat meaningless)
     * whenever the most recently applied status frame was V1-length (23) --
     * an older Pico, or one that has not yet confirmed this ESP supports V2
     * (see safety_link.h's SAFETY_LINK_STATUS_FRAME_LEN comment). Same
     * "false/meaningless until proven otherwise" convention as
     * power_ever_received/diag_ever_received below, at Frame-A scope
     * instead of a whole separate frame's. */
    bool     tx_dropped_known;
    uint8_t  tx_dropped_sat; /* saturating uart_owner TX-ring-drop count, Pico-side; 255 = "254 or more" */

    /* V3 status frame (bytes 24/25, SAFETY_LINK_STATUS_FRAME_LEN_V3) --
     * 2026-09-03, the BORROWED status flag. borrowed_known is false (and
     * borrowed/borrowed_zone_index meaningless) whenever the most recently
     * applied status frame was V1 or V2 length -- an older Pico, or one that
     * has not yet confirmed this ESP supports V3 (see safety_link.h's
     * SAFETY_LINK_STATUS_FRAME_LEN_V3 comment for why this is UNKNOWN, never
     * a false "not borrowed"). Same "false/meaningless until proven
     * otherwise" convention as tx_dropped_known above, one byte further out.
     * borrowed_zone_index is SAFETY_LINK_BORROWED_ZONE_UNKNOWN (0xFF)
     * whenever the Pico itself does not know it (borrowed_zone_index not
     * commissioned there), even on a V3 frame -- "known to be borrowed, but
     * from an unknown zone" is a real, distinct state from "not borrowed"
     * and from "unknown whether borrowed at all". */
    bool     borrowed_known;
    bool     borrowed;
    uint8_t  borrowed_zone_index; /* SAFETY_LINK_BORROWED_ZONE_UNKNOWN (0xFF) if not commissioned on the Pico */

    /* 2026-09-08 (safety_tc_warn_mask_disagreement audit): cold-junction
     * validity, carried in the SAME V3 status frame's flags2 byte (bit 1,
     * SAFETY_LINK_STATUS_FLAG2_CJ_VALID) that borrowed_known/borrowed above
     * already use -- no new frame, no protocol bump. cj_valid_known is false
     * (and cj_valid/cj_temp_c's validity meaningless) under the exact same
     * "false/meaningless until proven otherwise" convention as
     * borrowed_known: a V1/V2 frame, or a V3 peer this ESP hasn't yet
     * confirmed, carries no information here, which is UNKNOWN, not "cold
     * junction is bad". When cj_valid_known is true, cj_valid says whether
     * cj_temp_c is a trustworthy on-chip cold-junction reading INDEPENDENTLY
     * of the thermocouple probe's own SAFETY_FLAG_TEMP_VALID -- this is the
     * whole point: a probe fault (temp_valid false) with cj_valid true means
     * the chip is alive and converting, only the external probe/wiring is
     * bad, while cj_valid false alongside temp_valid false means the chip
     * itself never completed a conversion. See
     * diagnostics_http.h's diag_safety_tc_state(). */
    bool     cj_valid_known;
    bool     cj_valid;

    /* 2026-09-23: SaftyFW thermo_task.c's live-config mismatch counter,
     * carried in the SAME V3 status frame's flags2 byte (bit 2,
     * SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED) that borrowed_known/
     * cj_valid_known above already use -- no new frame, no protocol bump.
     * Same "false/meaningless until proven otherwise" convention: a V1/V2
     * frame, or a V3 peer this ESP hasn't yet confirmed, carries no
     * information here, which is UNKNOWN, not "never reasserted". When
     * tc_config_reasserted_known is true, tc_config_reasserted says whether
     * the safety processor has EVER re-asserted the MAX31856's CR0/CR1
     * config against a live-readback mismatch this boot (sticky, not a
     * count) -- see SaftyFW's thermo_task.h/link_frame.h for the full
     * rationale. */
    bool     tc_config_reasserted_known;
    bool     tc_config_reasserted;

    /* 2026-09-23: docs/PICO_AUTO_UPDATE_PLAN.md:64's named gap -- the Pico's
     * own view of which bootloader A/B slot it is currently running,
     * carried in the SAME V3 status frame's flags2 byte (bits 3/4,
     * SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN/_ACTIVE_SLOT_B). Same
     * "false/meaningless until proven otherwise" convention as
     * tc_config_reasserted_known above: pico_active_slot_known is false for
     * a V1/V2 frame, a V3 peer not yet confirmed, or a Pico boot that never
     * established its own active slot (e.g. a non-slot-linked dev target).
     * When pico_active_slot_known is true, pico_active_slot_is_b says A
     * (false) or B (true). */
    bool     pico_active_slot_known;
    bool     pico_active_slot_is_b;

    /* SAFETY_CMD_POWER (Frame E) telemetry -- ROADMAP.md M5/M6, TODO.md
     * 10.10. NaN/false fields below mean "never received" or "not a valid
     * reading", same convention as tc_temp_c/cj_temp_c above; a board with
     * no Pico firmware (or a Pico build that has not yet implemented Frame
     * E) leaves this permanently at its NaN-initialized state, which is
     * correct, not a bug -- see safety_link_start()'s init and
     * dashboard_http.c's dashboard_get_status(). */
    bool     power_ever_received;             /* at least one POWER frame has arrived */
    float    power_total_w;                   /* p_total_w; NaN if any contributing channel invalid */
    float    power_channel_w[SAFETY_LINK_POWER_CHANNELS]; /* p_avg_w per channel */
    float    power_channel_i_conducting_a[SAFETY_LINK_POWER_CHANNELS];
    float    power_channel_conduction_fraction[SAFETY_LINK_POWER_CHANNELS];
    float    power_mains_voltage_v;            /* NaN if not configured */
    uint8_t  power_window_s;                   /* window the conduction fractions cover */
    bool     power_mains_voltage_configured;
    bool     power_any_channel_clipped;
    bool     power_calibrated;
    /* Raw 16x-oversampled ADC counts per channel, independent of
     * calibration -- CommonFW/docs/LINK_PROTOCOL.md Frame E's counts_avg
     * field (2026-09-06, kilnlink_power.h KILNLINK_POWER_LEN_V2). Valid only
     * when power_counts_valid is true (a pre-11-protocol Pico's 55-byte V1
     * frame leaves these at 0/false -- see safety_apply_power()). */
    uint16_t power_channel_counts_avg[SAFETY_LINK_POWER_CHANNELS];
    bool     power_counts_valid;

    /* SAFETY_CMD_DIAG (Frame B) telemetry -- ROADMAP.md M5, LINK_PROTOCOL.md
     * sec 6: "Everything the 23-byte frame has no room for." diag_ever_received
     * is false (and every other diag_* field below meaningless) until the
     * first DIAG frame arrives -- a board with no Pico firmware, or a Pico
     * build that predates this frame's sender, leaves this permanently at
     * its zero-initialized state, same "no hardware yet, not a bug"
     * convention as power_ever_received above. */
    bool     diag_ever_received;
    uint8_t  diag_trip_reason;         /* SAFETY_TRIP_* (SaftyFW's safety_guards.h), 0 = none */
    uint16_t diag_warn_mask;           /* one bit per guard currently warning */
    uint16_t diag_trip_mask;           /* one bit per guard currently tripped */
    uint32_t diag_uptime_ms;           /* Pico uptime */
    uint8_t  diag_boot_reason;         /* SAFETY_LINK_DIAG_BOOT_* bits */
    uint8_t  diag_context_age_100ms;   /* SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER (255) = never */
    uint32_t diag_context_frames_ok;
    uint32_t diag_context_frames_bad;  /* CRC/framing/length errors, Pico-side */
    uint32_t diag_tx_frames_dropped;   /* Pico's TX ring full */
    uint8_t  diag_state;               /* SAFETY_LINK_DIAG_STATE_* */
    uint8_t  diag_flags;               /* SAFETY_LINK_DIAG_FLAG_* bits */
    uint32_t diag_log_frames_dropped;  /* Pico's log_task.c s_dropped -- LOG
                                         * frames never enqueued/sent (queue
                                         * full). Added KILNLINK_PROTOCOL_VERSION
                                         * 15 -> 16 / UART_PROTOCOL_VERSION 12 -> 13. */

    /* SAFETY_CMD_TRIP_EVENT (Frame D) -- the most recent trip event the Pico
     * has pushed, cached until a newer one replaces it. Deliberately never
     * cleared by anything else (not by CLEAR_TRIP, not by the trip
     * condition going away): "why did the kiln stop" must stay answerable
     * long after the trip itself cleared, which is the entire point of this
     * frame (LINK_PROTOCOL.md sec 6: "the answer is the only place the
     * evidence is preserved"). trip_event_ever_received is false (and every
     * other trip_* field below meaningless) until the first TRIP_EVENT frame
     * arrives. trip_last_seq is the Pico's own dedup key -- LINK_PROTOCOL.md
     * sec 6: "Idempotent: the ESP dedups on trip_seq" -- repeated copies of
     * the same event refresh trip_event_age_ms but are not logged as a new
     * event (see safety_apply_trip_event() in safety_link.c). */
    bool     trip_event_ever_received;
    uint8_t  trip_last_seq;
    uint8_t  trip_reason;              /* SAFETY_TRIP_* at the moment of this trip */
    uint32_t trip_uptime_ms;           /* Pico uptime at trip */
    float    trip_safety_tc_c;         /* safety thermocouple, degC, at trip */
    float    trip_deciding_threshold;  /* the threshold value that decided the trip */
    float    trip_current_a[SAFETY_LINK_TRIP_EVENT_CHANNELS]; /* current sense 1..3, amps, at trip */
    uint8_t  trip_relay_recent_mask;   /* relay_recent_mask last received from the ESP, at trip */
    uint8_t  trip_context_age_100ms;   /* context_age_100ms at trip */
    /* NOT part of the wire frame -- SaftyFW sees one bit (mainFault) and
     * cannot know why, so it cannot put this in Frame D. This is THIS
     * board's OWN safety_fault_source_t bitmask (safety_link_get_fault_
     * sources()), snapshotted the moment safety_apply_trip_event() sees a
     * NEW trip_seq (safety_link.c). Only meaningful when trip_reason ==
     * SAFETY_LINK_TRIP_REASON_MAIN_FAULT (S6a) -- every other guard's cause
     * lives entirely on SaftyFW's side and this field is simply whatever
     * this board's fault line happened to read at that instant, which may be
     * unrelated. Distinct from heat_block_sources (safety_link_get_fault_
     * sources(), read live): a source asserted at trip time may since have
     * been released, so "what tripped it" (this field) and "what's asserted
     * right now" (heat_block_sources) can legitimately disagree -- callers
     * that show both must label them separately, never merge them. */
    uint32_t trip_fault_sources;
    /* 2026-08-28 audit fix (N3): true only when trip_fault_sources above was
     * captured on a trip_seq change THIS BOOT ACTUALLY WITNESSED (this boot
     * was already tracking a previous trip_seq and saw it change) -- false
     * for the first TRIP_EVENT frame received after an ESP reboot, since that
     * frame can just as easily be the Pico resending a trip that latched
     * BEFORE this boot, in which case link->fault_sources at snapshot time
     * reflects only this boot's current fault lines, not whatever was
     * actually asserted at the real trip instant (see safety_apply_trip_
     * event() in safety_link.c for the is_genuinely_live_event logic).
     * Callers rendering trip_fault_sources (ui_page_diagnostics.c's S6a
     * "(at trip)" line, dashboard_http.c's trip_fault_sources/_words JSON)
     * MUST check this first and show "not captured" rather than a
     * plausible-looking but possibly-wrong value when it is false. Meaningless
     * (and left false) until trip_event_ever_received is true. */
    bool     trip_fault_sources_valid;
    /* How long ago this trip event was received, computed the same way
     * age_ms above is (safety_link_get_status() fills this in at read time
     * from a tick recorded when the frame was applied) -- distinct from
     * "how long ago the trip happened" (trip_uptime_ms is the Pico's clock,
     * which this ESP does not share a reference with; see SET_CLOCK's
     * "no guard may ever read this clock" note in LINK_PROTOCOL.md sec 4 for
     * why the two are not conflated). 0 if trip_event_ever_received is
     * false. */
    uint32_t trip_event_age_ms;
} safety_link_status_t;

/* Counters for the PC's GET_LINK_STATS. All monotonic since start. */
typedef struct {
    uint32_t frames_sent;     /* requests handed to uart_protocol_send (retransmissions
                               * happen inside that call and are not counted again) */
    /* well-formed GET_STATUS (Frame A) replies accepted -- ONLY that frame
     * type, despite the generic-sounding name. 2026-08-23, the DIAG-frame-
     * went-dark investigation: this field was read, more than once this
     * session, as "total frames received of any kind" -- it never was.
     * DIAG/POWER/TRIP_EVENT/FW_VERSION arriving and applying correctly
     * would never move this counter even in a perfectly healthy build; its
     * staying in lockstep with frames_sent at exactly the poll rate is
     * therefore NOT evidence that other frame types are being dropped, only
     * that GET_STATUS replies keep arriving. See diag_applied/power_applied
     * below for the counters that actually answer "has DIAG/POWER been
     * applied more than once" -- added specifically because this field
     * cannot. */
    uint32_t frames_received;
    uint32_t frame_errors;    /* malformed/unexpected payloads seen by this driver,
                               * plus uart_owner's line-error count for UART1 */
    uint32_t timeouts;        /* REDEFINED 2026-09-10 (docs/audits/
                               * safety_link_get_status_timeout_counter_2026-09-10.md
                               * and its follow-up review). NO LONGER "requests
                               * that produced no usable answer" -- that
                               * definition was only ever meaningful for
                               * SAFETY_CMD_GET_STATUS in practice (the only
                               * expect_status=true command), and GET_STATUS is
                               * not a request/reply pair: the Pico never
                               * answers it, so counting a per-exchange miss
                               * either produced a permanent, bimodal ~0%/~100%
                               * artifact (the original bug) or, gated on
                               * safety_link_up_locked(), a strict subset of
                               * that existing 1500 ms boolean blind to real
                               * partial loss (finding 5). Neither is counted
                               * here any more; safety_exchange() (safety_link_
                               * inbox.c) no longer touches this field at all.
                               *
                               * Now written exclusively by safety_poll_task()
                               * (safety_link_poll.c): incremented once per
                               * poll iteration (~poll_period_ms, nominally
                               * matching the Pico's own free-running STATUS
                               * push period) whose START-to-END span saw ZERO
                               * new STATUS frames applied via ANY drain path
                               * -- not just that iteration's own GET_STATUS
                               * wait window, so it is not phase-locked to this
                               * side's own request timing the way the old
                               * definition was. See safety_link_status_push_
                               * gap_observed()'s (this header) own doc comment
                               * for the full reasoning: near zero on a healthy
                               * link, rises with real partial loss, and
                               * distinguishable from safety_link_up_locked()
                               * (which only trips after a full 1500 ms of
                               * silence) because this counts individual lossy
                               * periods, not a single age threshold. */
    uint16_t poll_period_ms;  /* current period; 0 = polling off */
    /* 2026-08-23, the DIAG-frame-went-dark investigation, continued: real,
     * monotonic "this frame type was successfully applied N times" counts --
     * unlike safety_link_status_t's diag_ever_received/power_ever_received
     * (booleans, true forever after the first success, so they cannot
     * distinguish "applied once" from "applied continuously"), and unlike
     * diag_uptime_ms/the Pico's own clock fields (which an SWD halt of the
     * Pico can perturb, and did, during this investigation -- a frozen
     * uptime_ms does not by itself prove nothing since has arrived; a frozen
     * diag_applied does). Incremented at the end of safety_apply_diag()/
     * safety_apply_power() on every successful application, same
     * "monotonic since start" contract as every other field here. */
    uint32_t diag_applied;
    uint32_t power_applied;
    /* 2026-08-23, the DIAG-frame-went-dark investigation: BROADCAST frames
     * (GET_STATUS/DIAG/POWER/TRIP_EVENT/FW_VERSION -- everything SaftyFW's
     * link_task.c sends) whose delivery into this task's inbox failed
     * because the inbox was already full -- uart_protocol_get_task_
     * broadcast_dropped()'s own doc comment has the full "why this is a
     * distinct number from frame_errors" reasoning: frame_errors only counts
     * a frame that reached safety_apply_status()/_diag()/_power() and failed
     * ITS OWN check; this counts a frame that never got that far at all. A
     * lost BROADCAST has no ACK to withhold and the sender never retries, so
     * this is the ONLY record of it happening anywhere. */
    uint32_t broadcast_dropped;
    /* 2026-08-23, the DIAG-frame-went-dark investigation, final round:
     * diag_applied/power_applied pinned at exactly 1 per boot even with
     * tx_dropped==0 (Pico TX ring, confirmed live by SWD read) and
     * broadcast_dropped==0 (this ESP's per-task inbox, confirmed live by a
     * real nonzero reading during a reflash burst) both clean -- meaning
     * neither bracket of the pipeline was losing frames, yet the frames
     * were still vanishing somewhere between them. These five mirror
     * uart_protocol_t's own deframer/dispatch-level counters (see that
     * struct's doc comment for the full "why five, why now" reasoning) --
     * confirmed by reading the whole of handle_raw_frame()/
     * uart_protocol_rx_task() first that nothing already existed at this
     * layer under another name. */
    uint32_t frames_deframed;       /* CRC-valid frames of ANY type/dest this port pulled
                                     * off the wire -- upstream of every type-based branch,
                                     * including GET_STATUS/DIAG/POWER/TRIP_EVENT/FW_VERSION
                                     * alike (unlike frames_received above, which is
                                     * GET_STATUS only) */
    uint32_t frames_routed_nowhere; /* deframed, CRC-valid, but found no home: an
                                     * unregistered dst_task, or an ACK/NACK matching no
                                     * outstanding transaction. NOT a dst_device mismatch
                                     * (normal traffic filtering, not a loss) */
    uint32_t frame_length_mismatch; /* handle_raw_frame()'s own length check failed */
    uint32_t frame_crc_mismatch;    /* handle_raw_frame()'s own CRC16/CCITT-FALSE check failed */
    uint32_t frame_resync;          /* uart_protocol_rx_task()'s raw assembly buffer
                                     * overflowed before a delimiter closed the frame --
                                     * oversized/corrupt, discarded, resynced */
    /* 2026-08-23, the DIAG-frame-went-dark investigation, one round further:
     * frames_deframed proved DIAG/POWER ARE arriving CRC-valid (roughly the
     * expected extra ~0.83 f/s over status alone), frames_routed_nowhere
     * stayed 0 (not an unregistered dst_task), and broadcast_dropped stayed
     * flat at 0 across 30s against a 4-deep inbox -- which, combined,
     * ruled out "enqueued and never consumed" (that would fill a 4-deep
     * queue within ~4s of a stalled consumer, not sit flat). The remaining
     * question was whether safety_drain_inbox_ex() (the ONE registered
     * consumer of this inbox -- confirmed by grep, see the comment at its
     * own call to uart_protocol_receive()) is the thing draining them, and
     * if so, why its switch isn't applying them. These two answer that
     * directly rather than by further elimination. */
    uint32_t dequeued_total;       /* every successful uart_protocol_receive() return inside
                                    * safety_drain_inbox_ex(), counted BEFORE the switch --
                                    * compare against frames_deframed minus frames_received
                                    * (status): a match means the drain IS consuming
                                    * DIAG/POWER and the loss is inside the switch; a gap
                                    * means something other than this drain is emptying the
                                    * queue */
    uint32_t unmatched_cmd_count;  /* times the switch's default: branch was hit -- a
                                    * dequeued, CRC-valid message whose payload[0] matched
                                    * none of the switch's case labels */
    uint8_t  last_unmatched_cmd_byte; /* the actual payload[0] value from the most recent
                                       * unmatched_cmd_count hit -- meaningless (0) if
                                       * unmatched_cmd_count is still 0. Recording the real
                                       * byte rather than just a count on purpose: if DIAG is
                                       * somehow dequeued with the wrong first byte, this is
                                       * what shows what it actually was instead of leaving it
                                       * to be inferred */
    /* 2026-08-23, the DIAG-frame-went-dark investigation, the measurement
     * that finally ends it: a per-command histogram of every message this
     * drain actually dequeues, one field per switch case (see
     * safety_count_cmd_byte()'s own doc comment, safety_link.c, for the full
     * "why now" reasoning -- dequeued_total tracked frames_deframed almost
     * exactly and unmatched_cmd_count stayed 0, yet diag_applied/
     * power_applied both stayed 0 against ~80 unaccounted messages, which is
     * only possible if the working "non-status traffic is DIAG/POWER"
     * assumption was wrong). Summing all nine of these plus
     * unmatched_cmd_count must equal dequeued_total exactly -- every
     * dequeued message lands in exactly one bucket, never more than one,
     * never none. */
    uint32_t cmd_status_count;                  /* SAFETY_CMD_GET_STATUS (0x01) */
    uint32_t cmd_fw_version_count;               /* SAFETY_CMD_FW_VERSION (0x0B) */
    uint32_t cmd_update_status_count;            /* SAFETY_CMD_UPDATE_STATUS (0x14) */
    uint32_t cmd_power_count;                    /* SAFETY_CMD_POWER (0x0E) */
    uint32_t cmd_diag_count;                     /* SAFETY_CMD_DIAG (0x08) */
    uint32_t cmd_trip_event_count;                /* SAFETY_CMD_TRIP_EVENT (0x0D) */
    uint32_t cmd_ct_cal_count;                    /* KILNLINK_CT_CAL_CMD (0x1A) */
    uint32_t cmd_config_page_count;               /* KILNLINK_CONFIG_PAGE_CMD (0x1F) */
    uint32_t cmd_commit_config_rejected_count;    /* KILNLINK_COMMIT_CONFIG_REJECTED_CMD (0x20) */
    uint32_t cmd_rollback_result_count;           /* KILNLINK_ROLLBACK_RESULT_CMD (0x25) */
    uint32_t cmd_ct_auto_zero_status_count;       /* KILNLINK_CT_AUTO_ZERO_STATUS_CMD (0x28) */
    uint32_t cmd_reboot_result_count;             /* KILNLINK_REBOOT_RESULT_CMD (0x2A) */
    uint32_t cmd_stack_margin_count;              /* KILNLINK_STACK_MARGIN_CMD (0x2C) */
    uint32_t cmd_param_count;                     /* KILNLINK_PARAM_CMD (0x1E), reply to SAFETY_CMD_GET_PARAM (0x23) */

    /* 2026-08-23, size-window follow-up: the histogram above proves WHICH
     * cmd byte a dequeued frame carried, but says nothing about how LONG it
     * was -- and the leading FW_VERSION theory (safety_poll_task() retries
     * SAFETY_CMD_GET_FW_VERSION every poll cycle until peer_version_known
     * latches; safety_apply_fw_version() only requires msg.length >= 5 to
     * latch it) hinges entirely on length. If every cmd_fw_version_count hit
     * this boot has length == 1 (the ESP's own 1-byte request length, never
     * a real >=5-byte Pico reply), that is what is silently defeating
     * safety_parse_fw_version() and keeping the retry loop alive -- and it
     * would show here as last_fw_version_len pinned at 1 no matter how high
     * cmd_fw_version_count climbs. Captured for every counted cmd, not just
     * FW_VERSION, since it costs nothing extra here and the DIAG/POWER
     * lengths (should either ever start arriving) are exactly the field the
     * size-window hypothesis needs next. */
    uint8_t last_status_len;
    uint8_t last_fw_version_len;
    uint8_t last_update_status_len;
    uint8_t last_power_len;
    uint8_t last_diag_len;
    uint8_t last_trip_event_len;
    uint8_t last_ct_cal_len;
    uint8_t last_config_page_len;
    uint8_t last_commit_config_rejected_len;
    uint8_t last_rollback_result_len;
    uint8_t last_ct_auto_zero_status_len;
    uint8_t last_reboot_result_len;
    uint8_t last_stack_margin_len;
    uint8_t last_param_len;

    /* HW_ABSTRACTION.md "Still open", 2026-09-06: on-board ESP<->Pico link
     * reply latency, measured in safety_exchange() (safety_link_inbox.c)
     * from the moment a GET_STATUS request frame is handed to
     * uart_protocol_send_broadcast() to the moment safety_drain_inbox_
     * for_status() reports the matching STATUS reply decoded. This replaces
     * the pre-HAL paper figures (345 ms reply window, ~40 ms flight) and the
     * MCP-timed safety_ping() (~592 ms, contaminated by client/HTTP/serial
     * round-trip overhead outside this board) with a number measured
     * entirely on this side of the isolated link, using hal_time_now_us().
     *
     * Correlation is NOT by a wire seq/msg id -- SAFETY_CMD_GET_STATUS
     * carries none, and SaftyFW's link_task never runs the ACK'd DATA/ACK/
     * NACK transport (see safety_exchange()'s own comment). xact_lock only
     * rules out a second *exchange* racing this one on this ESP; it does
     * NOT make the STATUS frame decoded here a reply CAUSED by this send --
     * corrected 2026-09-10 (docs/audits/
     * safety_link_get_status_timeout_counter_2026-09-10.md): the Pico has no
     * dispatch case for GET_STATUS at all (LINK_PROTOCOL.md "no longer a
     * poll") and pushes STATUS unsolicited on its own free-running 500 ms
     * clock, so the frame landing inside this wait window may be that
     * independent push, coincidentally timed, rather than an answer to this
     * request. What this field actually measures is "how long after sending
     * did a status push happen to land", which is a useful on-link latency
     * figure but not a proven round-trip reply time. Only the expect_status
     * path (safety_link_ping()/the periodic poll) is measured; the fire-and-
     * forget BROADCAST path (safety_link_request_enable()) has no defined
     * "matching reply" to time.
     *
     * A miss inside the window (safety_drain_inbox_for_status() returns
     * false) contributes to none of these fields -- there is no frame to
     * time -- but, since the 2026-09-10 follow-up review, no longer
     * increments `timeouts` either: that field is redefined (see its own
     * doc comment above) to measure real elapsed-time push loss from
     * safety_poll_task(), not this call's own request-phase-dependent miss,
     * and `err` is returned as ESP_ERR_TIMEOUT from THIS call unconditionally
     * on a miss so safety_link_ping() stays independent evidence (finding 4
     * -- gating the return value on safety_link_up_locked() made a "ping"
     * that could report success against a peer that had gone silent up to
     * 1500 ms ago, laundering the very age check it exists to corroborate).
     * No separate link_reply_timeouts counter exists here either, since a
     * per-exchange miss count would just reintroduce the original
     * phase-locked artifact under a new name. */
    uint32_t link_reply_us_count;
    uint32_t link_reply_us_last;
    uint32_t link_reply_us_min;
    uint32_t link_reply_us_max;
    uint32_t link_reply_us_mean;    /* recomputed on every update from the
                                     * running sum kept in SafetyLinkClass::
                                     * link_reply_us_sum -- always current in
                                     * *out, no separate query needed */
} safety_link_stats_t;

typedef struct {
    /* This driver owns a *second* UART peripheral and a second protocol stack,
     * entirely separate from the PC link on UART0 (see App/main.c). Same code,
     * same framing, different port and different peer -- there is deliberately
     * no bespoke framing layer here. */
    uart_owner_t    owner;
    uart_protocol_t proto;
    QueueHandle_t   inbox;       /* frames addressed to (ESP, UART_TASK_ID_SAFETY) */
    /* frames addressed to (ESP, UART_TASK_ID_LOG) -- the Pico's own log_task
     * (LOG_LEVEL_*, firmware/SaftyFW/src/tasks/log_task.h) sends ordinary
     * BROADCAST frames here, same wire shape KilnFW's own uart_log_bridge.c
     * uses for its native lines (CommonFW/docs/LINK_PROTOCOL.md sec 6, Frame
     * F). Drained non-blockingly by safety_poll_task (safety_link_poll.c)
     * and handed to uart_log_bridge_relay_safety() -- a SEPARATE inbox from
     * `inbox` above so a burst of Pico log lines can never delay or starve a
     * GET_STATUS/DIAG/POWER reply sharing the same poll cycle. */
    QueueHandle_t   log_inbox;
    TaskHandle_t    poll_task;

    /* Guards everything below. Held only for the duration of a field
     * read/update -- never across a UART transaction, so a silent peer can
     * never block safety_link_get_status(). */
    SemaphoreHandle_t state_lock;
    /* Serializes whole request/reply exchanges. The reply arrives on a shared
     * inbox, so two tasks exchanging at once would each be able to consume the
     * other's answer; this makes "send, then wait for the frame it produces"
     * an actual critical section. Always taken *outside* state_lock. */
    SemaphoreHandle_t xact_lock;

    safety_link_status_t cached;      /* last good status, age computed on read */
    TickType_t           cached_tick; /* when `cached` arrived */
    bool                 ever_received;

    /* RELAY_LIFE_BUDGET.md -- K4 edge counting. Previous observed
     * SAFETY_FLAG_RELAY bit off consecutive GET_STATUS frames, so
     * safety_apply_status() can call relay_cycles_note_safety_edge() once per
     * OBSERVED transition rather than once per frame. Deliberately starts
     * "unknown" (safety_relay_state_known == false) rather than defaulting
     * safety_relay_state to false/off: an unknown-to-known first observation
     * must never itself be counted as an edge (same "reset one side of a
     * pair" hazard the boot_id_changed block below already guards against for
     * trip_last_seq -- a Pico reboot, or the ESP's own boot, must not
     * fabricate a transition out of nothing). Cleared back to unknown by
     * safety_apply_fw_version()'s boot_id_changed branch, since a Pico that
     * just rebooted may have left K4 in either state before this ESP ever
     * sees a fresh status frame from the new boot -- the first post-reboot
     * observation must resync, not compare against a stale pre-reboot
     * memory. */
    bool                 safety_relay_state_known;
    bool                 safety_relay_state;

    /* When the last SAFETY_CMD_TRIP_EVENT was applied -- separate from
     * cached_tick above, which only moves on GET_STATUS (Frame A). Read
     * under state_lock, same as cached_tick; safety_link_get_status() turns
     * it into safety_link_status_t::trip_event_age_ms the same way
     * cached_tick becomes age_ms. Meaningless until
     * cached.trip_event_ever_received is true. */
    TickType_t            trip_event_tick;

    /* Last CONFIG_PAGE (0x1F) frame that arrived while NOBODY was waiting for
     * one, kept so safety_link_get_config_page() can still find it.
     *
     * This is deliberately NOT the config cache -- safety_cfg_store.c still
     * owns that, and this driver still keeps no decoded config state. It is
     * one raw frame held for reply PAIRING, which is a transport concern and
     * so does belong here.
     *
     * It exists because every drain on this inbox is shared: the 500 ms
     * GET_STATUS poll drains the same queue, and a CONFIG_PAGE it happens to
     * pull was simply discarded (its case only stored the frame when the
     * current caller had asked for one). With the refetch re-requesting page
     * 0 on every poll, that races on every single attempt, which is how 69
     * CONFIG_PAGE frames were dequeued in one window while every fetch still
     * returned ESP_ERR_TIMEOUT. Stashing costs one memcpy on a path that
     * previously threw the frame away, and adds no blocking anywhere -- which
     * matters, because two earlier attempts at this bug extended how long
     * safety_poll may block and panic-rebooted the ESP.
     *
     * Read and written under state_lock. `has_stashed_config_page` is
     * cleared by whoever consumes it, so a stale page is never handed to a
     * second caller; the page_index is re-checked by the consumer, since the
     * stash may hold a page a later caller did not ask for. */
    uart_proto_message_t stashed_config_page;
    bool                 has_stashed_config_page;
    /* When the stashed frame was captured. The stash is only handed to a
     * caller asking for that exact page index (safety_take_stashed_config_
     * page()), so a page nobody goes on to ask for would otherwise sit here
     * forever; this lets it be aged out instead. */
    TickType_t           stashed_config_page_tick;

    /* Same problem, same fix, for SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20):
     * safety_link_send_commit_config() waits only SAFETY_LINK_REPLY_TIMEOUT_MS
     * for it before releasing xact_lock and reporting "accepted" by default.
     * On a bench run where the Pico's REJECTED frame arrived after that
     * window closed, the next drain to pass through this inbox -- typically
     * the 500 ms GET_STATUS poll, which shares this same queue -- found the
     * frame with no caller waiting for it and silently discarded it, the same
     * way an unmatched CONFIG_PAGE used to be discarded before the stash
     * above existed. Unlike CONFIG_PAGE there is no page index to match: only
     * one COMMIT_CONFIG can be in flight at a time (serialized by xact_lock),
     * so any stashed rejection belongs to the most recent commit and is
     * unconditionally reclaimed by the next call that asks. */
    uart_proto_message_t stashed_commit_rejected;
    bool                 has_stashed_commit_rejected;
    TickType_t           stashed_commit_rejected_tick;

    /* Same problem, same fix, for SAFETY_CMD_ROLLBACK_RESULT (0x25) --
     * safety_link_send_rollback_ex()'s send-burst/reply-window wait is
     * bounded (SAFETY_LINK_REPLY_TIMEOUT_MS, ~345ms at 230400 baud), but a
     * rollback reboot -- flash metadata write, watchdog reboot, bootloader
     * jump, app re-init -- takes far longer than that. A refusal that lands
     * after the reply window closes used to be silently discarded by
     * whatever drain passed through this inbox next (typically the ordinary
     * GET_STATUS poll, which shares this same queue) -- the exact
     * CONFIG_PAGE/COMMIT_CONFIG_REJECTED failure mode the two stashes above
     * already fix, applied to the one frame that used to have no stash
     * branch at all (see safety_drain_inbox_ex()'s KILNLINK_ROLLBACK_
     * RESULT_CMD case). Consulted by safety_link_send_rollback_ex()'s
     * boot_id-reconnect watch on every poll of that watch, so a late
     * refusal is still surfaced as REFUSED instead of being silently lost
     * and the watch timing out into a wrong ACCEPTED (or worse, a boot_id
     * change from an unrelated reboot racing the same window). Read and
     * written under state_lock, same as the two stashes above; only one
     * rollback request can be in flight at a time (serialized by
     * xact_lock), so any stashed frame unconditionally belongs to the most
     * recent request. */
    uart_proto_message_t stashed_rollback_result;
    bool                 has_stashed_rollback_result;
    TickType_t           stashed_rollback_result_tick;

    /* CT_COMMISSIONING_PLAN.md step 2 -- SAFETY_CMD_CT_AUTO_ZERO_STATUS
     * (0x28), same unconditional-stash treatment as the three above: this
     * frame arrives on the same shared inbox as GET_STATUS/DIAG/POWER, so a
     * reply landing between safety_link_get_ct_auto_zero_status()'s own
     * drain window and the caller looking again would otherwise be silently
     * discarded by the next unrelated drain (typically the 500 ms GET_
     * STATUS poll). Unlike CT_CAL/CONFIG_PAGE/COMMIT_REJECTED (which have a
     * `want_*` out-param path threaded through safety_drain_inbox_ex()),
     * this one is ALWAYS stashed rather than also offering a direct
     * out-param -- the auto-zero flow only ever has one caller
     * (safety_cfg_http.c's commissioning POST handler) polling at its own
     * pace across a multi-second measurement, so the extra out-param
     * plumbing safety_drain_still_waiting() would need bought nothing here. */
    uart_proto_message_t stashed_ct_auto_zero_status;
    bool                 has_stashed_ct_auto_zero_status;
    TickType_t           stashed_ct_auto_zero_status_tick;

    /* SAFETY_CMD_REBOOT_RESULT (0x2A) -- the reply to the reboot-in-place
     * command (kilnlink_reboot_result.h), stashed unconditionally exactly
     * like CT_AUTO_ZERO_STATUS above and for the same reason: it arrives on
     * this same shared inbox as GET_STATUS/DIAG/POWER, so a reply landing
     * between safety_link_send_reboot()'s own drain window and the caller
     * looking again would otherwise be discarded by the next unrelated
     * drain. One caller (sw_reset_http.c, via safety_link_send_reboot()),
     * serialized by xact_lock, so there is nothing to match on -- any
     * stashed frame belongs to the most recent request. */
    uart_proto_message_t stashed_reboot_result;
    bool                 has_stashed_reboot_result;
    TickType_t           stashed_reboot_result_tick;

    /* KILNLINK_STACK_MARGIN_CMD (0x2C) -- the reply to SAFETY_CMD_GET_STACK_
     * MARGIN (0x2B, KILNLINK_PROTOCOL_VERSION 13). Same "always stashed, one
     * caller (safety_link_get_stack_margin()), serialized by xact_lock"
     * pattern as stashed_reboot_result above -- there is exactly one
     * consumer and nothing to match a reply against beyond "most recent
     * request", so the out-param plumbing safety_drain_inbox_ex() offers
     * CT_CAL/CONFIG_PAGE/etc buys nothing here. */
    uart_proto_message_t stashed_stack_margin;
    bool                 has_stashed_stack_margin;
    TickType_t           stashed_stack_margin_tick;

    /* KILNLINK_PARAM_CMD (0x1E) -- the reply to SAFETY_CMD_GET_PARAM (0x23,
     * KILNLINK_PROTOCOL_VERSION 7). Same "always stashed, one caller
     * (safety_link_get_param()), serialized by xact_lock" pattern as
     * stashed_stack_margin/stashed_reboot_result above: exactly one consumer
     * and nothing to match a reply against beyond "most recent request" and
     * the param_id echoed back in the payload itself (checked by the caller
     * after taking the stash, not here). Variable length (KILNLINK_PARAM_
     * HDR_LEN..KILNLINK_PARAM_MAX_LEN), unlike the fixed-length stashes
     * above -- the bound check lives in safety_drain_inbox_ex()'s switch. */
    uart_proto_message_t stashed_param;
    bool                 has_stashed_param;
    TickType_t           stashed_param_tick;

    safety_link_stats_t stats;
    /* Running sum backing stats.link_reply_us_mean -- kept outside
     * safety_link_stats_t itself (which is a plain snapshot struct copied
     * whole by safety_link_get_stats()) so the mean field in that snapshot
     * can be recomputed from this exact sum/count pair every time it is
     * updated, same "sum kept separately, mean derived at read/update time"
     * shape as MAX31856.c's s_read_all_sum_us. Read/written under
     * state_lock, same as every other stats field. */
    uint64_t            link_reply_us_sum;
    uint16_t            poll_period_ms;
    /* 2026-08-20 congestion fix -- see SAFETY_LINK_BACKOFF_MAX_STREAK's
     * comment. Poll-task-only, no lock needed. */
    uint8_t             no_reply_streak;

    /* 2026-09-10, finding 5 fix: the per-iteration push-gap detector's own
     * baseline. Poll-task-only, no lock needed -- same convention as
     * no_reply_streak above; only safety_poll_task() ever reads or writes
     * these. push_gap_baseline_valid is false until the first iteration has
     * taken a baseline snapshot (there is nothing to compare the very first
     * reading against). See safety_link_status_push_gap_observed()'s doc
     * comment and stats.timeouts' own doc comment for what this drives. */
    uint32_t            push_gap_baseline_frames_received;
    bool                push_gap_baseline_valid;

    uint32_t fault_sources;        /* bitwise OR of safety_fault_source_t; a bit set here
                                     * means the isolated line IS physically asserted for
                                     * that reason -- true even while the SAME bit also sits
                                     * in fault_pending_deassert_mask below, since the line
                                     * has not actually been released yet. */

    /* 2026-09-27 (starved-ack fix, SAFETY_FAULT_MIN_HOLD_MS's own comment):
     * a deassert request for a bit that has not yet been continuously
     * asserted for SAFETY_FAULT_MIN_HOLD_MS is deferred rather than applied
     * immediately -- see safety_link_set_fault_source() and safety_link_
     * service_pending_fault_deassert() (safety_link.c). A bit set here is
     * still counted asserted in fault_sources above; both masks clear that
     * bit together once the hold time elapses. A re-assert of a pending bit
     * before that (safety_link_set_fault_source(..., true)) simply clears it
     * from this mask -- the line never moved, so there is nothing else to
     * undo. Read/written only under state_lock. */
    uint32_t fault_pending_deassert_mask;

    /* hal_time_now_ms() at the most recent 0->1 transition of each fault-
     * source bit (indexed by bit position, 0..SAFETY_LINK_FAULT_SRC_BIT_
     * COUNT-1) -- meaningful only while that bit is currently set in
     * fault_sources. Used solely to measure whether SAFETY_FAULT_MIN_HOLD_MS
     * has elapsed before honoring a deassert; distinct from fault_edge_last_
     * rising_uptime_ms below, which is a diagnostic history ring (xTaskGetTick
     * Count()-derived, never read to make a control decision) -- this array
     * uses hal_time_now_ms() specifically so the hold-time logic is host-
     * testable via fake_time.h's controllable clock, since the FreeRTOS tick
     * stub used by these host tests always reads 0 (App/test/stubs/freertos/
     * task.h). Read/written only under state_lock. */
    uint64_t fault_assert_tick_ms[SAFETY_LINK_FAULT_SRC_BIT_COUNT];

    /* 2026-09-24 fault-edge instrumentation (see safety_fault_edge_t's
     * comment above). Ring is a plain fixed array; fault_edge_ring_head is
     * the index the NEXT recorded edge will be written to (i.e. one past the
     * most-recently-written slot, mod the ring length -- the usual circular-
     * buffer convention). All fields here are read/written only under
     * state_lock, from inside safety_apply_fault_locked()'s caller
     * (safety_link_set_fault_source()), same lock discipline as fault_sources
     * itself. */
    safety_fault_edge_t fault_edge_ring[SAFETY_LINK_FAULT_EDGE_RING_LEN];
    uint32_t            fault_edge_ring_head;
    uint32_t            fault_edge_ring_count;   /* min(fault_edge_total_recorded, RING_LEN) */
    uint32_t            fault_edge_total_recorded;
    uint16_t            fault_edge_rising_count[SAFETY_LINK_FAULT_SRC_BIT_COUNT];
    uint32_t            fault_edge_last_rising_uptime_ms[SAFETY_LINK_FAULT_SRC_BIT_COUNT];
    bool                fault_edge_last_rising_valid[SAFETY_LINK_FAULT_SRC_BIT_COUNT];

    bool     fault_on_link_loss;   /* policy: raise SAFETY_FAULT_SRC_SAFETY_LINK
                                    * from the poll task when this link is down
                                    * OR a peer version mismatch is known (see
                                    * peer_version_compatible below) -- Phase
                                    * 7b.5, "the ESP treats it exactly like a
                                    * dead link" (LINK_PROTOCOL.md sec 4). */
    int      fault_io;

    /* Phase 7b (LINK_PROTOCOL.md sec 4, ANNOUNCE_VERSION/FW_VERSION):
     * mutual version handshake. esp_boot_id is generated once at
     * safety_link_start() and sent in every outbound ANNOUNCE_VERSION.
     * pico_boot_id/pico_boot_id_known come from the last FW_VERSION frame
     * the Pico pushed; a change re-sends ANNOUNCE_VERSION (a Pico that just
     * rebooted has forgotten everything, including who it was talking to).
     * peer_version_known/peer_version_compatible come from the same frame:
     * "known" gates whether a mismatch is asserted at all (no opinion until
     * the Pico has actually announced itself), "compatible" is the result of
     * the same two-way formula SaftyFW's link_frame_versions_compatible()
     * uses on its side. */
    uint8_t esp_boot_id;
    uint8_t pico_boot_id;
    bool    pico_boot_id_known;
    /* Set (under state_lock) by safety_apply_fw_version() when a boot_id
     * change means an ANNOUNCE_VERSION re-burst is owed to the peer, per the
     * comment above. Deliberately NOT sent synchronously from inside that
     * function any more (2026-09-10, docs/audits/
     * profile_executor_panic_2026-09-10_root_cause.md): safety_apply_fw_
     * version() is reachable from safety_drain_inbox()'s opportunistic
     * pre-drain, which runs on WHICHEVER task called safety_exchange() --
     * including profile_executor on its comparatively tiny 4096 B stack --
     * and the burst's own send chain (uart_protocol_send_broadcast ->
     * frame_and_send -> ... ) is the deepest path in this codebase. Flagging
     * it here and having safety_poll_task (safety_link_poll.c, its own
     * dedicated 8192 B stack, and the same task that already does this exact
     * send unconditionally as its boot push) perform the actual send moves
     * that chain off every OTHER caller's stack instead. Read and cleared
     * under state_lock by safety_poll_task once per iteration; never read or
     * cleared anywhere else. */
    bool    reannounce_pending;
    /* Same deferral, same reason, for safety_apply_diag()'s stale-S6a
     * boot-clear send (safety_link_frames.c) -- another synchronous
     * uart_protocol_send_broadcast()-reaching call previously made straight
     * from inside safety_drain_inbox_ex()'s dispatch, on whatever task was
     * draining. Set under state_lock by safety_apply_diag() once its
     * one-shot eligibility check passes; consumed by safety_link_service_
     * boot_clear_if_pending() (called from safety_poll_task) which performs
     * the actual send and latches s_boot_clear_attempted only on success,
     * preserving the original retry-on-refusal contract. */
    bool    boot_clear_pending;
    bool    peer_version_known;
    bool    peer_version_compatible;
    /* The peer's own numbers off the last FW_VERSION frame, kept alongside
     * peer_version_known/peer_version_compatible above purely so a caller
     * (dashboard_http.c/ui_page_home.c, TODO.md 9.0's deferred "GUI names
     * both versions and which one is older" item) can show the actual
     * numbers rather than just a bool -- meaningless until peer_version_known
     * is true, same convention as peer_version_compatible. */
    uint16_t peer_protocol_version;
    uint16_t peer_min_compatible;

    /* TODO.md owner-report item 5 (2026-08-21): the rest of the Pico's own
     * FW_VERSION (Frame C) reply -- dirty/commit/datetime/config_version/
     * config_crc -- previously parsed only far enough to reach boot_id and
     * then discarded (safety_parse_fw_version() skipped over commit/datetime
     * purely to find their length, never copying them anywhere). Captured
     * here so a caller (dashboard_http.c/safety_page.html,
     * CommonFW/docs/LINK_PROTOCOL.md sec 7's "Show the safety processor's
     * own build identity, not just the ESP's") can display which RP2040
     * firmware is actually running and whether its config_store has ever
     * been commissioned (config_crc == 0 alongside peer_build_known == true
     * means it is still running compiled-in defaults). peer_build_known is
     * false (and every other peer_build_* field meaningless) until at least
     * one FW_VERSION frame has parsed far enough to reach config_crc -- a
     * truncated/old-format frame that stops short of it leaves this false,
     * same "don't report a stale/zero value as real" discipline as
     * peer_version_known above. Strings are NOT null-terminated by the wire
     * (kilnlink_announce.h's own convention) -- peer_build_commit_len/
     * peer_build_datetime_len record how many bytes of each buffer are
     * valid. */
    bool    peer_build_known;
    bool    peer_build_dirty;
    /* Sized to kilnlink_announce.h's KILNLINK_ANNOUNCE_MAX_COMMIT_LEN (64) /
     * MAX_DATETIME_LEN (32) by value, not by #include -- this header stays
     * free of the kilnlink dependency (see the forward-declaration comment
     * above); safety_link.c, which already includes kilnlink_announce.h,
     * _Static_assert's these two literals equal to it. */
    uint8_t peer_build_commit[64];
    uint8_t peer_build_commit_len;
    uint8_t peer_build_datetime[32];
    uint8_t peer_build_datetime_len;
    uint8_t  peer_config_version;
    uint16_t peer_config_crc;

    /* Phase 10 (SaftyFW) / TODO.md 9.5: last-received UPDATE_STATUS,
     * applied the same way cached/cached_tick are (safety_apply_status()) --
     * see safety_apply_update_status() in safety_link.c. Guarded by
     * state_lock, same as everything else in this struct. */
    safety_link_update_status_t update_status;
    TickType_t                  update_status_tick;
    bool                        update_status_ever_received;

    bool       down_logged;      /* rate limiting for the "link is down" warning */
    TickType_t down_log_tick;

    /* 2026-09-15 (Opus review F3, "the commissioning page owns the type"):
     * tc_type_last_sent used to live here, tracking the last tc_type this
     * driver pushed to the Pico (safety_sync_tc_type(), safety_link_poll.c,
     * now removed). The Pico's own commissioning page is the sole writer of
     * its tc_type now; the ESP never pushes one, so there is nothing left to
     * track. Field removed entirely, not merely unused. */

    /* ROADMAP.md M5 / LINK_PROTOCOL.md sec 4 -- SAFETY_CMD_PUSH_CONTEXT
     * source pointers, set once by safety_link_set_context_sources() (called
     * from app_main after both boards have come up, same pattern as
     * dashboard_http_start()'s hardware pointers). Either may stay NULL
     * (board not populated this boot); the push then reports zone_count = 0
     * and relay_now_mask = 0 rather than skipping the frame -- the Pico
     * still learns boot_id/seq/uptime/flags either way. Stored as void* --
     * see this header's forward-declaration comment above. */
    void *context_io;         /* kiln_io_t*, relay state */
    void *context_thermo_bus; /* MAX31856BusClass*, raw per-channel readings */

    /* relay_recent_mask bookkeeping (LINK_PROTOCOL.md sec 4: "relays
     * commanded on at any point in the last recent_window_s"), poll-task-only
     * state, same no-lock reasoning as down_logged above -- only the poll
     * task ever samples relay_now_mask or reads/writes these. Index i =
     * relay i+1 (bit i of relay_now_mask). *_valid distinguishes "never seen
     * on this boot" from tick 0, which xTaskGetTickCount() can legitimately
     * be early in boot. */
    TickType_t relay_last_on_tick[4];
    bool       relay_last_on_tick_valid[4];

    /* SAFETY_CMD_PUSH_CONTEXT's own seq counter (LINK_PROTOCOL.md sec 4:
     * "increments every frame, never resets except on boot") -- distinct
     * from stats.frames_sent, which counts ACK'd GET_STATUS exchanges, not
     * this fire-and-forget broadcast. Poll-task-only, same as the fields
     * just above. */
    uint32_t context_seq;

    /* Per-channel sample_counter (LINK_PROTOCOL.md sec 4: "increments only
     * when a new conversion was actually read from that channel"),
     * incremented in safety_build_context() itself -- the point at which
     * this driver consumes that channel's MAX31856Reading -- never by the
     * mere act of building the frame around an unchanged counter. Sized to
     * MAX31856_CHANNEL_COUNT (3) without including MAX31856.h; a 4th slot
     * would simply never be touched if that constant ever grew. */
    uint8_t context_sample_counter[3];

    /* TODO.md 9.5/9.6: while a deliberate Pico update is relaying
     * (ota_pico_relay.c), the link legitimately goes quiet -- UPDATE_
     * PROTOCOL.md's "the Pico update deliberately trips the liveness rule"
     * section requires SAFETY_FAULT_SRC_SAFETY_LINK to keep asserting
     * (correct, untouched by this flag) but asks for the operator-facing
     * TEXT to say "updating" rather than "not responding". This flag does
     * exactly that and nothing else: it only affects which ESP_LOG* line
     * safety_update_health() emits for an already-down link, never whether
     * the fault bit itself is raised. Set via
     * safety_link_set_update_in_progress(), which ota_pico_relay.c's relay
     * task calls at the start and on every exit path -- see that module's
     * header comment for why this setter (rather than a direct include of
     * ota_pico_relay.h here) is what keeps this driver from depending on a
     * higher-level OTA feature it otherwise knows nothing about. */
    bool       update_in_progress_quiet;
    bool       version_mismatch_logged; /* edge-detect for the Phase 7b.5 mismatch log line;
                                          * poll-task-only, same no-lock reasoning as down_logged */
    /* Separates "we have not heard the peer's FW_VERSION yet" from "we heard
     * it and it is incompatible". Both fail closed and always have; only the
     * log severity differs, because the first is the NORMAL state for the
     * first few seconds of every boot -- the Pico's FW_VERSION is an
     * unsolicited broadcast, so there is always a window where the link is
     * carrying telemetry but the version has not landed. Logging that window
     * at ERROR on every healthy boot is how an operator learns to scroll past
     * the one message that matters (ROADMAP.md M10's whole premise). Held
     * poll-task-only, same no-lock reasoning as down_logged. */
    uint32_t   version_unknown_since_tick;
    bool       version_unknown_since_valid;
    bool       version_loud_logged; /* edge-detect for the escalated (ERROR) form, kept separate
                                      * from version_mismatch_logged so the quiet boot-window
                                      * message and the loud "still nothing" one cannot collapse
                                      * into a single edge and hide the second */
    bool       initialized;
} SafetyLinkClass;

/* Brings up UART1 on SAFETY_TX_IO/SAFETY_RX_IO at SAFETY_UART_BAUD_RATE with
 * no line inversion (neither end inverts across the ADuM1201 isolator) and
 * the RX pull-up enabled, drives SAFETY_FAULT_IO low
 * (de-asserted), attaches a uart_protocol_t, registers UART_TASK_ID_SAFETY and
 * starts the poll task.
 *
 * Returns ESP_OK once the *local* side is up. It does not, and cannot, tell
 * you whether a safety processor is listening -- that only shows up as
 * link_up in safety_link_get_status(). A missing peer is not a startup
 * failure; treating it as one would make the board refuse to boot for the
 * entire period during which the Pico firmware does not exist. */
esp_err_t safety_link_start(SafetyLinkClass *link);
esp_err_t safety_link_stop(SafetyLinkClass *link);

/* Copies the cached status out, with age_ms measured now. Never talks to the
 * far side, never blocks on it: a dead safety processor is stale data with
 * link_up = 0, not a hung call. This is what the SAFETY bridge task answers
 * GET_STATUS from. */
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out);

/* ROADMAP.md "Safety TC display audit, 2026-09-05" -- the single shared
 * predicate every "Thermocouple faults"-style display site (LCD, web,
 * PcTools) must call before showing the safety processor's OWN
 * thermocouple reading/fault as if it were an independent physical sensor.
 * SaftyFW's tc_source (safety_guards.h) is never sent to the ESP directly --
 * it is folded into the V3 status frame's BORROWED bit instead
 * (link_frame_pack_status(): BORROWED iff tc_source is BORROWED_ZONE or
 * BOTH), so "not BORROWED" here is exactly "tc_source == SAFETY_TC_SOURCE_
 * OWN_J7" on the Pico. Fail-to-shown: this hides the reading only when it
 * has been CONFIRMED borrowed (a V3-or-newer frame was received AND that
 * frame's BORROWED bit is set). Link down suppresses the display (no safety
 * processor at all means nothing to show). An older Pico that has never
 * sent a V3 frame (borrowed_known clear) is UNKNOWN, not confirmed
 * borrowed -- unknown must fail to shown, since hiding a genuine separate
 * safety-TC reading just because the peer hasn't confirmed it yet is worse
 * than occasionally showing a reused zone's reading unmarked as confirmed
 * separate. */
static inline bool safety_tc_is_separate_physical_sensor(const safety_link_status_t *st)
{
    if (!st || !st->link_up) {
        return false;
    }
    return !(st->borrowed_known && st->borrowed);
}

/* Asks the safety processor to permit (1) or drop (0) heating. Advisory --
 * the Pico's own interlocks always win. Blocks for the exchange, so call it
 * from a bridge/app task, not from anything latency-critical.
 *
 * Return values, corrected 2026-08-25. This used to promise ESP_ERR_TIMEOUT
 * "if the far side never ACKed", which stopped being implementable when this
 * request moved to the BROADCAST transport: broadcasts are not ACKed, so
 * there is no ACK to time out on and ESP_OK had degraded to meaning only
 * "the local UART accepted the bytes" -- returned happily with no peer at
 * all. Now:
 *
 *   enable=1: ESP_ERR_INVALID_STATE, having sent NOTHING, if the cached
 *             link_up is false (the last SAFETY_LINK_UP_PERIODS polls got no
 *             status back). ESP_OK does not confirm the peer acted -- nothing
 *             on this wire can -- but it does now confirm a peer was
 *             answering when the request went out.
 *   enable=0: always attempted, link_up or not. Asking a possibly-absent peer
 *             to STOP heating is the fail-safe direction; refusing it on a
 *             link that merely looks down is the one refusal that could leave
 *             heat on.
 *
 * Callers that need to know the peer actually changed state must read it back
 * from safety_link_get_status()'s heating_enabled, as they always had to. */
esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable);

/* Forces a status exchange now instead of waiting for the next poll tick, and
 * refreshes the cache from it. Works with polling switched off. Same blocking
 * caveat as safety_link_request_enable. */
esp_err_t safety_link_ping(SafetyLinkClass *link);

/* 0 stops polling (the link then carries only explicit ping/enable traffic);
 * anything else takes effect on the next poll iteration. Note this also moves
 * the link_up window, which is SAFETY_LINK_UP_PERIODS * period. */
esp_err_t safety_link_set_poll_period(SafetyLinkClass *link, uint16_t period_ms);

esp_err_t safety_link_get_stats(SafetyLinkClass *link, safety_link_stats_t *out);

/* ROADMAP.md M5 / LINK_PROTOCOL.md sec 4: sets the two hardware pointers the
 * poll task reads to build SAFETY_CMD_PUSH_CONTEXT -- io_or_null (kiln_io_t*,
 * relay state) and thermo_bus_or_null (MAX31856BusClass*, raw thermocouple
 * readings). Call once after both have come up (app_main, alongside the
 * other dashboard_http_start()-style wiring); either may be NULL if that
 * board didn't come up this boot, same non-fatal convention as everywhere
 * else in this codebase -- the push still goes out, just with zone_count = 0
 * and/or relay_now_mask = 0. Safe to call before or after safety_link_start
 * (the poll task only reads these fields, never assumes they're set by a
 * particular point in bring-up), but must not be called concurrently with
 * itself. */
void safety_link_set_context_sources(SafetyLinkClass *link, void *io_or_null,
                                      void *thermo_bus_or_null);

/* Phase 7b (LINK_PROTOCOL.md sec 4): reports what the last FW_VERSION frame
 * from the Pico said about compatibility. *out_known is false, and
 * *out_compatible, *out_peer_protocol and *out_peer_min_compatible are all
 * meaningless, until the Pico has pushed at least one FW_VERSION frame (its
 * own boot push, or a reply to safety_poll_task()'s explicit 0x0B request,
 * retried every poll period until known -- ROADMAP.md M6, closed 2026-08-19).
 * out_peer_protocol and out_peer_min_compatible
 * are the peer's own KILNLINK_PROTOCOL_VERSION/KILNLINK_MIN_COMPATIBLE off
 * that frame -- TODO.md 9.0's deferred "GUI names both versions and which one
 * is older" item, added so a caller can show the actual numbers rather than
 * just the bool verdict; either pointer may be NULL if the caller only wants
 * a subset. Never blocks on the far side. */
esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *out_known,
                                               bool *out_compatible,
                                               uint16_t *out_peer_protocol,
                                               uint16_t *out_peer_min_compatible);

/* TODO.md owner-report item 5: the rest of the Pico's FW_VERSION reply --
 * build identity and config commissioning state -- see the SafetyLinkClass
 * field comments above for exactly what "known" gates and why the strings
 * are copied out with explicit lengths rather than null-terminated. Any
 * out_* pointer may be NULL if the caller only wants a subset. commit_buf/
 * datetime_buf must have room for at least 64/32 bytes respectively when
 * non-NULL (the wire caps); *out_commit_len and *out_datetime_len are set to
 * how many bytes of each were actually copied. Never blocks on the far side. */
esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known,
                                             bool *out_dirty, uint8_t *commit_buf,
                                             uint8_t *out_commit_len, uint8_t *datetime_buf,
                                             uint8_t *out_datetime_len,
                                             uint8_t *out_config_version,
                                             uint16_t *out_config_crc);

/* --- Isolated fault line (GPIO6, an ESP OUTPUT) ---
 * High asserts: it lights U1's LED, which pulls the Pico's mainFault input
 * low. There is no hardware path in the other direction.
 *
 * set_fault() is the manual override the PC's SET_FAULT_OUT drives; it maps
 * onto SAFETY_FAULT_SRC_MANUAL. Automatic reasons (PC link lost, thermocouple
 * fault, ...) belong in their own source bits via set_fault_source, so that
 * clearing one reason cannot clear another's assertion -- the line is high
 * whenever any source is set. get_fault() reports the resulting line state. */
esp_err_t safety_link_set_fault(SafetyLinkClass *link, bool assert_fault);
bool      safety_link_get_fault(SafetyLinkClass *link);
esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask,
                                        bool assert_fault);
uint32_t  safety_link_get_fault_sources(SafetyLinkClass *link);

/* 2026-09-24 fault-edge instrumentation: copies out the fault-source
 * transition ring plus the per-source rising-edge counters, under
 * state_lock, in oldest-to-newest order (see safety_fault_edge_snapshot_t's
 * comment). Returns ESP_ERR_INVALID_ARG for a NULL link/out, ESP_ERR_INVALID_
 * STATE if the link was never started (out is left zeroed either way, so a
 * caller that ignores the return code still gets an honest "nothing
 * recorded" rather than garbage). Never blocks on the far side -- this reads
 * only local, already-applied state, same as safety_link_get_fault_sources(). */
esp_err_t safety_link_get_fault_edges(SafetyLinkClass *link, safety_fault_edge_snapshot_t *out);

/* Tells this driver that app_main's own bring-up this boot found nothing
 * wrong (boot_fault_sources == 0) -- i.e. the isolated fault line was never
 * asserted this boot. If SaftyFW is still reporting a LATCHED S6a trip
 * (main-controller-fault) when it is, that latch can only be a leftover from
 * before this boot (SaftyFW does not reboot alongside the ESP), so this
 * driver sends one SAFETY_CMD_CLEAR_TRIP the first time it sees that
 * specific combination. SaftyFW's own link_task_handle_clear_trip() still
 * decides whether to honor it -- if the guard is genuinely still tripped by
 * the time the request arrives, it re-latches, same "resend after
 * conditions change is safe" contract safety_link_send_clear_trip()'s doc
 * comment already describes. Scoped to S6a only: other guards (welded
 * contactor, frozen sensor, ...) are not this board's own fault and this
 * function must not paper over them. Call once, right after computing
 * boot_fault_sources in app_main -- a no-op if it is ever left uncalled. */
void safety_link_mark_boot_clean(void);

/* Policy switch for the one source this driver can raise by itself: whether
 * losing *this* link asserts the fault line.
 *
 * Default is true -- fail-safe. The safety processor's job is to cut heat when
 * the main controller is untrustworthy, and a main controller that cannot even
 * be reached is the clearest possible case of that; the failure we must never
 * have is a dead ESP that leaves the kiln heating because nobody told the Pico.
 * The cost of the default is that on a board with no Pico firmware (i.e. today)
 * the line sits asserted from the first missed poll onward -- which is correct
 * and harmless, since the only thing reading it is the processor that isn't
 * there. During bring-up, or on a board built without the safety domain
 * populated, call this with false; that is a deliberate, logged choice rather
 * than something the driver decides for you.
 *
 * Everything above the driver -- "the PC link dropped", "a thermocouple
 * faulted" -- is policy for the caller: raise the matching source bit. This
 * driver supplies the mechanism and one default it can defend. */
esp_err_t safety_link_fault_on_link_loss(SafetyLinkClass *link, bool enable);

/* TODO.md 9.6: tells the poll task's down-link logging (not its fault-source
 * assertion, which is untouched) that a deliberate Pico update is currently
 * relaying, so the link going quiet gets an informational "updating" line
 * instead of a "not responding" warning. `ota_pico_relay.c`'s relay task is
 * the intended caller: true when the relay actually starts sending frames,
 * false again on every exit path (success, refusal, timeout, internal
 * failure) via its single exit point. Safe to call from any task. */
esp_err_t safety_link_set_update_in_progress(SafetyLinkClass *link, bool in_progress);
bool      safety_link_get_fault_on_link_loss(SafetyLinkClass *link);

/* --- Phase 10 (SaftyFW) / TODO.md 9.5: Pico firmware-update relay --------
 * Thin, additive wrappers -- App/drivers/net/ota_pico_relay.c is the only
 * caller today, but these are general enough for anything that needs to
 * drive the Pico's UPDATE_* state machine. Neither of these touches
 * safety_exchange()'s request/reply machinery: UPDATE_* frames are
 * fire-and-forget broadcasts on both sides of this exchange (the Pico
 * "never participates in the ACK'd DATA/ACK/NACK transport" for them, per
 * SaftyFW's link_task.c), so uart_protocol_send() (ACK'd) is the wrong
 * primitive here -- see uart_protocol_send_broadcast(). */

/* Sends one UPDATE_* frame (BEGIN/DATA/END/ABORT -- `payload[0]` is the
 * caller's chosen SAFETY_CMD_UPDATE_* id, uart_task_ids.h) as a broadcast,
 * same (device, task_id) pair safety_link_send_announce_version_once() uses
 * for ANNOUNCE_VERSION. Fire-and-forget: returns as soon as the bytes are
 * handed to the UART, does not wait for or expect any reply -- the caller
 * polls safety_link_get_update_status() separately for that. `length` must
 * be <= UART_PROTO_MAX_PAYLOAD (253). */
esp_err_t safety_link_send_update_frame(SafetyLinkClass *link, const uint8_t *payload,
                                         size_t length);

/* Copies the last-received UPDATE_STATUS (0x14) out, with *out_age_ms (may
 * be NULL) set to how long ago it arrived -- same "copy is a snapshot, ages
 * are computed on read" contract as safety_link_get_status(). Returns
 * ESP_ERR_NOT_FOUND (out untouched) if no UPDATE_STATUS has ever been
 * received since boot; this is the expected state for the whole time no
 * Pico update is in progress, not an error. */
esp_err_t safety_link_get_update_status(SafetyLinkClass *link, safety_link_update_status_t *out,
                                         uint32_t *out_age_ms);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_CLEAR_TRIP (0x0A) -- the
 * GUI's path to acknowledging a Pico-latched trip (the only other path is
 * the physical E-stop assert/release cycle, SaftyFW/docs/SAFETY_MODEL.md
 * sec 6). Fire-and-forget BROADCAST, same as ANNOUNCE_VERSION/PUSH_CONTEXT:
 * the Pico's link_task.c never ACKs CLEAR_TRIP on the wire (see that file's
 * link_task_handle_clear_trip()), so there is no reply to wait for here --
 * success is observed by the caller polling safety_link_get_status() and
 * watching diag_trip_reason / diag_state fall back to "not tripped" on a
 * later DIAG (Frame B) frame, exactly like every other outcome-via-telemetry
 * pattern this driver already uses (REQUEST_ENABLE's own doc comment above).
 *
 * The `trip_mask` this sends is NOT a caller-supplied value: it is derived
 * from this driver's own cached DIAG state (cached.diag_trip_mask), the same
 * field the Pico's Frame B already reports and the *only* place this ESP
 * ever learns what the Pico currently has latched. This matters because
 * link_frame_trip_mask_for_reason() on the Pico side (SaftyFW's link_frame.c,
 * read-only reference) is exactly the function that produced that mask in
 * the first place -- echoing the cached value back is guaranteed to match
 * whatever the Pico is still comparing against, whereas anything computed
 * independently on this side could drift.
 *
 * Refuses locally (returns ESP_ERR_INVALID_STATE, logs why, sends nothing)
 * when:
 *   - no DIAG frame has ever been received (cached.diag_ever_received false)
 *     -- nothing is known to be latched, so there is nothing to echo back;
 *   - the cached DIAG data is stale beyond SAFETY_LINK_STALE_MS -- sending a
 *     mask that might no longer describe what the Pico has latched risks
 *     exactly the "stale clear matches a *different*, newer trip" case the
 *     mask-echo check exists to prevent (LINK_PROTOCOL.md's own words);
 *   - cached.diag_state != SAFETY_LINK_DIAG_STATE_TRIPPED -- nothing is
 *     currently latched to clear (the Pico would refuse anyway, but failing
 *     closed here means the operator gets an explanation immediately instead
 *     of after a round trip to a Pico that may not even be listening).
 * A resend after conditions changed (a second, different trip latched, or
 * the original trip cleared on its own) is safe to attempt again: the Pico's
 * own mask check is idempotent and simply refuses the mismatch, which is the
 * designed protection -- this local check is an operator-friendliness
 * shortcut, not the safety boundary. Returns ESP_ERR_INVALID_STATE if the
 * driver isn't initialized, ESP_OK once the broadcast has been handed to the
 * UART (not proof of Pico acceptance). Safe to call from any task (HTTP
 * handler, UART bridge, LVGL callback) -- broadcasts never block beyond
 * handing bytes to the UART, same as safety_link_send_update_frame(). */
esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_CONFIG (0x16) -- the
 * GUI's path to commissioning the safety processor's config_store.h record
 * (SaftyFW TODO.md Phase 9's "SAFETY_CMD_SET_CONFIG (wire command)"). Same
 * fire-and-forget BROADCAST shape as safety_link_send_clear_trip() above:
 * the Pico's link_task.c never ACKs this on the wire either (see that file's
 * link_task_handle_set_config()), so there is no reply to wait for here --
 * success is observed the same way CLEAR_TRIP's is, by the caller polling
 * the next GET_DIAG/GET_FW_VERSION and watching config_version advance (once
 * the accept/refuse outcome has propagated), or via the SaftyFW log if a
 * debug probe is attached.
 *
 * Unlike CLEAR_TRIP's trip_mask, `tc_type` is NOT derived from any cached
 * ESP-side state -- it is an operator choice (which thermocouple type is
 * physically fitted), not something the ESP could infer on its own. This
 * function only validates the wire-level range (0..0x0F, the MAX31856 CR1
 * TC[3:0] nibble -- the same range uart_bridge.c's THERMO_CMD_CONFIG_CHANNEL
 * handler already checks for the identical field, see its bridge_range_ok()
 * call) and sends; SaftyFW does not build KilnFW's MAX31856.h, and this
 * driver does not build SaftyFW's own, narrower max31856.h (only 8 of the 16
 * nibble values name a real MAX31856_TC_TYPE_*) -- so the real "is this
 * actually a recognised thermocouple type" decision is entirely SaftyFW's
 * (config_store_decide_write(), link_task_handle_set_config()).
 *
 * Returns ESP_ERR_INVALID_ARG if tc_type is out of the wire-level range,
 * ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_OK once the
 * broadcast has been handed to the UART (not proof of Pico acceptance).
 * Safe to call from any task, same as safety_link_send_clear_trip(). */
esp_err_t safety_link_send_set_config(SafetyLinkClass *link, uint8_t tc_type);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_LOG_LEVEL (0x1B) --
 * docs/COMMISSIONING.md sec 2's table. Runtime log verbosity on the safety
 * processor, not a staged config field -- unrelated to SET_CONFIG/SET_PARAM
 * despite the neighboring id, so this takes effect immediately on the Pico
 * side (log_task_set_level(), never persisted) and is never ARMED-gated.
 * Nothing on the ESP ever called this before (ROADMAP.md "SET_LOG_LEVEL has
 * a codec and a Pico consumer but no ESP caller" loose end) -- this is that
 * caller, following safety_link_send_set_config()'s exact shape: a local
 * wire-range check the codec itself does not perform, then encode/send.
 *
 * `level` is the same UART_LOG_LEVEL_* scale as safety_link.c's own
 * log-forwarding code (uart_task_ids.h) and SaftyFW's LOG_LEVEL_* (link_task.c
 * link_task_handle_set_log_level() range-checks it again on the Pico side;
 * this is a second, independent check on the wire value, same
 * defense-in-depth as SET_CONFIG's tc_type range check).
 *
 * Returns ESP_ERR_INVALID_ARG if level is out of the wire-level range,
 * ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_OK once the
 * broadcast has been handed to the UART (not proof of Pico acceptance --
 * link_task_handle_set_log_level() never replies on the wire, same
 * fire-and-forget shape as safety_link_send_set_config()). Safe to call from
 * any task. */
esp_err_t safety_link_send_set_log_level(SafetyLinkClass *link, uint8_t level);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_FIRING_CEILING (0x09)
 * is RETIRED (owner decision 2026-09-24: the safety processor is a backup in
 * case the ESP fails and must never run a tighter limit than the ESP's own).
 * The ESP no longer sends this command at all -- there is no
 * safety_link_send_firing_ceiling() any more. The opcode stays reserved (not
 * reused) so an older ESP build or a captured trace naming it still makes
 * sense; SaftyFW still decodes a 0x09 frame if one arrives (from an older
 * ESP), but safety_guards.c's S1 ignores the result and trips at
 * abs_max_temp_c alone regardless. */

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_ROLLBACK (0x17) --
 * tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico half (the ESP
 * half is ota_http.c's POST /api/ota/esp/rollback). Explicit "revert to the
 * previously-running bootloader slot, right now" -- distinct from the
 * automatic boot_attempts fallback, which only fires after the active slot
 * has already failed real CRC checks on its own.
 *
 * No arguments, no local refusal checks (unlike safety_link_send_clear_trip()
 * this driver has no cached state to fail closed against) -- every refusal
 * reason is SaftyFW's: the relay is ARMED, or (the load-bearing property of
 * this whole feature) the OTHER bootloader slot is not currently VALID/
 * PENDING_VERIFY, so a rollback can never strand the board with zero
 * bootable slots. Same fire-and-forget BROADCAST shape as
 * safety_link_send_clear_trip()/safety_link_send_set_config(): the Pico
 * never ACKs this on the wire, so success is observed by watching the link
 * drop and recover with a new boot_id on the next poll, not by a reply here.
 *
 * Returns ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_OK once
 * the broadcast has been handed to the UART (not proof of Pico acceptance).
 * Safe to call from any task, same as safety_link_send_clear_trip(). */
esp_err_t safety_link_send_rollback(SafetyLinkClass *link);

/* Shared bounded-wait/unknown-outcome helper (M15 ROADMAP item "No shared
 * bounded-wait/unknown-outcome helper"). Extracted from safety_link_send_
 * rollback_ex()'s boot_id-reconnect watch, the ONLY place this discipline
 * existed before this helper: poll for positive/negative evidence up to a
 * timeout, and if the timeout elapses with neither, report UNKNOWN --
 * NEVER infer success from silence. This is CommonFW/docs/LINK_PROTOCOL.md's
 * skew rule made reusable: on this link a new ESP can be talking to an old
 * Pico (or vice versa) that never sends the reply a newer peer would, so
 * "no reply" is a routine, expected outcome, not evidence of anything --
 * see LINK_PROTOCOL.md sec 4, "a timeout must never be misreported as
 * success."
 *
 * ANY new command whose only feedback is "did something happen, or did the
 * peer just stay silent" (as opposed to an ordinary safety_exchange() with
 * a single well-defined reply frame) MUST route through this rather than
 * hand-rolling its own poll loop -- a second hand-rolled copy is exactly
 * the drift this extraction exists to prevent.
 *
 * poll_fn is called at poll_interval_ms intervals (immediately on entry,
 * then after each delay) until it returns something other than
 * SAFETY_LINK_AWAIT_PENDING, or until timeout_ms has elapsed since entry --
 * whichever comes first. It receives ctx unchanged; use it to close over
 * whatever the specific command needs to inspect (an inbox stash, a cached
 * peer field, ...) and to do its own outcome-specific logging/output-param
 * writes BEFORE returning ACKED or UNKNOWN -- this helper itself carries no
 * domain knowledge of what "acked" means for a given command, only the
 * wait/poll/timeout shape. Runs on the calling task; callers must not hold
 * a lock their own poll_fn (or the evidence poll_fn is watching for) needs
 * released to make progress -- see safety_link_send_rollback_ex()'s
 * xact_lock release, taken BEFORE this helper is ever called, for exactly
 * that reason. */
typedef enum {
    SAFETY_LINK_AWAIT_PENDING = 0, /* poll_fn: no verdict yet, keep waiting */
    SAFETY_LINK_AWAIT_ACKED,       /* poll_fn: positive evidence observed, stop now */
    SAFETY_LINK_AWAIT_UNKNOWN,     /* poll_fn: terminal negative/inconclusive evidence
                                     * observed, stop now -- also the ONLY value this
                                     * helper itself returns on a plain timeout */
} safety_link_await_poll_t;

typedef safety_link_await_poll_t (*safety_link_await_poll_fn)(void *ctx);
typedef safety_link_await_poll_t safety_link_await_result_t; /* PENDING never returned by the helper itself */

/* The honest outcome of safety_link_send_rollback_ex() below -- see that
 * function's own doc comment for the full reasoning. Five states, not a
 * bool, because "no reply" is ambiguous on this link and this driver must
 * never collapse it into a false "it worked": */
typedef enum {
    /* The link was already down (safety_link_get_status()'s link_up was
     * false) before anything was sent -- the request never reached the
     * Pico, or if it did, no delivery could be confirmed either way. This
     * is checked and reported BEFORE sending, distinct from a timeout that
     * occurs after a healthy-looking send, so a caller can never mistake
     * "the link was down the whole time" for "the Pico is thinking about
     * it." */
    SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN = 0,
    /* uart_protocol_send_broadcast() itself failed locally (a UART fault,
     * not a peer response) -- the request never left this board. */
    SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED,
    /* A SAFETY_CMD_ROLLBACK_RESULT (0x25) frame arrived with accepted == 0
     * -- an explicit, wire-confirmed refusal. *out_reason_code is a
     * kilnlink_rollback_result_reason_t value. This can now surface EITHER
     * from the initial send-burst/reply-window wait, or from the boot_id
     * watch below discovering a refusal that was stashed after arriving
     * late (safety_drain_inbox_ex()'s SAFETY_CMD_ROLLBACK_RESULT stash
     * branch) -- either way it is still a wire-confirmed refusal, not an
     * inference. */
    SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED,
    /* No refusal ever arrived, and the peer's boot_id (safety_apply_fw_
     * version()'s pico_boot_id/pico_boot_id_known, the SAME tracking that
     * already drives the ordinary "Pico rebooted, re-announce" path) has
     * changed from what it was immediately before this request was sent.
     * This is the ONLY positive evidence this driver accepts for ACCEPTED
     * -- see kilnlink_rollback_result.h's "ASYMMETRIC BY DESIGN" comment:
     * "acceptance is something inferred from the ABSENCE of this frame PLUS
     * a link drop-and-reconnect with a new boot_id -- never as something
     * this frame itself reports." A version-compatibility guess is
     * deliberately NOT part of this decision (removed 2026-08-3x): whether
     * the PICO sends 0x25 is gated by the Pico's own cached view of the
     * ESP's protocol_version, a value this ESP cannot observe, so no
     * version read on this side could ever stand in for it. A boot_id
     * change is direction-agnostic positive evidence and needs no such
     * proxy.
     *
     * A boot_id change ALONE is not enough (opus-review finding 2, fixed
     * 2026-08-3x): an unrelated Pico crash/watchdog/power-glitch reboot
     * inside the watch window changes the boot_id exactly like a rollback
     * does. This outcome additionally requires the peer's build identity
     * (commit+datetime, from the same FW_VERSION frame) to have changed too
     * -- see safety_link_rollback_reboot_confirmed()/safety_link_rollback_
     * build_identity_changed() above. An unrelated reboot comes back
     * running the SAME image, so it fails this second test and falls
     * through to UNKNOWN_TIMEOUT instead of being misreported as ACCEPTED. */
    SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED,
    /* Neither a refusal nor a boot_id change was observed within this
     * driver's full wait budget (the send-burst/reply window, plus the
     * boot_id-reconnect watch that follows it). Absence of both is
     * UNINFORMATIVE: it could be a request or reply frame lost on the wire,
     * a refusal that reached only the Pico's own local log, a peer too busy
     * to answer inside the window, or the link simply staying down for the
     * whole watch. Reported as its own state rather than folded into either
     * ACCEPTED or REFUSED -- see CommonFW/docs/LINK_PROTOCOL.md sec 4's
     * entry on this frame: "a timeout must never be misreported as
     * success," and it must equally never be misreported as a refusal. */
    SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT,
} safety_link_rollback_outcome_t;

/* Did the peer's boot_id actually CHANGE, relative to the snapshot taken
 * immediately before the rollback request went out? Split out of safety_
 * link_send_rollback_ex()'s watch loop so the baseline rule below is
 * host-testable (test_safety_link.c) rather than living only inside a
 * FreeRTOS polling loop no host test can drive.
 *
 * The rule that matters: if the boot_id was NOT known before the request
 * (`had_before` false) there is NO baseline, so "known now" is not evidence
 * of anything -- it means only that a FW_VERSION frame finally arrived,
 * which happens routinely on a lossy link while the Pico keeps running (the
 * poll task re-requests GET_FW_VERSION every poll period for exactly as
 * long as the version stays unknown). Reporting that as a reboot would
 * re-create the very "silence plus an unrelated event is read as success"
 * defect this whole path exists to remove -- and would do it precisely on a
 * flaky link, the case the fix is for. Unknown-before therefore yields
 * false (no evidence), never true.
 *
 * NOTE this is deliberately NOT the same test as safety_apply_fw_version()'s
 * own boot_id_changed, which treats unknown-before as changed: there,
 * "changed" only triggers conservative housekeeping (re-announce our
 * version, drop trip-dedup state), so erring toward "changed" is safe. Here
 * it manufactures positive evidence for ACCEPTED, so it must err the other
 * way. */
static inline bool safety_link_rollback_boot_id_changed(bool had_before, uint8_t before,
                                                         bool known_now, uint8_t now)
{
    if (!had_before || !known_now) {
        return false;
    }
    return now != before;
}

/* Opus-review finding 2, "a reboot is not a rollback": a boot_id change
 * alone is NOT proof of a rollback -- an unrelated Pico crash, watchdog
 * reset, or power glitch inside the boot_id watch window changes the
 * boot_id exactly the same way a rollback reboot does. FW_VERSION already
 * carries the peer's build commit + datetime (safety_apply_fw_version(),
 * safety_link.c), so this side has independent evidence a boot_id change
 * alone does not: a rollback reboots into the OTHER bootloader slot, which
 * (barring the degenerate case of rolling back to an identical rebuild) has
 * a DIFFERENT build identity, while an unrelated reboot comes back running
 * the SAME image it was already running. Same "no baseline, no evidence"
 * convention as safety_link_rollback_boot_id_changed() just above --
 * peer_build_known being false either before or after the snapshot means
 * there is nothing to compare, so this reports false (never fabricate
 * evidence) rather than guessing.
 *
 * Compares length-then-bytes rather than assuming a fixed width: the wire
 * strings are not null-terminated (safety_link_get_peer_build_status()'s own
 * doc comment) and a shorter/longer string on either side is itself a
 * difference, not something memcmp over a mismatched length could report
 * safely. commit_before/commit_now and datetime_before/datetime_now must
 * each have room for at least commit_len_before/commit_len_now and
 * datetime_len_before/datetime_len_now bytes respectively (the *_len values
 * themselves come from safety_link_get_peer_build_status(), which never
 * reports a length larger than its buffers). */
static inline bool safety_link_rollback_build_identity_changed(
    bool had_before, uint8_t commit_len_before, const uint8_t *commit_before,
    uint8_t datetime_len_before, const uint8_t *datetime_before, bool known_now,
    uint8_t commit_len_now, const uint8_t *commit_now, uint8_t datetime_len_now,
    const uint8_t *datetime_now)
{
    if (!had_before || !known_now) {
        return false;
    }
    if (commit_len_before != commit_len_now || datetime_len_before != datetime_len_now) {
        return true;
    }
    if (commit_len_before > 0 && memcmp(commit_before, commit_now, commit_len_before) != 0) {
        return true;
    }
    if (datetime_len_before > 0 && memcmp(datetime_before, datetime_now, datetime_len_before) != 0) {
        return true;
    }
    return false;
}

/* The full positive-evidence test for ACCEPTED -- BOTH the boot_id AND the
 * build identity must have changed (opus-review finding 2). Extracted as
 * its own pure function, same host-testability reasoning as the two
 * *_changed() helpers above, so this AND is exercised directly
 * (test_safety_link.c) rather than only inline at the one call site that
 * currently uses it. */
static inline bool safety_link_rollback_reboot_confirmed(bool boot_id_changed, bool build_identity_changed)
{
    return boot_id_changed && build_identity_changed;
}

/* Pure decision at the heart of safety_link_send_rollback_ex(): given what
 * this driver actually observed on the wire, which outcome is honest to
 * report? Extracted as a `static inline` (no FreeRTOS/hardware dependency)
 * for the same reason safety_link_is_stale()/safety_drain_still_waiting()
 * above are -- so the inference itself is host-testable in isolation from
 * the send-burst timing and the boot_id-watch polling loop that feed it
 * (see test_safety_link.c).
 *
 * `refusal_received`: a SAFETY_CMD_ROLLBACK_RESULT (0x25) frame was
 * obtained, either during the initial reply window or later out of the
 * late-refusal stash.
 * `refusal_decoded_ok`: meaningless unless refusal_received is true --
 * whether kilnlink_rollback_result_decode() actually parsed it. A frame
 * that matched the id/length gate but failed the codec's own checks proves
 * nothing, so it is treated the same as no evidence at all rather than as a
 * refusal.
 * `boot_id_changed`: the peer's boot_id (safety_apply_fw_version()'s
 * pico_boot_id) differs from what it was immediately before the request was
 * sent -- the ONLY positive evidence for ACCEPTED, per kilnlink_rollback_
 * result.h's "ASYMMETRIC BY DESIGN" comment. Ignored when a refusal was
 * received: a wire-confirmed refusal always wins over an unrelated reboot
 * that happened to coincide with it.
 *
 * A caller that has neither a refusal nor a boot_id change yet, and has not
 * exhausted its wait budget, must not call this at all -- there is no
 * "still waiting" return value on purpose, since this function only ever
 * runs once a caller has already decided to stop waiting (a wire result
 * showed up, or the watch window closed). */
static inline safety_link_rollback_outcome_t safety_link_rollback_infer_outcome(bool refusal_received,
                                                                                 bool refusal_decoded_ok,
                                                                                 bool boot_id_changed)
{
    if (refusal_received) {
        return refusal_decoded_ok ? SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED
                                   : SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT;
    }
    if (boot_id_changed) {
        return SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED;
    }
    return SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT;
}

/* The Pico half of a rollback request, WITH an honest outcome -- the
 * observable half of ota_http.c's POST /api/ota/pico/rollback. Unlike
 * safety_link_send_rollback() above (fire-and-forget, kept unchanged for
 * uart_bridge.c's existing PC-link caller), this function:
 *   1. snapshots the peer's boot_id before sending anything;
 *   2. sends the request as a small burst (SAFETY_LINK_ROLLBACK_SEND_
 *      REPEATS, safety_link.c), same loss-tolerance precedent as
 *      safety_link_send_announce_version_burst(), listening for a
 *      SAFETY_CMD_ROLLBACK_RESULT (0x25) refusal between/after each send;
 *   3. if no refusal arrived, watches for the peer's boot_id to change --
 *      the ONLY accepted evidence of a completed reboot -- for up to
 *      SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS, still checking for a late-
 *      arriving refusal (the stashed-frame mechanism SafetyLinkClass::
 *      stashed_rollback_result documents) on every poll of that watch.
 * classifying what happened into `*out_outcome` via safety_link_rollback_
 * infer_outcome() -- see safety_link_rollback_outcome_t's own doc comment
 * for the five states and why a plain bool cannot represent this honestly.
 *
 * `out_reason_code` is filled (kilnlink_rollback_result_reason_t) only when
 * `*out_outcome == SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED`; untouched
 * otherwise. Both out-params are optional (NULL-tolerant).
 *
 * Returns ESP_ERR_INVALID_ARG/ESP_ERR_INVALID_STATE for the usual reasons
 * (NULL link / not initialized). Otherwise always returns ESP_OK once
 * `*out_outcome` has been filled in -- the protocol-level result belongs in
 * `*out_outcome`, not the return value, since "the Pico refused" is not a
 * driver-level failure the way a bad argument is. Blocks the calling task
 * for up to roughly SAFETY_LINK_ROLLBACK_SEND_REPEATS *
 * SAFETY_LINK_ROLLBACK_SEND_REPEAT_GAP_MS + SAFETY_LINK_REPLY_TIMEOUT_MS +
 * SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS in the worst case (no refusal, no
 * reconnect) -- ota_http.c's caller runs this from an HTTP handler task, not
 * the poll task, precisely so this long a block cannot starve GET_STATUS
 * polling; see safety_link.c's own comment on releasing xact_lock before the
 * boot_id watch for why the poll task keeps running normally throughout. */
esp_err_t safety_link_send_rollback_ex(SafetyLinkClass *link, safety_link_rollback_outcome_t *out_outcome,
                                        uint8_t *out_reason_code);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_ANNOUNCE_REBOOT (0x18) --
 * KilnFW TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP reboots"
 * line. Fire-and-forget notice sent immediately before an OTA self-update's
 * esp_restart() (see ota_http.c's OTA_HTTP_CONTEXT_ESP delayed-reboot task)
 * so the several seconds of silence a routine reboot necessarily causes does
 * not look identical, from the Pico's side, to a crashed main controller.
 *
 * This is advisory only and grants NO permission to heat: SaftyFW's S6b
 * guard (SAFETY_MODEL.md section 4) suppresses its own trip for a bounded
 * window after receipt, nothing more -- it does not touch relay_owner's
 * energize/ARM logic, does not suppress any other guard, and if the ESP is
 * still silent once the window expires, S6b trips exactly as if this frame
 * had never been sent. See safety_guards.c's S6b block and safety_core.c for
 * the window arithmetic; this driver has no say in any of it.
 *
 * No arguments, no local refusal checks -- there is nothing here to fail
 * closed against; this is a courtesy notice, not a request the Pico can
 * refuse. Same fire-and-forget BROADCAST shape as
 * safety_link_send_clear_trip()/safety_link_send_rollback(): the Pico never
 * ACKs this on the wire (link_task.c never replies to it), so there is no
 * outcome to observe here at all -- by design, since the ESP is about to
 * stop listening anyway.
 *
 * Returns ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_OK once
 * the broadcast has been handed to the UART. Safe to call from any task,
 * same as safety_link_send_clear_trip(). Callers MUST send this before
 * calling esp_restart(), not after -- there is no way to recover a reboot
 * that has already begun. */
esp_err_t safety_link_send_announce_reboot(SafetyLinkClass *link);

/* Public (moved out of safety_link_commands.c 2026-09-23) so callers that
 * must size a bound AROUND safety_link_send_reboot()'s own worst case --
 * today, sw_reset_http.c's delayed-reboot task arm-wait -- derive it from
 * this constant instead of a second, independently hardcoded number that
 * can silently drift out of sync with it (exactly what happened the first
 * time: the arm-wait was sized only for the ~345 ms ordinary reply window,
 * before this fallback watch existed, and was never revisited when it was
 * added). See safety_link_send_reboot()'s own doc comment below for what
 * this bounds and why. */
#define SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS 3000u
#define SAFETY_LINK_REBOOT_BOOT_ID_WATCH_POLL_MS 200u

/* The honest outcome of safety_link_send_reboot() below. Deliberately
 * distinguishes "the Pico said yes" from "nobody answered" -- silence is
 * NEVER folded into success here, the same discipline
 * safety_link_rollback_outcome_t documents at length for the rollback path
 * (and for the same reason: a link that was simply down looks exactly like
 * a peer that accepted, if you only measure whether a refusal arrived). */
typedef enum {
    /* The Pico decoded SAFETY_CMD_REBOOT, its ARMED gate passed, and it
     * replied SAFETY_CMD_REBOOT_RESULT with accepted=1. This means "accepted
     * and about to reset", NOT "finished rebooting" -- the reply necessarily
     * leaves before the reset happens (kilnlink_reboot_result.h). */
    SAFETY_LINK_REBOOT_OUTCOME_ACCEPTED = 0,
    /* The Pico replied accepted=0. `out_reason_code` carries the
     * kilnlink_reboot_result_reason_t (today: ARMED). */
    SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
    /* The link was down before the request was even sent -- nothing left
     * this board. Distinguished from NO_REPLY because it is diagnosable
     * without involving the peer at all. */
    SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN,
    /* The request could not be encoded, or the UART refused it locally. */
    SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED,
    /* The request went out and nothing came back within the reply window,
     * AND the bounded boot_id fallback watch below (safety_link_send_
     * reboot()'s doc comment) never observed the peer's boot_id change
     * either. The honest reading: UNKNOWN. Either the Pico predates this
     * command (its dispatch has no case for 0x29 and it silently dropped
     * it), or the reply was lost, or the link died in between. Callers must
     * report this as "not confirmed", never as a reboot that happened. */
    SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY,
    /* 2026-09-23 fallback: the request went out, no SAFETY_CMD_REBOOT_RESULT
     * (0x2A) ever arrived (bench evidence, 2026-09-23: the Pico's link task
     * only drains its TX ring for LINK_TASK_REBOOT_TX_DRAIN_MS = 10ms before
     * calling hal_wdt_reboot() -- firmware/SaftyFW/src/tasks/link_task.c, not
     * edited here -- so the reply frame is routinely still queued, not yet on
     * the wire, when the reboot actually happens; the Pico DOES reboot in
     * this case, this driver simply never hears about it), BUT the peer's
     * boot_id (safety_apply_fw_version()'s pico_boot_id, the SAME tracking
     * safety_link_send_rollback_ex() already trusts) changed within
     * safety_link_send_reboot()'s bounded fallback watch after the ordinary
     * reply window closed. Unlike the rollback path this does NOT also
     * require the peer's build identity to change: a reboot-in-place command
     * requests the SAME firmware image back, so there is no "which slot"
     * ambiguity for a build-identity check to resolve here, and a boot_id
     * change alone is the one and only observable consequence a genuine
     * accepted-but-unacknowledged reboot leaves behind. `out_reason_code` is
     * meaningless for this outcome (no refusal was ever decoded). `out_
     * boot_id_before`/`out_boot_id_after`, if non-NULL, are filled with the
     * peer's boot_id immediately before the request and at the moment the
     * change was observed, so the caller can name both values rather than
     * asserting the change happened. NOTE this is still not proof the
     * REQUESTED reboot specifically is what changed the boot_id -- an
     * unrelated Pico crash/watchdog/power-glitch reboot inside the same
     * bounded watch window would look identical, exactly the ambiguity
     * safety_link_rollback_outcome_t's ACCEPTED discusses at length for the
     * rollback path. Accepted here anyway, per this outcome's own use case
     * (an operator-triggered reboot the Pico was already expected to take),
     * rather than left unreported. */
    SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID,
} safety_link_reboot_outcome_t;

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_REBOOT (0x29) / its reply
 * SAFETY_CMD_REBOOT_RESULT (0x2A) -- the genuine "safety processor, reboot
 * yourself in place, into the SAME firmware slot" command, the Pico half of
 * this driver's own sw_reset_http.c (POST /api/sw_reset).
 *
 * NOT safety_link_send_rollback[_ex]() above: a rollback marks the running
 * slot BAD, writes bootloader metadata, and comes back on a DIFFERENT,
 * possibly-refused image. NOT safety_link_send_announce_reboot() either:
 * that is a courtesy notice about the ESP's own reboot that asks the Pico to
 * do nothing at all. This command changes no image and no configuration --
 * see update_task_reboot_now() (SaftyFW) for the "touches no configuration"
 * proof, which is a property of that function's whole body.
 *
 * One round trip, structured like safety_link_get_ct_auto_zero_status():
 * take xact_lock, clear any stale stash, send the 1-byte BROADCAST, wait up
 * to SAFETY_LINK_REPLY_TIMEOUT_MS, then read the stash. This frame is ACKed
 * on acceptance (kilnlink_reboot_result.h's "SYMMETRIC" comment), so a
 * positive answer normally is either on the wire or it did not happen --
 * unlike the rollback path there is no PROTOCOL reason to ever infer
 * acceptance from silence here.
 *
 * 2026-09-23 bench fallback: bench evidence the same day showed the Pico
 * DOES reboot even when this driver never sees 0x2A -- SaftyFW's link task
 * only drains its TX ring for 10ms before calling hal_wdt_reboot()
 * (firmware/SaftyFW/src/tasks/link_task.c, LINK_TASK_REBOOT_TX_DRAIN_MS, not
 * edited by this change), so a queued-but-not-yet-transmitted reply is
 * routinely lost to the reset itself. xact_lock is released after the
 * ordinary reply window (same reasoning as safety_link_send_rollback_ex()'s
 * own release before its boot_id watch -- holding it here would block the
 * GET_STATUS poll that is the only thing keeping pico_boot_id current), and
 * ONLY on NO_REPLY, this function then watches the peer's boot_id (already
 * snapshotted before the request was sent) for up to
 * SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS for a change, polling every
 * SAFETY_LINK_REBOOT_BOOT_ID_WATCH_POLL_MS -- see safety_link_reboot_
 * outcome_t's SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID for what a
 * change during that watch means and does not mean. A timeout of that watch
 * with no change falls through to plain NO_REPLY, same as before this
 * fallback existed.
 *
 * `out_outcome` is required and always written. `out_reason_code`, if
 * non-NULL, is filled with the peer's kilnlink_reboot_result_reason_t on
 * REFUSED only. `out_boot_id_before`/`out_boot_id_after`, if non-NULL, are
 * filled only on CONFIRMED_BY_BOOT_ID (see that outcome's own doc comment);
 * left untouched otherwise. Returns ESP_ERR_INVALID_ARG/ESP_ERR_INVALID_STATE
 * for the usual local misuse, otherwise ESP_OK with the real answer in
 * `*out_outcome` -- an unreachable or refusing peer is not an error of this
 * function, it is an outcome. Blocks the calling task for up to roughly
 * SAFETY_LINK_REPLY_TIMEOUT_MS plus, only on the no-reply path,
 * SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS more -- see sw_reset_http.c's
 * sw_reset_post_handler() comment at its call site (starting "Pico half
 * next") for the real worst-case total, why it occupies the single httpd
 * worker for that long, and why that is accepted. */
esp_err_t safety_link_send_reboot(SafetyLinkClass *link, safety_link_reboot_outcome_t *out_outcome,
                                   uint8_t *out_reason_code, uint8_t *out_boot_id_before,
                                   uint8_t *out_boot_id_after);

/* Pure decision at the heart of safety_link_send_reboot()'s NO_REPLY boot_id
 * fallback watch -- extracted the same way safety_link_rollback_infer_
 * outcome() is, so it is host-testable (test_safety_link.c) independent of
 * the FreeRTOS polling loop that feeds it. `boot_id_changed` must already
 * have been computed by safety_link_rollback_boot_id_changed() (reused
 * as-is: it is a pure had-baseline/before/known-now/now comparison with
 * nothing rollback-specific about its logic, only its name) against the
 * boot_id snapshotted immediately before the reboot request was sent. See
 * SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID's own doc comment for why
 * no build-identity check is needed here the way the rollback path needs
 * one. */
static inline safety_link_reboot_outcome_t safety_link_reboot_infer_boot_id_outcome(bool boot_id_changed)
{
    return boot_id_changed ? SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID
                            : SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY;
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_CT_CAL (0x19) -- the
 * GUI/bench-tool's path to commissioning one channel of SaftyFW's
 * config_store.h ct_cal record (firmware/SimFW/tools/ct_calibration/
 * push_ct_cal.py's SET_CT_CAL step, its own doc comment: "no path exists to
 * push calibration constants back into SaftyFW's own flash" -- this closes
 * that gap on the KilnFW side). Same fire-and-forget BROADCAST shape as
 * safety_link_send_set_config(): the Pico's link_task.c never ACKs this on
 * the wire, so there is no reply to wait for here -- success is observed by
 * the caller polling GET_CT_CAL afterward, same "outcome via telemetry"
 * pattern as everything else in this section.
 *
 * `channel` is validated locally against KILNLINK_SET_CT_CAL_NUM_CHANNELS (3)
 * -- the wire-level range every sender of this frame agrees on
 * (push_ct_cal.py's encode_set_ct_cal() performs the identical check on its
 * side) -- same "catch it here, not just on the Pico" discipline
 * safety_link_send_set_config() uses for tc_type. An uncalibrated channel
 * sends explicit gain=0/offset=0 regardless of what the caller passed,
 * mirroring push_ct_cal.py's own "belt and suspenders against stale numbers"
 * choice, so a caller cannot accidentally leave a previous calibrated
 * channel's numbers on the wire under calibrated=false.
 *
 * Returns ESP_ERR_INVALID_ARG if channel is out of range, ESP_ERR_INVALID_STATE
 * if the driver isn't initialized, ESP_OK once the broadcast has been handed
 * to the UART (not proof of Pico acceptance). Safe to call from any task,
 * same as safety_link_send_set_config(). */
esp_err_t safety_link_send_set_ct_cal(SafetyLinkClass *link, uint8_t channel, bool calibrated,
                                       float gain, float offset);

/* CommonFW/docs/LINK_PROTOCOL.md sec 4/6, SAFETY_CMD_GET_CT_CAL (0x22
 * request) / SAFETY_CMD_CT_CAL (0x1A reply -- separate ids since
 * KILNLINK_PROTOCOL_VERSION 7) -- kilnlink_get_ct_cal.h/kilnlink_ct_cal.h.
 * Unlike every other safety_link_build_*_payload()/safety_link_get_status()
 * in this file, this is NOT answered from a cache: this driver keeps no
 * ct_cal state of its own, so every call is a live, blocking round trip to
 * the Pico -- same "request now, wait for the specific reply, worst case
 * ~SAFETY_LINK_ACK_TIMEOUT_MS * UART_PROTO_MAX_RETRIES + SAFETY_LINK_REPLY_
 * TIMEOUT_MS with no peer" caveat safety_link_ping()/safety_link_request_
 * enable() already carry. Call it from a bridge/app task, never from
 * anything latency-critical.
 *
 * On success, copies the Pico's raw CT_CAL reply (byte-for-byte, cmd byte
 * included) into `out` and sets *out_len -- the ESP relays this frame to the
 * PC unmodified rather than decoding/re-encoding it, since the PC-facing
 * SAFETY_CMD_CT_CAL reply and the Pico's own CT_CAL frame share the exact
 * same 28-byte layout (kilnlink_ct_cal.h's own doc comment). `out_cap` must
 * be at least KILNLINK_CT_CAL_LEN (28) bytes.
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL/too-small `out`,
 * ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_ERR_TIMEOUT if
 * the request was never ACKed or no CT_CAL reply arrived within
 * SAFETY_LINK_REPLY_TIMEOUT_MS, ESP_OK with *out_len == KILNLINK_CT_CAL_LEN
 * on success. */
esp_err_t safety_link_get_ct_cal(SafetyLinkClass *link, uint8_t *out, size_t out_cap,
                                  size_t *out_len);

/* SAFETY_CMD_GET_STACK_MARGIN (0x2B) / SAFETY_CMD_STACK_MARGIN (0x2C reply),
 * KILNLINK_PROTOCOL_VERSION 14 (payload grew a last_tick_ms freshness field
 * 13 -> 14) -- docs/audits/saftyfw_live_stack_reporting_design_2026-09-11.md
 * and its impl audit docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md.
 *
 * Same request/reply/stash shape as safety_link_send_reboot() (one caller,
 * serialized by xact_lock, reply always stashed rather than out-param
 * plumbed -- see stashed_stack_margin's own comment above), NOT the CT_CAL
 * shape (that one threads a want/got pair through safety_drain_inbox_ex()).
 * Live, blocking round trip -- call from a bridge/app task (e.g. a future
 * GET /api/saftyfw_stack_margin handler), never from anything latency-
 * critical. This is expected to be polled SLOWLY (this data changes on the
 * order of minutes/boots, not the ~1 Hz DIAG frame) -- there is no benefit
 * to polling it any faster, and every call is a live round trip with no
 * cache of its own.
 *
 * On success, decodes the Pico's KILNLINK_STACK_MARGIN_CMD reply into
 * `out` (kilnlink_stack_margin_t, kilnlink_stack_margin.h). Units: WORDS,
 * not bytes -- see that header's own "UNITS" section; ESP-IDF's own
 * get_stack_margin() reports BYTES, so do not compare the two processors'
 * numbers without converting one of them.
 *
 * FLOOR, NOT WORST CASE: every value is the tightest margin OBSERVED since
 * the Pico's own boot, on whatever code paths it has actually taken --
 * check `out->rounds_completed` before trusting every entry (0 means the
 * poller has not finished its first full round-robin cycle and at least
 * one entry is still KILNLINK_STACK_MARGIN_UNMEASURED). Any surfaced
 * rendering of this data must repeat this caveat in its own text.
 *
 * FRESHNESS: `rounds_completed` saturates at 255 within ~2 minutes of Pico
 * uptime and cannot indicate staleness past that point. Use `out->
 * last_tick_ms` instead -- compare it across two calls; if it has not
 * advanced (allowing for one u32 wrap), the poller has stalled. See
 * kilnlink_stack_margin.h's own "last_tick_ms" section for the full
 * contract.
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL link/out, ESP_ERR_INVALID_STATE if
 * the driver isn't initialized, ESP_ERR_TIMEOUT if the request was never
 * ACKed or no STACK_MARGIN reply arrived within SAFETY_LINK_REPLY_TIMEOUT_MS
 * (including a peer built before protocol 13, which has no dispatch case
 * for 0x2B at all and never replies -- silence, never a fabricated zero
 * reading), ESP_FAIL if a reply arrived but failed to decode, ESP_OK on
 * success. */
esp_err_t safety_link_get_stack_margin(SafetyLinkClass *link, kilnlink_stack_margin_t *out);

/* SAFETY_CMD_GET_PARAM (0x23) / SAFETY_CMD_PARAM (0x1E reply -- kilnlink_
 * param.h's KILNLINK_PARAM_CMD), KILNLINK_PROTOCOL_VERSION 7 -- docs/
 * COMMISSIONING.md sec 2. Same request/reply/stash shape as safety_link_
 * get_stack_margin() above (one caller, serialized by xact_lock, reply
 * always stashed rather than out-param plumbed -- see stashed_param's own
 * comment above), NOT the CT_CAL shape. Unlike GET_STACK_MARGIN this is NOT
 * decoded here: like safety_link_get_ct_cal(), the raw PARAM reply (cmd byte
 * included, KILNLINK_PARAM_HDR_LEN..KILNLINK_PARAM_MAX_LEN bytes) is copied
 * into `out` verbatim so the PC-facing bridge case can relay it unmodified,
 * except that the returned param_id (bytes 1-2) is checked against the one
 * requested before this function hands the frame back -- a reply belonging
 * to a different param_id (e.g. a very late stash from a prior request that
 * survived the stale-take below by a hair) must never be handed out as this
 * call's answer.
 *
 * `out_cap` must be at least KILNLINK_PARAM_MAX_LEN (9) bytes. `found == 0`
 * in the decoded reply means the Pico's build does not recognise this
 * param_id -- that is a normal, valid answer (KILNLINK_PARAM_HDR_LEN bytes,
 * ESP_OK), not an error; the caller (the PC-facing bridge case) relays it
 * exactly as the CT_CAL case relays a driver reply, letting the far end see
 * `found`.
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL link/out or too-small out_cap,
 * ESP_ERR_INVALID_STATE if the driver isn't initialized, ESP_ERR_TIMEOUT if
 * the request was never ACKed or no PARAM reply arrived within SAFETY_LINK_
 * REPLY_TIMEOUT_MS, ESP_FAIL if a PARAM-shaped frame arrived but its
 * param_id did not match the one requested, ESP_OK with *out_len set on
 * success. */
esp_err_t safety_link_get_param(SafetyLinkClass *link, uint16_t param_id, uint8_t *out, size_t out_cap,
                                 size_t *out_len);

/* CT_COMMISSIONING_PLAN.md step 2 -- SAFETY_CMD_CT_AUTO_ZERO_BEGIN (0x26),
 * fire-and-forget, same contract as safety_link_send_set_ct_cal(): only
 * arms the Pico's accumulator (current_task.c), never blocks waiting for a
 * measurement. Returns ESP_ERR_INVALID_ARG for channel out of range or a
 * NULL link, ESP_ERR_INVALID_STATE if not initialized, otherwise whatever
 * uart_protocol_send_broadcast() reports (a local send outcome, not proof
 * the Pico accepted the arm -- follow up with safety_link_get_ct_auto_zero_
 * status()). */
esp_err_t safety_link_send_ct_auto_zero_begin(SafetyLinkClass *link, uint8_t channel);

/* CT_COMMISSIONING_PLAN.md step 2 -- SAFETY_CMD_GET_CT_AUTO_ZERO (0x27) /
 * SAFETY_CMD_CT_AUTO_ZERO_STATUS (0x28 reply). One bounded round trip
 * (SAFETY_LINK_REPLY_TIMEOUT_MS); the caller polls this repeatedly across
 * the multi-second measurement rather than this function blocking for all
 * of it. Returns ESP_ERR_INVALID_ARG for a NULL link/out, ESP_ERR_INVALID_
 * STATE if not initialized, ESP_ERR_TIMEOUT if no STATUS reply arrived,
 * ESP_OK with `*out` filled in on success (check out->state). */
esp_err_t safety_link_get_ct_auto_zero_status(SafetyLinkClass *link,
                                               kilnlink_ct_auto_zero_status_t *out);

/* docs/COMMISSIONING.md sec 2/3 -- SAFETY_CMD_SET_PARAM (0x1C) / COMMIT_CONFIG
 * (0x1D) / GET_CONFIG_PAGE (0x1F). App/drivers/safety/safety_cfg_store.c (the
 * NVS-backed ESP-side cache) and safety_cfg_http.c (the /api/safety/
 * commissioning handlers) are the only intended callers -- see safety_link.c's
 * definitions for the full contract.
 *
 * safety_link_send_commit_config()'s return value is still a PROTOCOL-level
 * ACK/NACK only, but a rejected commit is no longer indistinguishable from an
 * accepted one: SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20,
 * kilnlink_commit_config_rejected.h) is a real reply now (ROADMAP.md "no wire
 * codec carries a per-field COMMIT_CONFIG rejection reason back to the ESP"
 * loose end, closed). `out_param_id`/`out_reason`/`out_rejected` are all
 * optional (pass NULL for any/all to ignore); when the Pico's own commit
 * handler refuses, *out_rejected is set true, and *out_param_id together
 * with *out_reason carry the offending field's wire id (or
 * KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID) and a
 * kilnlink_commit_config_reject_reason_t value. (Write that pair as
 * "*out_param_id together with *out_reason", never as "id/" directly
 * followed by "*out_reason": the resulting slash-star opens a nested
 * comment, and -Werror=comment fails the build on it. That is the same
 * class of typo that silently broke the thermocouple faults page's inline
 * script, where it cost far more to find.) safety_cfg_http.c's
 * commissioning_post_handler() is the intended consumer, surfacing this in
 * the HTTP response's error text; bench_preset's own call site passes
 * NULL/NULL/NULL, since COMMISSIONING.md sec 4.1's bench values are not
 * expected to be rejected by real commissioning rules.
 * safety_link_send_set_param() ACK'd-unicast-stages one field;
 * safety_link_send_commit_config() ACK'd-unicast-validates-and-writes the
 * whole staged set; safety_link_get_config_page() is a live, blocking round
 * trip for one page of the bulk readback (never cached inside this driver,
 * same split as safety_link_get_ct_cal() above). All three block for the
 * exchange (same worst-case caveat as safety_link_ping()) -- call from a
 * bridge/app task, never anything latency-critical. */
esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value);
esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id,
                                          uint8_t *out_reason, bool *out_rejected);

/* SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D, docs/KILN_PROFILES_PLAN.md item 15,
 * kilnlink_apply_config_volatile.h) -- the RAM-only sibling of
 * safety_link_send_commit_config() above. Same shape, byte for byte: same
 * xact_lock hold, same pre-send drain, same BROADCAST send (the Pico's
 * link_task_handle_raw_frame() drops anything else, identical reasoning to
 * commit_config's own comment), same "wait SAFETY_LINK_REPLY_TIMEOUT_MS for a
 * possible COMMIT_CONFIG_REJECTED reply" window -- link_task_handle_apply_
 * config_volatile() on the Pico reuses link_task_send_commit_config_
 * rejected() verbatim for its own validation-refusal reply, so this side's
 * rejection handling (out_param_id/out_reason/out_rejected, and the stash
 * safety_link_take_stashed_commit_rejected() drains) is identical to
 * COMMIT_CONFIG's.
 *
 * The ONLY difference from safety_link_send_commit_config() is which command
 * byte goes on the wire and which validation-independent outcome it causes
 * on the Pico: this one installs into the Pico's live RAM record
 * (config_store_write_volatile()) and can NEVER be refused for ARMED --
 * config_store_write_volatile() does not call config_store_decide_write() at
 * all (see that function's own header comment), so there is no ARMED
 * rejection reason this call can ever report. It also never reaches flash,
 * so it does not survive a Pico reboot -- callers that need the change to
 * survive a reboot must separately persist it via safety_link_send_commit_
 * config() when/if that is expected to succeed (kiln_cfg_swap.c's
 * best-effort flash-fallback step is the intended caller of that half). */
esp_err_t safety_link_send_apply_config_volatile(SafetyLinkClass *link, uint16_t *out_param_id,
                                                  uint8_t *out_reason, bool *out_rejected);

/* Consumes a COMMIT_CONFIG_REJECTED frame that arrived too late for
 * safety_link_send_commit_config()'s own reply window and was stashed
 * instead of dropped (SafetyLinkClass::stashed_commit_rejected). Returns
 * false (out-params untouched) if nothing is stashed. See
 * safety_cfg_http.c's apply_pairs() for the intended use: attaching the
 * Pico's own reason to a failure a live read-back already established. */
bool safety_link_take_stashed_commit_rejected(SafetyLinkClass *link, uint16_t *out_param_id,
                                               uint8_t *out_reason);
esp_err_t safety_link_get_config_page(SafetyLinkClass *link, uint8_t page_index,
                                       kilnlink_config_page_t *out);

/* Drops any CONFIG_PAGE currently held in the stash. Call this when a held
 * page could no longer belong to the configuration being fetched -- the config
 * store calls it whenever the peer's reported config CRC changes. The stash is
 * otherwise long-lived on purpose (SAFETY_STASHED_PAGE_MAX_AGE_MS), so this is
 * what keeps it from ever serving a page from a configuration that is gone. */
void safety_link_clear_stashed_config_page(SafetyLinkClass *link);

/* Serializers for the two PC-facing query payloads, so the exact byte layout
 * specified in uart_task_ids.h lives in one place instead of being open-coded
 * in the bridge. `out` must have room for SAFETY_LINK_STATUS_PAYLOAD_LEN /
 * SAFETY_LINK_STATS_PAYLOAD_LEN bytes; both return the number written (0 on a
 * bad argument). */
size_t safety_link_build_status_payload(SafetyLinkClass *link, uint8_t *out);
size_t safety_link_build_stats_payload(SafetyLinkClass *link, uint8_t *out);
size_t safety_link_build_diag_payload(SafetyLinkClass *link, uint8_t *out);
size_t safety_link_build_trip_event_payload(SafetyLinkClass *link, uint8_t *out);
/* Frame C (the Pico's build identity + config commissioning state) mirrored
 * onto the PC link, same cache-only contract as the two above. Returns the
 * byte count written, 0 on failure. Variable length (the commit/datetime
 * strings are wire-sized), worst case 105 bytes; `out` must have room for
 * UART_PROTO_MAX_PAYLOAD. Full layout and the "unknown is a real state,
 * never a fabricated placeholder" rule are documented on the definition in
 * safety_link.c. */
size_t safety_link_build_fw_version_payload(SafetyLinkClass *link, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_LINK_H
