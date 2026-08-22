// Opto-isolated link to the RP2040 safety processor (A1).
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
//   ESP32-S3 UART1, 8N1, CONFIG_KILNCTL_SAFETY_BAUD_RATE (115200 by default),
//   carrying the *same* uart_protocol framing as the PC link (0x7E-delimited,
//   byte-stuffed, CRC16/CCITT-FALSE, indexed DATA frames with ACK/NACK). The
//   ESP identifies itself as UART_PROTO_DEVICE_ESP; the Pico must identify
//   itself as UART_PROTO_DEVICE_SAFETY (= 2). Both ends register task_id
//   UART_TASK_ID_SAFETY (= 7); everything below is that task's payload.
//
//   Both directions are electrically INVERTED by the TCMT1109 optocouplers and
//   this side fixes that with uart_set_line_inverse(TXD_INV | RXD_INV) -- see
//   the "Isolation barrier" section of docs/HARDWARE.md and safety_link.c.
//
//   The Pico needs no inversion of its own: each optocoupler is an inverter,
//   so the ESP's TXD_INV cancels U2 and the Pico's RX sees standard polarity
//   (idle logical 1 -> GPIO5 low -> LED off -> R9 holds Pico RX high), while
//   U3 inverts the Pico's ordinary TX and RXD_INV puts it back. The RP2040's
//   plain hardware UART works unmodified -- no PIO UART, no external inverter.
//   Exactly ONE end inverts, and it is this one; inverting on the Pico side
//   too would cancel the optocouplers and break the link.
//
// ESP -> Pico requests (payload byte0 = subcommand, from uart_task_ids.h)
//   0x01 SAFETY_CMD_GET_STATUS      no args. Answer with the status frame
//                                   below, addressed back to
//                                   (UART_PROTO_DEVICE_ESP, task 7).
//   0x02 SAFETY_CMD_REQUEST_ENABLE  byte1 = enable (0/1). Advisory: the Pico
//                                   may refuse, and its own interlocks always
//                                   win. A protocol-level ACK is the whole
//                                   reply; the ESP learns the outcome from
//                                   SAFETY_FLAG_ENABLED on the next status.
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
//                   bit4 SAFETY_FLAG_ENABLED    -- heating currently permitted
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

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_param_value.h"
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

/* Length of the Pico's status frame (see the contract above). */
#define SAFETY_LINK_STATUS_FRAME_LEN 23u

/* Length of the Pico's power frame (SAFETY_CMD_POWER / Frame E,
 * CommonFW/docs/LINK_PROTOCOL.md sec 6), byte-for-byte
 * KILNLINK_POWER_LEN from kilnlink_power.h. Pushed unsolicited, "no guard
 * reads any of this. It exists to be displayed." -- see
 * safety_apply_power() in safety_link.c for the field layout. */
#define SAFETY_LINK_POWER_FRAME_LEN 55u
#define SAFETY_LINK_POWER_CHANNELS 3u

/* Power frame flags byte (offset 2), kilnlink_power.h's
 * kilnlink_power_flag_t mirrored here for the same reason SAFETY_CMD_POWER
 * is hand-parsed rather than calling that codec (see uart_task_ids.h). */
#define SAFETY_LINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED 0x01u
#define SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED       0x02u
#define SAFETY_LINK_POWER_FLAG_CALIBRATED                0x04u

/* Length of the Pico's DIAG frame (SAFETY_CMD_DIAG / Frame B,
 * CommonFW/docs/LINK_PROTOCOL.md sec 6), byte-for-byte KILNLINK_DIAG_LEN
 * from kilnlink_diag.h. Pushed unsolicited on the same 500 ms cadence as the
 * status frame -- see safety_apply_diag() in safety_link.c for the field
 * layout. */
#define SAFETY_LINK_DIAG_FRAME_LEN 26u

/* DIAG flags byte (offset 25), kilnlink_diag.h's kilnlink_diag_flag_t
 * mirrored here for the same reason SAFETY_CMD_POWER/DIAG are hand-parsed
 * rather than calling that codec (see uart_task_ids.h). */
#define SAFETY_LINK_DIAG_FLAG_SIM_CONTEXT_SEEN      0x01u
#define SAFETY_LINK_DIAG_FLAG_CALIBRATION_MISSING   0x02u
#define SAFETY_LINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT 0x04u

/* DIAG boot_reason byte (offset 10), kilnlink_diag.h's
 * kilnlink_diag_boot_flag_t mirrored here, same reasoning as above. */
#define SAFETY_LINK_DIAG_BOOT_POWERON  0x01u
#define SAFETY_LINK_DIAG_BOOT_WATCHDOG 0x02u
#define SAFETY_LINK_DIAG_BOOT_BROWNOUT 0x04u

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
 * ESP-measured age -- as specified in uart_task_ids.h. */
#define SAFETY_LINK_STATUS_PAYLOAD_LEN 25u

/* Length of the PC-facing GET_LINK_STATS payload (uart_task_ids.h). */
#define SAFETY_LINK_STATS_PAYLOAD_LEN 19u

/* Length of the PC-facing GET_DIAG / GET_TRIP_EVENT payloads
 * (uart_task_ids.h). */
#define SAFETY_LINK_DIAG_PAYLOAD_LEN       27u
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

/* Per-request ACK timeout handed to uart_protocol_send. Deliberately much
 * shorter than the PC link's 200 ms default: uart_protocol retries up to
 * UART_PROTO_MAX_RETRIES (10) times internally, so with no peer at all every
 * request costs ~10x this before it gives up, and that whole time is spent
 * inside the poll task. At 50 ms the dead-peer cost is ~500 ms, which stays
 * comparable to the default poll period instead of dwarfing it. A frame is
 * ~2 ms on the wire at 115200, so 50 ms is still ~25x the round trip. */
#define SAFETY_LINK_ACK_TIMEOUT_MS 50u

/* How long to wait for the status DATA frame *after* the request was ACKed.
 * Separate from the ACK timeout because this covers the Pico actually reading
 * its thermocouple and three ADC channels, not just its receive interrupt. */
#define SAFETY_LINK_REPLY_TIMEOUT_MS 250u

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
    uint32_t frames_received; /* well-formed status frames accepted */
    uint32_t frame_errors;    /* malformed/unexpected payloads seen by this driver,
                               * plus uart_owner's line-error count for UART1 */
    uint32_t timeouts;        /* requests that produced no usable answer (no ACK,
                               * a NACK, or an ACK with no status frame behind it) */
    uint16_t poll_period_ms;  /* current period; 0 = polling off */
} safety_link_stats_t;

typedef struct {
    /* This driver owns a *second* UART peripheral and a second protocol stack,
     * entirely separate from the PC link on UART0 (see App/main.c). Same code,
     * same framing, different port and different peer -- there is deliberately
     * no bespoke framing layer here. */
    uart_owner_t    owner;
    uart_protocol_t proto;
    QueueHandle_t   inbox;       /* frames addressed to (ESP, UART_TASK_ID_SAFETY) */
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

    /* When the last SAFETY_CMD_TRIP_EVENT was applied -- separate from
     * cached_tick above, which only moves on GET_STATUS (Frame A). Read
     * under state_lock, same as cached_tick; safety_link_get_status() turns
     * it into safety_link_status_t::trip_event_age_ms the same way
     * cached_tick becomes age_ms. Meaningless until
     * cached.trip_event_ever_received is true. */
    TickType_t            trip_event_tick;

    safety_link_stats_t stats;
    uint16_t            poll_period_ms;
    /* 2026-08-20 congestion fix -- see SAFETY_LINK_BACKOFF_MAX_STREAK's
     * comment. Poll-task-only, no lock needed. */
    uint8_t             no_reply_streak;

    uint32_t fault_sources;        /* bitwise OR of safety_fault_source_t */
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

    /* TODO.md owner-report item 3 (2026-08-21): poll-task-only state, same
     * no-lock reasoning as down_logged above -- see safety_sync_tc_type()
     * (safety_link.c) for the full design. 0xFF is never a real tc_type
     * (the wire range is 0-0x0F and this driver only ever sends 0-7, the
     * real thermocouple types), so it doubles as "never successfully sent /
     * must (re)send at the next opportunity" -- set at safety_link_start()
     * and again on every down->up transition, which is what makes a
     * reconnect re-apply the setting rather than silently trusting a Pico
     * that may have rebooted and lost it. */
    uint8_t    tc_type_last_sent;

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
    bool       initialized;
} SafetyLinkClass;

/* Brings up UART1 on SAFETY_TX_IO/SAFETY_RX_IO at SAFETY_UART_BAUD_RATE with
 * both line inversions and the RX pull-up enabled, drives SAFETY_FAULT_IO low
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

/* Asks the safety processor to permit (1) or drop (0) heating. Advisory --
 * the Pico's own interlocks always win. Blocks for the exchange (worst case
 * ~SAFETY_LINK_ACK_TIMEOUT_MS * UART_PROTO_MAX_RETRIES with no peer), so call
 * it from a bridge/app task, not from anything latency-critical. Returns
 * ESP_ERR_TIMEOUT if the far side never ACKed. */
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
 * Thin, additive wrappers -- App/drivers/ota_pico_relay.c is the only
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

/* CommonFW/docs/LINK_PROTOCOL.md sec 4/6, SAFETY_CMD_GET_CT_CAL /
 * SAFETY_CMD_CT_CAL (shared id 0x1A) -- kilnlink_get_ct_cal.h/kilnlink_ct_cal.h.
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

/* docs/COMMISSIONING.md sec 2/3 -- SAFETY_CMD_SET_PARAM (0x1C) / COMMIT_CONFIG
 * (0x1D) / GET_CONFIG_PAGE (0x1F). App/drivers/safety_cfg_store.c (the
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
esp_err_t safety_link_get_config_page(SafetyLinkClass *link, uint8_t page_index,
                                       kilnlink_config_page_t *out);

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
