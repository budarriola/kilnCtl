// link_task.c -- Phase 7/7b/8 (partial): kilnlink BROADCAST TX of the
// existing 23-byte status frame, SAFETY_CMD_FW_VERSION and SAFETY_CMD_DIAG
// (Frame B), RX handling of ANNOUNCE_VERSION, GET_FW_VERSION and now
// SAFETY_CMD_PUSH_CONTEXT (0x07) -> context_snapshot_t, and the receiver
// hardening (resync-on-0x7E, bounded buffers, no allocation)
// LINK_PROTOCOL.md section 3 requires. SET_FIRING_CEILING, CLEAR_TRIP and
// SET_CLOCK are still explicitly out of scope for this pass -- see
// docs/TODO.md Phase 7's remaining checkboxes. Parsing the context frame is
// only half of Phase 7: nothing here yet acts on it (no S2/S3/S4/S6/S10
// guard exists to disable on SIM_PLANT or reset on a boot_id change -- see
// the comments at the PUSH_CONTEXT case below and TODO.md's own notes on
// what remains).
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): this file must
// never reference GPIO6 or the relay, by name, number or symbol -- not even
// in a comment intended as an example. tools/check_isolation.ps1 greps this
// file for exactly that. Nothing below touches relay_owner.h, board_pins.h's
// SAFTYFW_PIN_RELAY, or hardware/gpio.h's relay pin -- only UART1, via
// uart_owner.
//
// A direct consequence, worth stating plainly rather than leaving implicit:
// this file structurally CANNOT ask relay_owner what GPIO6 is actually doing
// -- not "chooses not to", cannot, because doing so would mean including or
// calling into relay_owner, which is exactly what the isolation check exists
// to forbid. The two status-frame bits that need that answer instead go
// through safety_core_get_output_status() -- safety_core.h is not link/uart
// shaped, safety_core already legitimately depends on relay_owner (it
// commands it), and this file never names the word this comment is
// otherwise avoiding. See safety_core.h's doc comment on that function for
// the exact bit semantics.
#include "link_task.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "pico/time.h"

#include "task_priorities.h"
#include "watchdog_task.h"

#include "uart_owner.h"
#include "link_frame.h"

#include "boot_reason.h"
#include "current_task.h"
#include "discrete_task.h"
#include "safety_core.h"
#include "snapshots.h"
#include "thermo_task.h"
#include "update_task.h" // Phase 10 -- UPDATE_BEGIN/_DATA/_END/_ABORT dispatch, see the switch below

#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_power.h"
#include "kilnlink/kilnlink_version.h"

// Stack bumped from a single configMINIMAL_STACK_SIZE (Phase 10, this pass):
// LINK_RX_ASSEMBLY_MAX grew from 128 to KILNLINK_FRAME_STUFFED_MAX (~530
// bytes, see that macro's own comment below) to fit a full UPDATE_DATA frame,
// and link_task_handle_raw_frame() puts a same-sized `unstuffed[]` buffer on
// its own stack frame -- two ~530-byte buffers plus the usual call-depth
// margin no longer comfortably fits configMINIMAL_STACK_SIZE alone.
#define LINK_TASK_STACK_WORDS      (configMINIMAL_STACK_SIZE * 3)
// Bounded wait, not a blocking read: this task also owns the 500 ms TX
// cadence and must check in with watchdog_task, so it polls uart_owner's RX
// ring on a short period rather than blocking on a queue receive.
#define LINK_TASK_POLL_MS          100
#define LINK_STATUS_TX_PERIOD_MS   500
// Frame B (SAFETY_CMD_DIAG) cadence: slower than Frame A, deliberately.
// LINK_PROTOCOL.md's own text on Frame B: "additive... can ship before the
// ESP knows what to do with it" -- nothing on the ESP side gates on it yet
// (an unmodified KilnFW discards an unrecognised command byte), and Frame A
// is the one the ESP's liveness detection actually depends on (section 8:
// blocks heating at 1.5s, aborts a firing at 30s of silence). Giving Frame B
// a 2s period rather than matching Frame A's 500ms keeps the TX ring lighter
// for exactly the frame that has no deadline riding on it, without making
// the diagnostics stale on any timescale a human bench-watching them would
// notice.
#define LINK_DIAG_TX_PERIOD_MS     2000
// Frame E (SAFETY_CMD_POWER) cadence: "No guard reads any of this. It exists
// to be displayed" (kilnlink_power.h) -- same reasoning as Frame B above, a
// GUI readout has no deadline riding on it, so this rides well below Frame
// A's 500ms without making the wattage/energy figures stale on any
// human-noticeable timescale. Matches Frame B's period rather than inventing
// a third cadence.
#define LINK_POWER_TX_PERIOD_MS    2000

// Addressing (CommonFW/docs/LINK_PROTOCOL.md section 3, firmware/KilnFW/App/
// drivers/espInterfaces/uart_protocol.h and uart_task_ids.h). Mirrored here
// rather than included: SaftyFW does not, and must not, depend on KilnFW
// headers (CLAUDE.md/TODO.md -- the two firmwares are independently built),
// and these three values are part of the frozen compatibility-floor wire
// contract (LINK_PROTOCOL.md section 4), not implementation detail that
// could plausibly drift.
#define LINK_DEVICE_ESP     0u // UART_PROTO_DEVICE_ESP
#define LINK_DEVICE_SAFETY  2u // UART_PROTO_DEVICE_SAFETY
#define LINK_TASK_ID_SAFETY 7u // UART_TASK_ID_SAFETY
// LOG relay (LINK_PROTOCOL.md section 6, "Frame F"): the Pico's log_task
// addresses ordinary BROADCAST frames to this task id, same as KilnFW's own
// log lines (firmware/KilnFW/App/drivers/uart_task_ids.h:58,
// UART_TASK_ID_LOG) -- "no new task id, no new payload format" is the
// document's own framing for why this is not a fresh protocol addition.
#define LINK_TASK_ID_LOG    5u

// Bounds for the RX frame assembler below. Every command byte defined before
// Phase 10 (Status/FW_VERSION/DIAG/PUSH_CONTEXT/ANNOUNCE_VERSION) has a
// payload well under 128 bytes raw, which is what this used to be sized to.
// Phase 10's UPDATE_DATA (LINK_FRAME_UPDATE_DATA_CMD) does not: its payload
// is up to 1 (cmd) + 4 (offset) + UPDATE_CHUNK_LEN (248) = 253 bytes, the
// kilnlink protocol maximum, so this buffer now has to hold a full
// worst-case STUFFED frame -- KILNLINK_FRAME_STUFFED_MAX (~530 bytes) --
// rather than an arbitrary round number comfortably above the old frames'
// sizes. s_rx_assembly (below) collects stuffed wire bytes at this size;
// link_task_handle_raw_frame()'s local `unstuffed[]` buffer is the same
// size, since unstuffing never grows a frame.
#define LINK_RX_ASSEMBLY_MAX  KILNLINK_FRAME_STUFFED_MAX
#define LINK_RX_POLL_BUF      64u

static TaskHandle_t s_task_handle = NULL;

// Written only by link_task_fn (or functions it calls, all running on this
// task); read by link_task_get_degraded_no_context() from any task. volatile
// single-word read/write is sufficient, same pattern discrete_task.h/
// relay_owner.h already use for their own cross-task flags.
static volatile bool s_degraded_no_context = false;

static uint16_t s_msg_index = 0;
static uint8_t s_boot_id = 0;

static uint8_t s_rx_assembly[LINK_RX_ASSEMBLY_MAX];
static size_t s_rx_assembly_len = 0;
static bool s_rx_collecting = false;

// Context snapshot, mutex-guarded exactly like thermo_task.c's
// s_snapshot_lock/s_snapshot/s_snapshot_published pattern (see that file --
// link_task_get_context_snapshot()/link_task_publish_context() below mirror
// thermo_task_get_snapshot()/thermo_task_publish() call-for-call).
static SemaphoreHandle_t s_context_lock = NULL;
static context_snapshot_t s_context_snapshot; // guarded by s_context_lock
static bool s_context_published = false;

// Single-writer bookkeeping: touched only from link_task_fn / functions it
// calls (all running on this task), read back only by link_task_send_diag()
// (also this task) -- no lock needed, same reasoning as s_degraded_no_context
// above but for a handful of scalars instead of one bool.
static uint32_t s_context_frames_ok = 0;
static uint32_t s_context_frames_bad = 0;
// Tick of the last SUCCESSFUL parse. Never read until s_context_frames_ok > 0
// (see link_task_send_diag()), so its zero-initialised value before the
// first frame never gets treated as a real timestamp -- DIAG keeps sending
// 255 ("never received") until then, per LINK_PROTOCOL.md section 4.
static TickType_t s_last_context_rx_tick = 0;
// Latches true the first time a successfully-parsed context frame carries
// CONTEXT_FLAG_SIM_PLANT. LINK_PROTOCOL.md section 4: "a safety processor
// correlating against fabricated temperatures is worse than one with no
// context at all" -- this is a persistent warning, not a live indicator, so
// it is never cleared once set, even if a later frame omits the bit.
static bool s_context_sim_seen = false;
// boot_id change detection (LINK_PROTOCOL.md section 4: "boot_id invalidates
// history... the Pico resets every correlation window"). Tracked here so the
// detection exists from day one; see the PUSH_CONTEXT case in
// link_task_handle_raw_frame() for why acting on a change is still a no-op.
static uint8_t s_last_context_boot_id = 0;
static bool s_context_boot_id_known = false;

// Count of successful (accepted-by-the-TX-ring) Frame A sends since
// link_task_start() -- Phase 10's confirm.h telemetry_sent_ok evidence, see
// link_task_get_status_tx_ok_count()'s doc comment in link_task.h. Written
// only from link_task_send_status() (this task); read from any task, same
// single-writer/plain-read reasoning as s_context_frames_ok/bad above.
static uint32_t s_status_tx_ok_count = 0;

// --- TX ----------------------------------------------------------------

// dst_task-general version -- link_task_send_broadcast() below is the
// existing SAFETY-task-id wrapper every Frame A/B/C call site already used
// before this function existed; link_task_send_log() (added for log_task,
// see link_task.h) is the other caller, addressing LINK_TASK_ID_LOG instead.
// Returns true iff uart_owner_send() accepted the frame (room in the TX
// ring) -- the caller decides what "false" means for its own counters
// (log_task counts it as a dropped log line; the Frame A/B/C call sites
// below still discard it, matching their pre-existing behaviour, since
// telemetry's own drop accounting already lives in uart_owner's counter).
static bool link_task_send_broadcast_to(uint8_t dst_task, const uint8_t *payload, uint8_t length)
{
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = s_msg_index++,
        .src_device = LINK_DEVICE_SAFETY,
        .src_task = LINK_TASK_ID_SAFETY,
        .dst_device = LINK_DEVICE_ESP,
        .dst_task = dst_task,
        .length = length,
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    if (raw_len == 0) {
        return false; // encode failure -- shouldn't happen for a well-formed frame we built ourselves
    }

    uint8_t stuffed[KILNLINK_FRAME_STUFFED_MAX];
    size_t stuffed_len = kilnlink_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0) {
        return false;
    }

    // uart_owner_send() is itself non-blocking and drops the WHOLE frame if
    // the TX ring has no room (its own counter tracks that) -- exactly
    // LINK_PROTOCOL.md section 2 rule 3. Nothing here retries or escalates.
    return uart_owner_send(stuffed, stuffed_len);
}

// Returns uart_owner_send()'s own accepted/dropped result now (previously
// discarded, `(void)`-cast) -- link_task_send_status() below needs it for
// s_status_tx_ok_count; the FW_VERSION/DIAG call sites still ignore it,
// unchanged behaviour for them.
static bool link_task_send_broadcast(const uint8_t *payload, uint8_t length)
{
    return link_task_send_broadcast_to(LINK_TASK_ID_SAFETY, payload, length);
}

static void link_task_send_status(void)
{
    thermo_snapshot_t th;
    bool th_present = thermo_task_get_snapshot(&th);
    // th.tc_c/th.cj_c are already NaN whenever th.valid is false
    // (snapshots.h's contract); the ternaries below only cover the
    // th_present == false case (nothing published yet), where *out is
    // documented as zeroed rather than NaN.
    bool temp_valid = th_present && th.valid;
    float tc_c = temp_valid ? th.tc_c : NAN;
    float cj_c = temp_valid ? th.cj_c : NAN;
    uint8_t fault_bits = th_present ? th.fault_bits : 0;

    current_snapshot_t cur;
    current_task_get_snapshot(&cur);

    bool estop = discrete_task_estop_pressed();

    uint8_t payload[LINK_FRAME_STATUS_LEN];
    bool energized_bit = false;
    bool enabled_bit = false;
    safety_core_get_output_status(&energized_bit, &enabled_bit);
    link_frame_pack_status(payload, estop, energized_bit, enabled_bit, temp_valid, tc_c, cj_c,
                            fault_bits, cur.amps[0], cur.amps[1], cur.amps[2]);

    if (link_task_send_broadcast(payload, LINK_FRAME_STATUS_LEN)) {
        s_status_tx_ok_count++;
    }
}

static void link_task_send_fw_version(void)
{
    // build_info.h (git commit, dirty flag, build timestamp) is not
    // generated yet -- TODO.md Phase 8 item, not this pass. "Unknown must map
    // to dirty = 1" (LINK_PROTOCOL.md section 4) is honoured trivially: an
    // unknown commit is reported dirty, never falsely clean. config_version/
    // config_crc are 0 -- there is no config_store yet (Phase 9), and the
    // spec documents 0 as meaning exactly that: "running on compiled-in
    // defaults that were never commissioned."
    uint8_t payload[16];
    size_t len = link_frame_pack_fw_version(payload, sizeof(payload), KILNLINK_PROTOCOL_VERSION,
                                             KILNLINK_MIN_COMPATIBLE, /* dirty = */ 1, NULL, 0, NULL,
                                             0, s_boot_id, /* config_version = */ 0,
                                             /* config_crc = */ 0);
    if (len == 0) {
        return;
    }

    link_task_send_broadcast(payload, (uint8_t)len);
}

static void link_task_send_diag(void)
{
    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    bool warn_active = false;
    uint8_t diag_state = 0;
    safety_core_get_diag_status(&trip_reason, &warn_active, &diag_state);

    // warn_mask/trip_mask: LINK_PROTOCOL.md documents these as "one bit per
    // guard" across the full 13-guard suite. This build's safety_guards.c
    // only tracks ONE is_tripped/reason pair for the whole module (5 of 13
    // guards implemented, see safety_guards.h's own header comment) and,
    // similarly, only an OR of the two WARN-capable guards' flags (S5/S12) --
    // there is no per-guard bitmask anywhere in this codebase to report a
    // real 13-bit mask from. Rather than inventing one, this synthesizes a
    // single-bit degraded approximation: trip_mask sets bit (reason-1) when
    // tripped (matching safety_trip_t's own numbering, so the one bit that IS
    // set at least identifies the right guard), and warn_mask sets bit 0 as
    // an aggregate "something is warning" signal when warn_active is true,
    // since no per-guard identity is available for WARN at all. TODO.md
    // records this as the honest state of Frame B, not a placeholder to
    // silently upgrade later.
    uint16_t trip_mask = 0;
    if (trip_reason != SAFETY_TRIP_NONE) {
        trip_mask = (uint16_t)(1u << ((uint8_t)trip_reason - 1u));
    }
    uint16_t warn_mask = warn_active ? 0x0001u : 0u;

    uint32_t uptime_ms = to_ms_since_boot(get_absolute_time());

    // boot_reason: bit1 (watchdog) is real, from the cached
    // watchdog_caused_reboot fact main.c read at boot step 3. bit0 (power-on)
    // is the honest complement of that -- this build has no separate true-
    // power-on-reset detection distinct from "some other, non-watchdog
    // reset" (RUN pin, debugger, etc.), so bit0 means "not a watchdog reset"
    // rather than a verified power-on event; that is the best this codebase
    // can report without inventing a detector it does not have. bit2
    // (brownout) has no source at all here and is always 0 -- see
    // link_frame.h's LINK_DIAG_BOOT_BROWNOUT comment.
    saftyfw_boot_reason_t boot = boot_reason_get_cached();
    uint8_t boot_reason_byte = 0;
    if (boot.watchdog_caused_reboot) {
        boot_reason_byte |= LINK_DIAG_BOOT_WATCHDOG;
    } else {
        boot_reason_byte |= LINK_DIAG_BOOT_POWERON;
    }

    // context_age_100ms: 255 ("never received") is real now, not a
    // placeholder default -- it is the honest answer whenever
    // s_context_frames_ok == 0, i.e. no well-formed PUSH_CONTEXT has ever
    // been parsed this boot. Once one has, the age is computed from
    // s_last_context_rx_tick and clamped to 254 max so a genuinely stale
    // (but received) context can never be misread as "never received" by
    // reusing the sentinel -- LINK_PROTOCOL.md doesn't specify the clamp
    // explicitly, but 254*100ms = 25.4s is well past where the exact value
    // still matters downstream.
    // context_frames_ok/bad: real running counts from
    // link_task_handle_push_context(), one increment per successful/rejected
    // PUSH_CONTEXT payload respectively.
    uint8_t context_age_100ms = 255;
    if (s_context_frames_ok > 0) {
        TickType_t age_ticks = xTaskGetTickCount() - s_last_context_rx_tick;
        uint32_t age_ms = (uint32_t)age_ticks * portTICK_PERIOD_MS;
        uint32_t age_100ms = age_ms / 100u;
        context_age_100ms = (age_100ms > 254u) ? 254u : (uint8_t)age_100ms;
    }

    // flags bit0 sim_context_seen: real now, from s_context_sim_seen (see
    // that variable's declaration for the "latched, never cleared" contract).
    uint8_t diag_flags = LINK_DIAG_FLAG_CALIBRATION_MISSING;
    if (s_context_sim_seen) {
        diag_flags |= LINK_DIAG_FLAG_SIM_CONTEXT_SEEN;
    }
    // flags: bit1 calibration_missing = 1 (no config_store, Phase 9 -- this
    // is the honest current state, not a bug); bit2 estop_unwired_suspect = 0
    // (no detection heuristic specified in SAFETY_MODEL.md/HARDWARE.md or
    // built). Note what parsing PUSH_CONTEXT does NOT yet mean: there is
    // still no context-consuming correlation guard (S2/S3/S4/S6/S10) to
    // disable on sim_context_seen or reset on a boot_id change -- this frame
    // reports that the fact is known, not that anything downstream acts on
    // it yet.
    uint8_t payload[LINK_FRAME_DIAG_LEN];
    link_frame_pack_diag(payload, (uint8_t)trip_reason, warn_mask, trip_mask, uptime_ms,
                          boot_reason_byte, context_age_100ms, s_context_frames_ok,
                          s_context_frames_bad, uart_owner_get_tx_dropped(), diag_state,
                          diag_flags);

    link_task_send_broadcast(payload, LINK_FRAME_DIAG_LEN);
}

// Frame E (SAFETY_CMD_POWER, 0x0E) -- docs/CURRENT_SENSE.md section 3b /
// CommonFW/docs/LINK_PROTOCOL.md section 6. current_task_get_power() is the
// same mutex/critical-section-guarded pull current_task.c already documents
// for current_task_get_snapshot() (used above by link_task_send_status());
// this is the second, independent consumer of that publish, exactly the
// split current_sense.h's header comment describes (unfiltered snapshot for
// a future guard, filtered power_t for reporting only -- this function only
// ever touches the latter). Nothing here reaches into current_sense_cal_t
// directly -- current_sense_power_t (current_sense.c, this pass) now carries
// mains_voltage_v/calibrated/any_clipped/p_total_w/energy_wh precisely so
// this file does not need calibration-struct visibility to build the frame.
static void link_task_send_power(void)
{
    current_sense_power_t pw;
    current_task_get_power(&pw);

    kilnlink_power_t frame = {
        .power_window_s = 120u, // docs/CURRENT_SENSE.md section 3b default; current_sense.c's
                                 // CS_POWER_WINDOW_S is not exposed across the module boundary,
                                 // so this mirrors the compiled-in constant rather than reading it
        .mains_voltage_v = pw.mains_voltage_v,
        .p_total_w = pw.p_total_w,
        .energy_wh = pw.energy_wh,
    };
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS && ch < 3u; ch++) {
        frame.i_conducting_a[ch] = pw.i_conducting_a[ch];
        frame.conduction_fraction[ch] = pw.conduction_fraction[ch];
        frame.p_avg_w[ch] = pw.p_avg_w[ch];
    }

    uint8_t flags = 0;
    if (!isnan(pw.mains_voltage_v)) {
        flags |= KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED;
    }
    if (pw.any_clipped) {
        flags |= KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED;
    }
    if (pw.calibrated) {
        flags |= KILNLINK_POWER_FLAG_CALIBRATED;
    }
    frame.flags = flags;

    uint8_t payload[KILNLINK_POWER_LEN];
    kilnlink_power_status_t status;
    size_t len = kilnlink_power_encode(&frame, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // KILNLINK_POWER_ERR_BUFFER_TOO_SMALL -- can't happen for a
                 // fixed sizeof(payload) == KILNLINK_POWER_LEN buffer, but
                 // guarded rather than assumed, same discipline as every
                 // other encode call site in this file.
    }

    link_task_send_broadcast(payload, (uint8_t)len);
}

// --- RX ------------------------------------------------------------------

// Mirrors thermo_task_publish()'s short-bounded-wait discipline exactly
// (same 50ms rationale: a single struct copy under the lock should never
// contend long enough to matter, but a finite wait still beats a hang).
static void link_task_publish_context(const context_snapshot_t *snap)
{
    if (!s_context_lock) {
        return;
    }
    if (xSemaphoreTake(s_context_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_context_snapshot = *snap;
    s_context_published = true;
    xSemaphoreGive(s_context_lock);
}

static void link_task_handle_push_context(const kilnlink_frame_t *frame)
{
    context_snapshot_t snap;
    if (!link_frame_unpack_context(frame->payload, frame->length, &snap)) {
        // Untrusted wire input rejected by link_frame_unpack_context()'s own
        // validation (short/truncated/oversized zone_count/length mismatch).
        // LINK_PROTOCOL.md section 3's "newer context is strictly more
        // useful than older context" is about the RX assembly path
        // overwriting an unread-but-good frame, not license to publish a
        // rejected one -- a rejected frame is noise, not "older good
        // context", and must never overwrite the last good snapshot.
        s_context_frames_bad++;
        return;
    }

    // link_frame_unpack_context() deliberately never sets timestamp_ms (it
    // is pure/RTOS-free and has no notion of the local clock) -- this is the
    // one field only the caller can fill in, same split thermo_task.c uses
    // for its own snapshot's timestamp.
    snap.timestamp_ms = to_ms_since_boot(get_absolute_time());

    // boot_id change detection (LINK_PROTOCOL.md section 4: "the Pico resets
    // every correlation window" on a boot_id change). Tracking the change is
    // built; acting on it is not -- there is no context-consuming
    // correlation guard anywhere in this codebase yet (S2/S3/S4/S6/S10 are
    // all still TODO.md Phase 7 checkboxes), so there is nothing to reset.
    // This is deliberately NOT wired to a reset call that does not exist;
    // when S2/S6/S10 land, this is where their window-reset call belongs --
    // `boot_id_changed` is computed and immediately unused beyond that
    // future hook, on purpose.
    bool boot_id_changed = s_context_boot_id_known && snap.boot_id != s_last_context_boot_id;
    (void)boot_id_changed;
    s_last_context_boot_id = snap.boot_id;
    s_context_boot_id_known = true;

    if (snap.flags & CONTEXT_FLAG_SIM_PLANT) {
        s_context_sim_seen = true; // latched, never cleared -- see the declaration's comment
    }

    link_task_publish_context(&snap);

    s_context_frames_ok++;
    s_last_context_rx_tick = xTaskGetTickCount();
}

static void link_task_handle_announce_version(const kilnlink_frame_t *frame)
{
    // Offsets 1..2 = peer protocol version, 3..4 = peer min_compatible, both
    // u16 LE, fixed offset (LINK_PROTOCOL.md section 4: "read bytes 1-4
    // first"). Anything shorter is malformed/truncated -- ignored, not
    // guessed at.
    if (frame->length < 5) {
        return;
    }

    uint16_t peer_protocol = (uint16_t)(frame->payload[1] | ((uint16_t)frame->payload[2] << 8));
    uint16_t peer_min_compatible =
        (uint16_t)(frame->payload[3] | ((uint16_t)frame->payload[4] << 8));

    bool compatible = link_frame_versions_compatible(KILNLINK_PROTOCOL_VERSION,
                                                       KILNLINK_MIN_COMPATIBLE, peer_protocol,
                                                       peer_min_compatible);

    // LINK_PROTOCOL.md section 4, "What each side does about a mismatch":
    // the Pico enters DEGRADED_NO_CONTEXT and does NOT latch a trip. This is
    // the ONLY effect a version mismatch has from in here -- no relay/trip
    // call, by design (and this file could not make one anyway, see the
    // header comment).
    s_degraded_no_context = !compatible;
}

static void link_task_handle_raw_frame(const uint8_t *stuffed, size_t stuffed_len)
{
    uint8_t unstuffed[LINK_RX_ASSEMBLY_MAX];
    kilnlink_frame_status_t ustatus;
    size_t ulen = kilnlink_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &ustatus);
    if (ulen == 0) {
        return; // unterminated escape or (shouldn't happen, same-size buffer) too small
    }

    kilnlink_frame_t frame;
    if (kilnlink_frame_decode(unstuffed, ulen, &frame) != KILNLINK_FRAME_OK) {
        return; // bad length/CRC/type -- untrusted wire input, discarded, not guessed at
    }

    if (frame.msg_type != KILNLINK_MSG_BROADCAST || frame.length == 0) {
        return; // the Pico never participates in the ACK'd DATA/ACK/NACK transport
    }

    uint8_t cmd = frame.payload[0];
    switch (cmd) {
    case LINK_FRAME_ANNOUNCE_VERSION_CMD:
        link_task_handle_announce_version(&frame);
        break;
    case LINK_FRAME_FW_VERSION_CMD:
        // Same id as the request (SAFETY_CMD_GET_FW_VERSION), distinguished
        // by direction and length: the ESP's request is exactly 1 byte, no
        // arguments. Answer every copy seen, idempotently -- the ESP is the
        // side allowed to retry (LINK_PROTOCOL.md section 2).
        if (frame.length == 1) {
            link_task_send_fw_version();
        }
        break;
    case LINK_FRAME_PUSH_CONTEXT_CMD:
        link_task_handle_push_context(&frame);
        break;
    // Phase 10 -- thin dispatch only, matching PUSH_CONTEXT's own one-line
    // call above, except the handler lives in update_task.c rather than
    // this file: flash I/O does not belong on link_task's priority/stack,
    // and update_task.c is not subject to this file's GPIO6/relay
    // isolation rule (it legitimately needs safety_core's relay/trip
    // status to gather Phase 10's own preconditions -- see that file's
    // header comment). Each handler copies frame->payload into a bounded
    // FreeRTOS queue with a zero-timeout xQueueSend and counts (rather than
    // blocks on) a drop -- link_task's own RX loop must never be delayed by
    // update_task falling behind. frame->payload/frame->length include the
    // command byte itself at payload[0], same convention
    // link_task_handle_push_context() already uses for PUSH_CONTEXT.
    case LINK_FRAME_UPDATE_BEGIN_CMD:
        update_task_handle_begin(frame.payload, frame.length);
        break;
    case LINK_FRAME_UPDATE_DATA_CMD:
        update_task_handle_data(frame.payload, frame.length);
        break;
    case LINK_FRAME_UPDATE_END_CMD:
        update_task_handle_end(frame.payload, frame.length);
        break;
    case LINK_FRAME_UPDATE_ABORT_CMD:
        update_task_handle_abort(frame.payload, frame.length);
        break;
    default:
        // Everything else (CLEAR_TRIP, SET_FIRING_CEILING, SET_CLOCK, ...)
        // is genuinely out of scope this pass -- see this file's header
        // comment and TODO.md Phase 7's remaining checkboxes. An
        // unrecognised type is silently discarded, matching
        // LINK_PROTOCOL.md's own additive-compatibility principle: "a peer
        // that has never heard of it discards it."
        break;
    }
}

// Resynchronises on 0x7E from any state (LINK_PROTOCOL.md section 3): a
// delimiter is always a frame boundary, whatever came before it. Bounded
// buffer, no allocation. A break (a long run of non-delimiter noise, or
// nothing at all) just never completes a frame -- tolerated as "peer not up",
// not latched as an error.
static void link_task_rx_process_byte(uint8_t b)
{
    if (b == KILNLINK_FRAME_DELIM) {
        if (s_rx_collecting && s_rx_assembly_len > 0) {
            link_task_handle_raw_frame(s_rx_assembly, s_rx_assembly_len);
        }
        s_rx_assembly_len = 0;
        s_rx_collecting = true; // this delimiter is simultaneously "end of previous" and "start of next"
        return;
    }

    if (!s_rx_collecting) {
        return; // noise before the first delimiter we've ever seen -- drop it
    }

    if (s_rx_assembly_len >= LINK_RX_ASSEMBLY_MAX) {
        // Oversized run with no delimiter in sight -- drop what we have and
        // wait for the next 0x7E to resync, rather than growing without
        // bound or overwriting past the buffer.
        s_rx_collecting = false;
        s_rx_assembly_len = 0;
        return;
    }

    s_rx_assembly[s_rx_assembly_len++] = b;
}

// --- Task ------------------------------------------------------------------

static void link_task_fn(void *arg)
{
    (void)arg;

    // Boot push, unsolicited, before entering the steady loop
    // (LINK_PROTOCOL.md section 6, Frame C: "pushed unsolicited once at
    // boot" -- this is what tells the ESP "the safety processor just
    // restarted" without polling for it).
    link_task_send_fw_version();

    TickType_t last_status_tx = xTaskGetTickCount();
    TickType_t last_diag_tx = xTaskGetTickCount();
    TickType_t last_power_tx = xTaskGetTickCount();

    for (;;) {
        uint8_t rx_buf[LINK_RX_POLL_BUF];
        size_t n = uart_owner_rx_read(rx_buf, sizeof(rx_buf));
        for (size_t i = 0; i < n; i++) {
            link_task_rx_process_byte(rx_buf[i]);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_tx) >= pdMS_TO_TICKS(LINK_STATUS_TX_PERIOD_MS)) {
            link_task_send_status();
            last_status_tx = now;
        }
        if ((now - last_diag_tx) >= pdMS_TO_TICKS(LINK_DIAG_TX_PERIOD_MS)) {
            link_task_send_diag();
            last_diag_tx = now;
        }
        if ((now - last_power_tx) >= pdMS_TO_TICKS(LINK_POWER_TX_PERIOD_MS)) {
            link_task_send_power();
            last_power_tx = now;
        }

        vTaskDelay(pdMS_TO_TICKS(LINK_TASK_POLL_MS));

        watchdog_task_checkin(WATCHDOG_CHECKIN_LINK_TASK);
    }
}

bool link_task_start(void)
{
    // Pseudo-random, latched once at boot. Nothing else in this build sources
    // an identity value (boot_reason.h only tracks trip-reason survival
    // across a watchdog reset, not a boot counter/id) -- time_us_64() has
    // been running since well before this call, so its low bits are a cheap,
    // adequate source of "looks different each boot." This is diagnostic
    // identity only (so the ESP can tell "the Pico just restarted" apart
    // from "same Pico, still running"), not a security or safety value, so
    // true entropy is not required.
    s_boot_id = (uint8_t)(time_us_64() ^ (time_us_64() >> 8));
    s_degraded_no_context = false;
    s_msg_index = 0;
    s_rx_assembly_len = 0;
    s_rx_collecting = false;

    s_context_frames_ok = 0;
    s_context_frames_bad = 0;
    s_last_context_rx_tick = 0;
    s_context_sim_seen = false;
    s_last_context_boot_id = 0;
    s_context_boot_id_known = false;
    s_context_published = false;
    s_status_tx_ok_count = 0;

    // Mutex-guarded snapshot, same pattern/failure handling as
    // thermo_task_start()'s s_snapshot_lock.
    s_context_lock = xSemaphoreCreateMutex();
    if (!s_context_lock) {
        return false;
    }

    BaseType_t ok = xTaskCreate(link_task_fn, "link_task", LINK_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_LINK_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_LINK_PATH);
    return true;
}

bool link_task_get_degraded_no_context(void)
{
    return s_degraded_no_context;
}

bool link_task_get_context_snapshot(context_snapshot_t *out)
{
    if (!out) {
        return false;
    }
    out->timestamp_ms = 0;
    out->valid = false;
    out->flags = 0;
    out->boot_id = 0;
    out->seq = 0;
    out->uptime_ms = 0;
    out->relay_now_mask = 0;
    out->relay_recent_mask = 0;
    out->recent_window_s = 0;
    out->zone_count = 0;

    if (!s_context_lock || !s_context_published) {
        return false;
    }
    if (xSemaphoreTake(s_context_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    *out = s_context_snapshot;
    xSemaphoreGive(s_context_lock);
    return true;
}

bool link_task_send_log(const uint8_t *payload, uint8_t length)
{
    // Thin wrapper: the only difference from the Frame A/B/C call sites is
    // the destination task id (LOG, not SAFETY) -- see
    // link_task_send_broadcast_to()'s comment for why both share the same
    // encode/stuff/send path. log_task is the caller, and it -- not this
    // function -- owns the dropped-log-frame counter (docs/ARCHITECTURE.md
    // section 1: "count the drops"), since log_task also drops lines before
    // ever reaching here (its own queue-full and TX-reserve-watermark
    // checks) and wants one counter covering every drop point, not several.
    return link_task_send_broadcast_to(LINK_TASK_ID_LOG, payload, length);
}

bool link_task_send_safety(const uint8_t *payload, uint8_t length)
{
    // Thin wrapper, same shape as link_task_send_log() just above -- see
    // link_task_send_broadcast_to()'s own comment for why both share the
    // encode/stuff/send path. update_task.c is the only intended caller
    // (Phase 10's UPDATE_STATUS replies).
    return link_task_send_broadcast_to(LINK_TASK_ID_SAFETY, payload, length);
}

uint32_t link_task_get_status_tx_ok_count(void)
{
    return s_status_tx_ok_count;
}

float link_task_get_tx_ring_fill_fraction(void)
{
    size_t cap = uart_owner_get_tx_capacity();
    if (cap == 0) {
        return 1.0f; // defensive -- treat "no capacity info" as "full", never as "empty"
    }
    return (float)uart_owner_get_tx_used() / (float)cap;
}
