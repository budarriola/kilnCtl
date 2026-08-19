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
//                                   Pico boot and on request). This driver
//                                   only *parses* it today (no explicit
//                                   0x0B request-with-retry yet -- Phase
//                                   0.6b remains open): bytes1..2 protocol,
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

#include "uart_owner.h"
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Length of the PC-facing GET_STATUS payload -- the frame above plus the
 * ESP-measured age -- as specified in uart_task_ids.h. */
#define SAFETY_LINK_STATUS_PAYLOAD_LEN 25u

/* Length of the PC-facing GET_LINK_STATS payload (uart_task_ids.h). */
#define SAFETY_LINK_STATS_PAYLOAD_LEN 19u

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

/* A down link is logged once on the transition, then at most this often, so a
 * permanently absent Pico leaves periodic evidence in the log without one
 * warning per poll (which at the default period would be two per second). */
#define SAFETY_LINK_DOWN_LOG_PERIOD_MS 60000u

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

    safety_link_stats_t stats;
    uint16_t            poll_period_ms;

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

    /* Phase 10 (SaftyFW) / TODO.md 9.5: last-received UPDATE_STATUS,
     * applied the same way cached/cached_tick are (safety_apply_status()) --
     * see safety_apply_update_status() in safety_link.c. Guarded by
     * state_lock, same as everything else in this struct. */
    safety_link_update_status_t update_status;
    TickType_t                  update_status_tick;
    bool                        update_status_ever_received;

    bool       down_logged;      /* rate limiting for the "link is down" warning */
    TickType_t down_log_tick;

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

/* Phase 7b (LINK_PROTOCOL.md sec 4): reports what the last FW_VERSION frame
 * from the Pico said about compatibility. *out_known is false, and
 * *out_compatible, *out_peer_protocol and *out_peer_min_compatible are all
 * meaningless, until the Pico has pushed at least one FW_VERSION frame (its
 * own boot push, or a reply to SAFETY_CMD_GET_STATUS's eventual
 * GET_FW_VERSION request -- Phase 0.6b, not built this pass).
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

/* Serializers for the two PC-facing query payloads, so the exact byte layout
 * specified in uart_task_ids.h lives in one place instead of being open-coded
 * in the bridge. `out` must have room for SAFETY_LINK_STATUS_PAYLOAD_LEN /
 * SAFETY_LINK_STATS_PAYLOAD_LEN bytes; both return the number written (0 on a
 * bad argument). */
size_t safety_link_build_status_payload(SafetyLinkClass *link, uint8_t *out);
size_t safety_link_build_stats_payload(SafetyLinkClass *link, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_LINK_H
