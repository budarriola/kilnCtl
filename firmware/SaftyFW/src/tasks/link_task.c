// link_task.c -- Phase 7/7b/8 (partial): kilnlink BROADCAST TX of the
// existing 23-byte status frame, SAFETY_CMD_FW_VERSION, SAFETY_CMD_DIAG
// (Frame B) and now SAFETY_CMD_TRIP_EVENT (Frame D), RX handling of
// ANNOUNCE_VERSION, GET_FW_VERSION, SAFETY_CMD_PUSH_CONTEXT (0x07) ->
// context_snapshot_t, and now SAFETY_CMD_CLEAR_TRIP (0x0A) ->
// safety_core_request_clear_trip(), and the receiver hardening
// (resync-on-0x7E, bounded buffers, no allocation) LINK_PROTOCOL.md section 3
// requires. A later pass wires SAFETY_CMD_SET_FIRING_CEILING (0x09) and
// SAFETY_CMD_SET_CLOCK (0x0C): see link_task_handle_set_firing_ceiling() and
// link_task_handle_set_clock() below, and link_frame.h's
// link_frame_ceiling_is_active()/link_frame_firing_ceiling_should_apply()/
// link_frame_clock_epoch_is_plausible() for the host-tested bounds/gating
// logic each one leans on.
// Parsing the context frame is only half of Phase 7: nothing here yet acts
// on it (no S2/S3/S4/S6/S10 guard exists to disable on SIM_PLANT or reset on
// a boot_id change -- see the comments at the PUSH_CONTEXT case below and
// TODO.md's own notes on what remains).
//
// ROADMAP.md M5 pass: Frame B's send path now calls the shared
// kilnlink_diag_encode() codec (CommonFW/src/kilnlink_diag.c, host-tested in
// CommonFW/test/test_diag.c) instead of this file's own hand-rolled
// link_frame_pack_diag() -- the two were independent, byte-identical-by-
// construction implementations of the same 26-byte layout, and this removes
// the duplication rather than leaving it. Frame D (SAFETY_CMD_TRIP_EVENT) is
// new this pass: link_task_send_trip_event() below, driven by polling
// safety_core_get_trip_event() every loop iteration (safety_core.c is the
// producer, this file the consumer -- see that function's own doc comment
// for why this is the isolation-legal direction, same pattern
// safety_core_get_diag_status()/_get_output_status() already established).
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
#include "config_params.h" // param_id <-> config_store_record_t field mapping, see SET_PARAM/GET_PARAM/COMMIT_CONFIG/GET_CONFIG_PAGE handlers below
#include "config_store.h" // SAFETY_CMD_SET_CONFIG, see link_task_handle_set_config()
#include "current_task.h"
#include "discrete_task.h"
#include "log_task.h" // CLEAR_TRIP/SET_CONFIG outcome logging, see link_task_handle_clear_trip()/_set_config()
#include "max31856.h" // MAX31856_TC_TYPE_* range check, see link_task_handle_set_config()
#include "reboot_announce.h" // SAFETY_CMD_ANNOUNCE_REBOOT (0x18), see link_task_handle_announce_reboot()
#include "safety_core.h"
#include "snapshots.h"
#include "thermo_task.h"
#include "update_task.h" // Phase 10 -- UPDATE_BEGIN/_DATA/_END/_ABORT dispatch, see the switch below

#include "kilnlink/kilnlink_announce.h"
#include "kilnlink/kilnlink_announce_reboot.h" // SAFETY_CMD_ANNOUNCE_REBOOT, see link_task_handle_announce_reboot()
// Generated into the build dir by CMakeLists.txt's saftyfw_build_info target,
// regenerated on every build (see link_task_send_fw_version() below, the only
// consumer). Firmware-only: the host test binary does not compile this file
// (test/build_host_tests.ps1 pulls in link_frame.c, not link_task.c), so no
// host-side shim is needed for it.
#include "saftyfw_build_info.h"

#include "kilnlink/kilnlink_ceiling.h" // SAFETY_CMD_SET_FIRING_CEILING, see link_task_handle_set_firing_ceiling()
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_commit_config.h" // SAFETY_CMD_COMMIT_CONFIG (0x1D), see link_task_handle_commit_config()
#include "kilnlink/kilnlink_commit_config_rejected.h" // SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20), see link_task_send_commit_config_rejected()
#include "kilnlink/kilnlink_config_page.h" // SAFETY_CMD_CONFIG_PAGE reply, see link_task_send_config_page()
#include "kilnlink/kilnlink_ct_cal.h" // SAFETY_CMD_CT_CAL reply, see link_task_send_ct_cal()
#include "kilnlink/kilnlink_diag.h"
#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_get_config_page.h" // SAFETY_CMD_GET_CONFIG_PAGE (0x1F), see link_task_handle_get_config_page()
#include "kilnlink/kilnlink_get_ct_cal.h" // SAFETY_CMD_GET_CT_CAL, see link_task_handle_get_ct_cal()
#include "kilnlink/kilnlink_get_param.h" // SAFETY_CMD_GET_PARAM (0x1E request), see link_task_handle_get_param()
#include "kilnlink/kilnlink_param.h" // SAFETY_CMD_PARAM (0x1E reply), see link_task_send_param()
#include "kilnlink/kilnlink_power.h"
#include "kilnlink/kilnlink_rollback.h" // SAFETY_CMD_ROLLBACK, see link_task_handle_rollback()
#include "kilnlink/kilnlink_set_clock.h" // SAFETY_CMD_SET_CLOCK, see link_task_handle_set_clock()
#include "kilnlink/kilnlink_set_config.h"
#include "kilnlink/kilnlink_set_ct_cal.h" // SAFETY_CMD_SET_CT_CAL, see link_task_handle_set_ct_cal()
#include "kilnlink/kilnlink_set_log_level.h" // SAFETY_CMD_SET_LOG_LEVEL (0x1B), see link_task_handle_set_log_level()
#include "kilnlink/kilnlink_set_param.h" // SAFETY_CMD_SET_PARAM (0x1C), see link_task_handle_set_param()
#include "kilnlink/kilnlink_trip.h"
#include "kilnlink/kilnlink_version.h"

// Stack bumped from a single configMINIMAL_STACK_SIZE (Phase 10, this pass):
// LINK_RX_ASSEMBLY_MAX grew from 128 to KILNLINK_FRAME_STUFFED_MAX (~530
// bytes, see that macro's own comment below) to fit a full UPDATE_DATA frame,
// and link_task_handle_raw_frame() puts a same-sized `unstuffed[]` buffer on
// its own stack frame -- two ~530-byte buffers plus the usual call-depth
// margin no longer comfortably fits configMINIMAL_STACK_SIZE alone.
//
// Bumped again 2026-08-23, from *3 to *6, after *3 overflowed on hardware:
// core 0 was found parked in vApplicationStackOverflowHook() with
// pcTaskName = "link_task", one FreeRTOS tick after the scheduler started.
// Because that hook halts with interrupts disabled (main.c, deliberately),
// the whole system stopped there: xTickCount froze at 1, no other task ever
// ran, and the un-fed hardware watchdog rebooted the board about a second
// later, over and over. Every downstream symptom -- "core 1 never runs a
// task", "the isolated UART link never carries a frame", "the board is in a
// reset loop" -- was this one overflow.
//
// Measure before trimming this: link_task_handle_raw_frame()'s worst-case
// chain nests unstuffed[~520] + a handler's payload[] + link_send_frame()'s
// raw[~259] and stuffed[~520] in one call path, so the true peak is only
// reachable by a full-size UPDATE_DATA frame, not by the boot-time TX that
// happened to trip it first.
#define LINK_TASK_STACK_WORDS      (configMINIMAL_STACK_SIZE * 6)
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
// Frame D (SAFETY_CMD_TRIP_EVENT) repeat burst: LINK_PROTOCOL.md sec 6,
// "pushed immediately... and repeated a few times over the next second in
// case the first copy is lost (there is no ACK)". Mirrors the exact burst
// shape KilnFW's own safety_link_send_announce_version_burst() already uses
// for ANNOUNCE_VERSION (4 copies, 250ms apart, ~750ms total -- see that
// function's own comment) rather than inventing a new cadence for the same
// "loss-tolerant unsolicited burst" idea. This is NOT the retransmission
// rule 2 of LINK_PROTOCOL.md sec 2 forbids: the burst count and timing are
// fixed at trip-detection time and fire unconditionally, never conditioned
// on whether an earlier copy in the same burst was "lost" (nothing here can
// even tell) or on any signal from the ESP.
#define LINK_TRIP_REPEAT_COUNT     4u
#define LINK_TRIP_REPEAT_PERIOD_MS 250

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

// Frame D (TRIP_EVENT) burst state. Touched only from link_task_fn / the
// functions it calls (all this task) -- no lock, same single-writer
// reasoning as the counters above. s_trip_last_seq_seen tracks the newest
// safety_core_get_trip_event() trip_seq this task has already started a
// burst for, so a trip -> clear -> re-trip pair produces two independent
// bursts rather than one being silently dropped as "already handled".
// s_pending_trip is filled ONCE, when a new seq is first observed, and
// reused unmodified for every repeat in the burst -- a trip event describes
// one instant, and every copy on the wire must describe the SAME instant,
// not a fresh live read each time (current_task/context_snapshot values in
// particular could otherwise drift between repeats of what is supposed to be
// one event).
static uint8_t s_trip_last_seq_seen = 0;
static kilnlink_trip_t s_pending_trip;
static unsigned s_trip_repeats_pending = 0;
static TickType_t s_last_trip_tx_tick = 0;

// Link liveness (SAFETY_MODEL.md section 4, S6b), snapshots.h's
// link_task_link_up() doc comment. Single-writer: only
// link_task_handle_raw_frame() below ever touches these, right after a
// frame's CRC/length/type checks pass in kilnlink_frame_decode() -- BEFORE
// the BROADCAST-only filter just below it, deliberately: S6b's question is
// "is the ESP alive and transmitting", not "did it send us something we act
// on", so even a frame this build ends up discarding for its type still
// proves the peer is there.
static TickType_t s_last_valid_frame_tick = 0;
static bool s_valid_frame_seen = false;
// A software recency window, not a measured physical constant (same category
// as REBOOT_GRACE_WINDOW_MS, safety_core.c) -- comfortably above the ESP's
// ~500ms PUSH_CONTEXT cadence (CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS,
// firmware/KilnFW/App/drivers/Kconfig) plus margin for scheduling jitter, and
// far below S6b's own 10s soft / 120s hard timeouts -- so a link_up that
// occasionally reads false between two healthy frames costs nothing (safety_
// guards.c's own s6b_link_down_elapsed_s accumulator tolerates that exactly
// the way it tolerates any other brief link_up==false tick), while a link_up
// that reads true only for genuinely recent traffic is what keeps the 10s/
// 120s timers meaningful.
#define LINK_UP_RECENCY_MS 1000u

// S4's "commanded on throughout the correlation window" fact (snapshots.h's
// link_task_get_relay_on_continuous_ms() doc comment) -- single-writer,
// touched only from link_task_handle_push_context() below.
static TickType_t s_relay_on_since_tick = 0;
static bool s_relay_on_continuous = false;

// SAFETY_CMD_SET_FIRING_CEILING (0x09) -- single-writer, touched only from
// link_task_handle_set_firing_ceiling() (this task), read from any task via
// link_task_get_firing_ceiling() (snapshots.h), same plain-read reasoning as
// s_context_frames_ok/bad above. s_firing_ceiling_have is the ALREADY-bounds-
// checked fact (link_frame_ceiling_is_active()) that the last decoded frame
// named an active ceiling; s_firing_ceiling_c is only meaningful while it is
// true. RAM-only, never config_store -- see link_task_handle_set_firing_
// ceiling()'s own comment for why this is context, not commissioning.
static bool s_firing_ceiling_have = false;
static float s_firing_ceiling_c = 0.0f;

// SAFETY_CMD_SET_CLOCK (0x0C) -- same single-writer shape as the ceiling
// state just above. Purely diagnostic (LINK_PROTOCOL.md section 4: "no guard
// may ever read this clock") -- see link_task_handle_set_clock()'s own
// comment.
static bool s_wall_clock_have = false;
static uint64_t s_wall_clock_epoch_ms = 0;

// Commissioning staging (docs/COMMISSIONING.md section 2: "Staged in RAM,
// then committed as one record"). Single-writer, same reasoning as every
// other plain static above: only SET_PARAM/COMMIT_CONFIG's handlers below
// ever touch this, both on link_task's own thread. Lazily seeded from
// config_store_get_full_record() (the currently COMMITTED record) the first
// time either handler runs, so a board that has never had a single
// SET_PARAM this boot starts staging from what is actually enforced, not
// from a blank record that would silently discard every field a PRIOR boot
// already committed. After a successful COMMIT_CONFIG, this becomes the new
// baseline for whatever SET_PARAM comes next -- exactly "staged, then
// committed as one record, then staged again from there."
static config_store_record_t s_staged_config;
static bool s_staged_config_init = false;

static void link_task_ensure_staged_config(void)
{
    if (s_staged_config_init) {
        return;
    }
    config_store_get_full_record(&s_staged_config);
    s_staged_config_init = true;
}

// --- TX ----------------------------------------------------------------

// --- DIAGNOSTIC: 2026-08-23 truncation investigation --------------------
// The ESP consistently reports a truncated frame ("hdr says 176, got 142
// bytes") on the config-page reply, and the deframer + uart_owner_send()
// drop counters have both been ruled out (see link_task_send_broadcast_to()
// below). These statics latch what this Pico actually built and handed to
// uart_owner_send() for the most recent frame of any kind, and separately
// for the config-page reply specifically (cmd 0x1F,
// KILNLINK_GET_CONFIG_PAGE_CMD), since the 500 ms status broadcast almost
// always overwrites the generic set before it can be read over SWD.
// `volatile` so the compiler can't optimize the stores away or keep them
// in a register -- nothing in the firmware reads these back.
//   s_last_tx_payload_len / s_last_page_payload_len   -- `length` argument (pre-encode payload size)
//   s_last_tx_raw_len     / s_last_page_raw_len        -- kilnlink_frame_encode_raw() return (unstuffed frame bytes)
//   s_last_tx_stuffed_len / s_last_page_stuffed_len    -- kilnlink_stuff() return (post-stuffing byte-stream length)
//   s_last_tx_cmd                                       -- payload[0], the command id, so we know which frame type this was
//   s_last_tx_accepted    / s_last_page_accepted        -- uart_owner_send()'s bool result
// Safe to delete once the truncation's cause is found -- purely diagnostic,
// no effect on behavior.
static volatile uint32_t s_last_tx_payload_len;
static volatile uint32_t s_last_tx_raw_len;
static volatile uint32_t s_last_tx_stuffed_len;
static volatile uint8_t  s_last_tx_cmd;
static volatile uint32_t s_last_tx_accepted;

static volatile uint32_t s_last_big_payload_len = 0;
static volatile uint32_t s_last_big_raw_len = 0;
static volatile uint32_t s_last_big_stuffed_len = 0;
static volatile uint32_t s_last_big_accepted = 0;
static volatile uint32_t s_last_big_count = 0;
static volatile uint8_t  s_last_big_cmd = 0;
static volatile uint8_t  s_last_big_dst_task = 0;
static volatile uint32_t s_last_page_payload_len;
static volatile uint32_t s_last_page_raw_len;
static volatile uint32_t s_last_page_stuffed_len;
static volatile uint32_t s_last_page_accepted;
// --- end diagnostic statics ----------------------------------------------

// --- DIAGNOSTIC: 2026-08-23 GET_CONFIG_PAGE stage1-vs-stage2 investigation -
// The statics above proved this Pico never TRANSMITS a config-page reply
// (s_last_page_* stayed all-zero over minutes of runtime), but that alone
// does not say whether the ESP's SAFETY_CMD_GET_CONFIG_PAGE request never
// arrives/dispatches (stage 1: link_task_handle_raw_frame()'s decode/
// BROADCAST-filter/switch) or whether it dispatches into
// link_task_handle_get_config_page() but that handler (or
// link_task_send_config_page() below it) declines to reply (stage 2).
// These four latch exactly that boundary, over SWD, without guessing:
//   s_diag_dispatch_accepted_count     -- bumped once per BROADCAST frame
//     that passes kilnlink_unstuff()+kilnlink_frame_decode()+the
//     msg_type==BROADCAST/length!=0 filter, i.e. every frame that reaches
//     the dispatch switch at all (any cmd, not just GET_CONFIG_PAGE). If
//     this never moves, the request never even decodes -- stage 1, before
//     the switch.
//   s_diag_get_config_page_seen_count  -- bumped once per dispatched frame
//     whose payload[0] == KILNLINK_GET_CONFIG_PAGE_CMD, REGARDLESS of
//     whether frame.length matches KILNLINK_GET_CONFIG_PAGE_LEN. If this
//     stays zero while s_diag_dispatch_accepted_count moves, the request is
//     arriving as some OTHER frame type/cmd byte (stage 1, wrong cmd) --
//     if this moves but s_diag_get_config_page_handled_count does not, the
//     frame's length never matches (stage 1, wrong length).
//   s_diag_get_config_page_handled_count -- bumped once per GET_CONFIG_PAGE
//     frame whose length DID match, i.e. every call into
//     link_task_handle_get_config_page(). If this moves, stage 1 is cleared
//     entirely: the request arrives and dispatches correctly, and any
//     silence is stage 2, inside the handler or link_task_send_config_page().
//   s_diag_page_last_outcome -- one of the DIAG_PAGE_OUTCOME_* values below,
//     set at every exit point link_task_handle_get_config_page()/
//     link_task_send_config_page() can take, so the LAST one latched shows
//     exactly why the last request got no reply.
// `volatile` for the same reason as the block above (nothing reads these
// back in firmware, only SWD). Safe to delete once this is settled --
// purely diagnostic, no effect on behavior.
static volatile uint32_t s_diag_dispatch_accepted_count = 0;
static volatile uint32_t s_diag_get_config_page_seen_count = 0;
static volatile uint32_t s_diag_get_config_page_handled_count = 0;
static volatile uint8_t  s_diag_page_last_outcome = 0;
#define DIAG_PAGE_OUTCOME_NONE            0u /* never touched this boot */
#define DIAG_PAGE_OUTCOME_HANDLER_ENTERED 1u /* handler running, no verdict yet -- should never be the LAST value observed */
#define DIAG_PAGE_OUTCOME_DECODE_REJECTED 2u /* kilnlink_get_config_page_decode() rejected the request payload */
#define DIAG_PAGE_OUTCOME_PACK_FAILED     3u /* kilnlink_config_page_pack() returned 0 -- link_task_send_config_page() returned without sending */
#define DIAG_PAGE_OUTCOME_REPLIED         4u /* link_task_send_broadcast() was called with the packed reply (accepted or not -- see s_last_page_accepted for that) */
// --- end diagnostic statics ----------------------------------------------

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
    bool accepted = uart_owner_send(stuffed, stuffed_len);

    // --- DIAGNOSTIC: 2026-08-23 truncation investigation, see statics
    // declared above this function -- pure recording, no control-flow effect.
    s_last_tx_payload_len = length;
    s_last_tx_raw_len = (uint32_t)raw_len;
    s_last_tx_stuffed_len = (uint32_t)stuffed_len;
    s_last_tx_cmd = (length > 0) ? payload[0] : 0;
    s_last_tx_accepted = accepted ? 1u : 0u;
    // Any frame big enough to be the one the ESP reports truncated. The
    // config-page latch below stayed all-zero on the bench, which proved the
    // Pico never sends a config page at all -- so the 176-byte frame the ESP
    // complains about is some OTHER frame type, and this catches it whatever
    // it is. dst_task is recorded because that is what distinguishes a LOG
    // frame from a SAFETY one.
    if (length >= 100u) {
        s_last_big_payload_len = length;
        s_last_big_raw_len = (uint32_t)raw_len;
        s_last_big_stuffed_len = (uint32_t)stuffed_len;
        s_last_big_cmd = (length > 0) ? payload[0] : 0;
        s_last_big_dst_task = dst_task;
        s_last_big_accepted = accepted ? 1u : 0u;
        s_last_big_count++;
    }
    if (length > 0 && payload[0] == KILNLINK_GET_CONFIG_PAGE_CMD) {
        s_last_page_payload_len = length;
        s_last_page_raw_len = (uint32_t)raw_len;
        s_last_page_stuffed_len = (uint32_t)stuffed_len;
        s_last_page_accepted = accepted ? 1u : 0u;
    }
    // --- end diagnostic ---

    return accepted;
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
    // Build identity now comes from saftyfw_build_info.h, regenerated on
    // EVERY build by CMakeLists.txt's saftyfw_build_info target (not merely
    // on reconfigure -- see that target's comment, and KilnFW's equivalent
    // gen_build_info.cmake, which this mirrors). Before that generator
    // existed this function hard-coded dirty = 1 with empty commit/datetime
    // fields: honest, because LINK_PROTOCOL.md section 4's rule is "unknown
    // must map to dirty", but useless for telling two builds apart.
    //
    // SAFTYFW_GIT_DIRTY is already 0/1 from the generator and is passed
    // straight through, so a clean tree now reports clean -- the one
    // direction the old hard-coded 1 could never express. The commit and
    // datetime strings go on the wire as ASCII, NOT null-terminated, exactly
    // strlen() bytes each (see link_frame_pack_fw_version()'s contract).
    //
    // config_version/config_crc come from config_store's real cache (Phase
    // 9's store landed, and SAFETY_CMD_SET_CONFIG's
    // link_task_handle_set_config() above is its first writer) -- still 0/0
    // on a never-commissioned board, since that is exactly
    // config_store_default()'s seq=0 record, which the spec documents as
    // meaning "running on compiled-in defaults that were never commissioned."
    //
    // Payload capacity: the old buffer was 16 bytes, sized for the
    // fixed-width fields alone when both strings were empty. It has to hold
    // the strings now; pack_fw_version() returns 0 (writing nothing) rather
    // than overflowing if this is ever too small, so a short buffer would
    // silently stop the frame being sent at all -- hence sizing it from the
    // generated strings themselves rather than a guessed constant.
    static const char commit_str[] = SAFTYFW_GIT_COMMIT;
    static const char datetime_str[] = SAFTYFW_BUILD_DATE " " SAFTYFW_BUILD_TIME;
    uint8_t payload[16 + sizeof(commit_str) + sizeof(datetime_str)];
    size_t len = link_frame_pack_fw_version(
        payload, sizeof(payload), KILNLINK_PROTOCOL_VERSION, KILNLINK_MIN_COMPATIBLE,
        SAFTYFW_GIT_DIRTY ? 1u : 0u, commit_str, sizeof(commit_str) - 1u, datetime_str,
        sizeof(datetime_str) - 1u, s_boot_id, config_store_get_config_version(),
        config_store_get_config_crc());
    if (len == 0) {
        return;
    }

    link_task_send_broadcast(payload, (uint8_t)len);
}

// SAFETY_CMD_CT_CAL (0x1A) reply -- sent in answer to SAFETY_CMD_GET_CT_CAL
// (link_task_handle_get_ct_cal() below), same shared-id/reply-on-request
// shape as link_task_send_fw_version() above. Reports config_store's cached
// ct_cal exactly, three channels' calibrated/gain/offset -- see kilnlink_
// ct_cal.h's header comment for why this exists (the GUI's way to show what
// SaftyFW is actually correcting current with right now, not what a bench
// tool last claimed to upload).
static void link_task_send_ct_cal(void)
{
    config_store_ct_channel_cal_t stored[CONFIG_STORE_CT_CAL_NUM_CHANNELS];
    config_store_get_ct_cal(stored);

    kilnlink_ct_cal_t cal;
    for (unsigned ch = 0; ch < KILNLINK_CT_CAL_NUM_CHANNELS && ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS;
         ch++) {
        cal.channels[ch].calibrated = stored[ch].calibrated ? 1u : 0u;
        cal.channels[ch].gain = stored[ch].gain;
        cal.channels[ch].offset = stored[ch].offset;
    }

    uint8_t payload[KILNLINK_CT_CAL_LEN];
    kilnlink_ct_cal_status_t status;
    size_t len = kilnlink_ct_cal_encode(&cal, payload, sizeof(payload), &status);
    if (len == 0) {
        return;
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

// Shared by link_task_send_diag() and link_task_send_trip_event(): both
// frames carry "how long since the last well-formed PUSH_CONTEXT" (DIAG byte
// 11, TRIP_EVENT byte 28), same sentinel (255 = never received) and same
// clamp (254 max, so a genuinely-stale-but-received context is never
// misread as "never received" by colliding with the sentinel). Factored out
// here rather than left duplicated inline a second time, now that a second
// call site needs it.
static uint8_t link_task_context_age_100ms(void)
{
    if (s_context_frames_ok == 0) {
        return 255u;
    }
    TickType_t age_ticks = xTaskGetTickCount() - s_last_context_rx_tick;
    uint32_t age_ms = (uint32_t)age_ticks * portTICK_PERIOD_MS;
    uint32_t age_100ms = age_ms / 100u;
    return (age_100ms > 254u) ? 254u : (uint8_t)age_100ms;
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
    uint16_t trip_mask = link_frame_trip_mask_for_reason(trip_reason);
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
    // kilnlink_diag.h's KILNLINK_DIAG_BOOT_BROWNOUT comment.
    saftyfw_boot_reason_t boot = boot_reason_get_cached();
    uint8_t boot_reason_byte = 0;
    if (boot.watchdog_caused_reboot) {
        boot_reason_byte |= KILNLINK_DIAG_BOOT_WATCHDOG;
    } else {
        boot_reason_byte |= KILNLINK_DIAG_BOOT_POWERON;
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
    uint8_t context_age_100ms = link_task_context_age_100ms();

    // flags bit0 sim_context_seen: real now, from s_context_sim_seen (see
    // that variable's declaration for the "latched, never cleared" contract).
    uint8_t diag_flags = KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    if (s_context_sim_seen) {
        diag_flags |= KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN;
    }
    // flags: bit1 calibration_missing = 1 (no config_store, Phase 9 -- this
    // is the honest current state, not a bug); bit2 estop_unwired_suspect = 0
    // (no detection heuristic specified in SAFETY_MODEL.md/HARDWARE.md or
    // built). Note what parsing PUSH_CONTEXT does NOT yet mean: there is
    // still no context-consuming correlation guard (S2/S3/S4/S6/S10) to
    // disable on sim_context_seen or reset on a boot_id change -- this frame
    // reports that the fact is known, not that anything downstream acts on
    // it yet.
    //
    // Built via the shared kilnlink_diag_encode() codec (CommonFW/src/
    // kilnlink_diag.c, host-tested test_diag.c) rather than this file's own
    // former hand-rolled link_frame_pack_diag() -- ROADMAP.md M5: the two
    // were duplicate implementations of the identical 26-byte layout, and
    // this removes the duplication now that the shared codec exists (it
    // predates this file's own packer having been written before
    // kilnlink_diag.c landed).
    kilnlink_diag_t dg = {
        .trip_reason = (uint8_t)trip_reason,
        .warn_mask = warn_mask,
        .trip_mask = trip_mask,
        .uptime_ms = uptime_ms,
        .boot_reason = boot_reason_byte,
        .context_age_100ms = context_age_100ms,
        .context_frames_ok = s_context_frames_ok,
        .context_frames_bad = s_context_frames_bad,
        .tx_frames_dropped = uart_owner_get_tx_dropped(),
        .state = diag_state,
        .flags = diag_flags,
    };
    uint8_t payload[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t len = kilnlink_diag_encode(&dg, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // can't happen for a fixed sizeof(payload) == KILNLINK_DIAG_LEN buffer
    }

    link_task_send_broadcast(payload, (uint8_t)len);
}

// Frame D (SAFETY_CMD_TRIP_EVENT, 0x0D) -- CommonFW/docs/LINK_PROTOCOL.md
// sec 6: "pushed immediately... repeated a few times over the next second."
// tr must already be fully populated (link_task_fn()'s poll loop below is
// the only caller, and it fills s_pending_trip once per newly-observed
// trip_seq, reusing it unmodified for every repeat -- see that struct's own
// declaration comment for why).
static void link_task_send_trip_event(const kilnlink_trip_t *tr)
{
    uint8_t payload[KILNLINK_TRIP_LEN];
    kilnlink_trip_status_t status;
    size_t len = kilnlink_trip_encode(tr, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // can't happen for a fixed sizeof(payload) == KILNLINK_TRIP_LEN buffer
    }

    link_task_send_broadcast(payload, (uint8_t)len);
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

    // S4's continuously-on tracking (see s_relay_on_since_tick's own
    // declaration comment): relay_now_mask is the only wire fact this frame
    // carries that is a true instant, so "continuously on" can only be
    // derived by watching it across the sequence of frames actually
    // received, here, where that sequence is single-writer-safe.
    bool relay_now_on = (snap.relay_now_mask != 0u);
    if (relay_now_on) {
        if (!s_relay_on_continuous) {
            s_relay_on_since_tick = xTaskGetTickCount();
            s_relay_on_continuous = true;
        }
    } else {
        s_relay_on_continuous = false;
    }
}

static void link_task_handle_announce_version(const kilnlink_frame_t *frame)
{
    // ROADMAP.md M2/M8: parse via the shared kilnlink_announce_decode()
    // codec (CommonFW/src/kilnlink_announce.c, host-tested) instead of this
    // file's own hand-rolled fixed-offset read -- same pattern already used
    // for CLEAR_TRIP above. Malformed/truncated/wrong-cmd frames are
    // discarded silently, same idiom every other decode failure in this file
    // uses; only protocol_version/min_compatible (the first two fields) are
    // needed here, per LINK_PROTOCOL.md section 4/6's "read bytes 1-4 first"
    // guidance that kilnlink_announce.h's own doc comment echoes.
    kilnlink_announce_t msg;
    kilnlink_announce_status_t dstatus =
        kilnlink_announce_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_ANNOUNCE_OK) {
        return;
    }

    uint16_t peer_protocol = msg.protocol_version;
    uint16_t peer_min_compatible = msg.min_compatible;

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

// SAFETY_CMD_REQUEST_ENABLE (0x02), CommonFW/docs/LINK_PROTOCOL.md section 4
// ("Kept, converted to BROADCAST. Advisory only -- the Pico's interlocks
// always win"). 2-byte payload: cmd (0x02) + a 0/1 enable byte, exactly what
// firmware/KilnFW/App/drivers/safety_link.c's safety_link_request_enable()
// sends (`{ SAFETY_CMD_REQUEST_ENABLE, enable ? 1u : 0u }`). No kilnlink_*
// codec exists for this frame in CommonFW (unlike CLEAR_TRIP/SET_CONFIG/
// ROLLBACK/ANNOUNCE_REBOOT above) and this pass is not authorised to add one
// there, so the 2-byte payload is validated and read inline here rather than
// through a bespoke link_frame.c unpacker -- there is nothing left to get
// wrong once frame->length == 2 is checked.
//
// Never touches relay_owner directly -- link_task.c is structurally
// forbidden from naming the relay at all (tools/check_isolation.ps1, this
// file's own header comment), so this only ever asks
// safety_core_request_enable(), the same shape as
// link_task_handle_clear_trip() calling safety_core_request_clear_trip()
// below. Every refusal path (TRIPPED refuses outright, GRACE accepts the
// command but never actually drives GPIO6 high, only ARMED honours it) is
// enforced inside relay_owner's own state machine
// (src/tasks/relay_owner.c's RELAY_OWNER_CMD_ENERGIZE case) -- safety_core_
// request_enable() is a thin forward, not a second policy layer, so there is
// exactly one place a refusal could be silently dropped, and it is not this
// one.
#define LINK_FRAME_REQUEST_ENABLE_CMD 0x02u

static void link_task_handle_request_enable(const kilnlink_frame_t *frame)
{
    if (frame->length != 2u) {
        // Malformed/wrong-length -- untrusted wire input, discarded silently
        // like every other decode failure in this file.
        return;
    }
    bool enable = frame->payload[1] != 0u;

    bool accepted = safety_core_request_enable(enable);
    if (enable) {
        log_task_log(accepted ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, "request_enable",
                     accepted ? "accepted" : "refused, interlocks");
    } else {
        // A disable request always succeeds (relay_owner_command_energize()
        // has no refusal path for energize == false) -- logged at INFO,
        // never WARN, since there is nothing advisory being overridden here.
        log_task_log(LOG_LEVEL_INFO, "request_enable", "disable requested");
    }
}

// SAFETY_CMD_CLEAR_TRIP (0x0A), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// the GUI's path to acknowledging a trip. This function only decodes and
// validates the wire frame and logs the outcome; the actual refuse/clear
// policy (still-tripped retick) is safety_core_request_clear_trip()'s job,
// called from here the same direction link_task already calls
// safety_core_get_diag_status()/_get_output_status()/_get_trip_event() --
// link_task calling INTO safety_core, never the reverse, so this file still
// never needs to be called by, or export anything to, safety_core.c.
//
// Two refusal paths per the protocol doc, both checked here before
// safety_core is even asked:
//   1. Nothing currently tripped (trip_reason == SAFETY_TRIP_NONE) -- there
//      is nothing to clear, and the wire trip_mask (whatever the ESP sent)
//      cannot possibly match a mask of 0 from an actual trip, so this is
//      also naturally a mismatch. Called out separately for a clearer log
//      line.
//   2. `trip_mask` doesn't match the currently-latched one -- LINK_PROTOCOL.md:
//      "prevents a stale clear queued before a second, different trip from
//      clearing that one too." Computed via
//      link_frame_trip_mask_for_reason(), the exact same single-bit
//      degraded-approximation this build's Frame B (DIAG) already reports,
//      so a GUI that echoes back the trip_mask it last saw in a DIAG frame
//      matches correctly.
//
// Never ACKs on the wire -- link_task never participates in the ACK'd
// transport (see link_task_handle_raw_frame()'s own BROADCAST-only check
// above) and LINK_PROTOCOL.md does not ask CLEAR_TRIP to reply; the ESP
// observes the outcome via the tripped bit in the next Frame A/B it
// receives.
static void link_task_handle_clear_trip(const kilnlink_frame_t *frame)
{
    kilnlink_clear_trip_t msg;
    kilnlink_clear_trip_status_t dstatus =
        kilnlink_clear_trip_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_CLEAR_TRIP_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file (see
        // link_task_handle_raw_frame()'s own comments).
        return;
    }

    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    safety_core_get_diag_status(&trip_reason, NULL, NULL);

    // link_frame_decide_clear_trip() (src/tasks/link_frame.c) is the pure,
    // host-tested extraction of the two refusal checks documented above --
    // this function only acts on its verdict now.
    link_clear_trip_decision_t decision = link_frame_decide_clear_trip(trip_reason, msg.trip_mask);
    if (decision == LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED) {
        log_task_log(LOG_LEVEL_INFO, "clear_trip", "ignored, nothing tripped");
        return;
    }
    if (decision == LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH) {
        log_task_log(LOG_LEVEL_WARN, "clear_trip", "refused, trip_mask mismatch");
        return;
    }

    bool cleared = safety_core_request_clear_trip();
    if (cleared) {
        log_task_log(LOG_LEVEL_INFO, "clear_trip", "accepted");
    } else {
        log_task_log(LOG_LEVEL_WARN, "clear_trip", "refused, condition still holds");
    }
}

// SAFETY_CMD_SET_CONFIG (0x16), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// the GUI's path to commissioning config_store.h's tc_type (TODO.md Phase 9's
// "SAFETY_CMD_SET_CONFIG (wire command) -- still not done"). Same shape as
// link_task_handle_clear_trip() immediately above: decode and validate the
// wire frame here, log accept/refuse, and never ACK on the wire -- the PC
// observes the outcome via the next GET_DIAG/GET_FW_VERSION poll, not a reply
// to this frame (LINK_PROTOCOL.md sec 4's SET_CONFIG entry).
//
// Two refusal paths, both checked (or delegated) before any flash write is
// attempted:
//   1. `tc_type` isn't a value this firmware recognises as a
//      MAX31856_TC_TYPE_* -- checked here, since config_store.c/.h are
//      deliberately dependency-free of max31856.h (config_store.h's own doc
//      comment) and so cannot make this check themselves.
//   2. The relay is currently ARMED -- config_store_write()'s own
//      unconditional refusal (config_store_decide_write()), delegated to
//      config_store_flash.c rather than duplicated here. This is the one
//      place in this file that reaches (indirectly, through config_store.c)
//      into relay state -- but link_task.c itself still never names GPIO6 or
//      the relay, which is what THE ONE RULE THAT MATTERS (this file's
//      header comment) actually forbids; config_store_flash.c is the file
//      that legitimately depends on relay_owner, the same "safety_core
//      already legitimately depends on relay_owner" carve-out this file's
//      header comment already documents for output-status reads.
static void link_task_handle_set_config(const kilnlink_frame_t *frame)
{
    kilnlink_set_config_t msg;
    kilnlink_set_config_status_t dstatus =
        kilnlink_set_config_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_SET_CONFIG_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file (see
        // link_task_handle_raw_frame()'s own comments).
        return;
    }

    if (msg.tc_type > MAX31856_TC_TYPE_T) {
        log_task_log(LOG_LEVEL_WARN, "set_config", "refused, tc_type out of range");
        return;
    }

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = msg.tc_type;
    rec.calibration_missing = true; // a new tc_type invalidates any prior
                                     // calibration -- see config_store.h's
                                     // own doc comment on this field; there
                                     // is no calibration-clearing mechanism
                                     // yet (TODO.md Phase 9's later bullets),
                                     // so every SET_CONFIG conservatively
                                     // re-arms it.

    const char *reason = NULL;
    bool written = config_store_write(&rec, &reason);
    if (written) {
        log_task_log(LOG_LEVEL_INFO, "set_config", "accepted");
    } else {
        log_task_log(LOG_LEVEL_WARN, "set_config", reason ? reason : "refused");
    }
}

// SAFETY_CMD_SET_CT_CAL (0x19), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// firmware/SimFW/tools/ct_calibration/README.md's documented gap: "no MCP
// tool exists to push calibration constants ... to the RP2040's own flash."
// Same fire-and-forget shape as link_task_handle_set_config() immediately
// above: decode, validate, log accept/refuse, never ACK on the wire.
//
// Read-modify-write: config_store_write() replaces the ENTIRE record, so
// this fetches every field first (tc_type/calibration_missing/all three
// ct_cal channels via config_store_get_ct_cal()) and overwrites only the
// ONE channel this frame named -- setting channel 1 must never disturb
// channel 0 or 2's stored constants, and must never re-arm calibration_
// missing or change tc_type, both of which are a SEPARATE commissioning
// concern (thermocouple, not CT current) that SET_CT_CAL has no business
// touching.
//
// Two refusal paths, same split as link_task_handle_set_config():
//   1. `channel` is out of range -- checked here, since kilnlink_set_ct_cal.c
//      only serializes bytes and has no opinion on the channel count.
//   2. The relay is currently ARMED -- config_store_write()'s own
//      unconditional refusal, delegated to config_store_flash.c exactly as
//      SET_CONFIG's does.
static void link_task_handle_set_ct_cal(const kilnlink_frame_t *frame)
{
    kilnlink_set_ct_cal_t msg;
    kilnlink_set_ct_cal_status_t dstatus =
        kilnlink_set_ct_cal_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_SET_CT_CAL_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    if (msg.channel >= CONFIG_STORE_CT_CAL_NUM_CHANNELS) {
        log_task_log(LOG_LEVEL_WARN, "set_ct_cal", "refused, channel out of range");
        return;
    }

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = config_store_get_tc_type();
    rec.calibration_missing = config_store_is_calibration_missing();
    config_store_get_ct_cal(rec.ct_cal);
    rec.ct_cal[msg.channel].calibrated = (msg.calibrated != 0u);
    rec.ct_cal[msg.channel].gain = msg.gain;
    rec.ct_cal[msg.channel].offset = msg.offset;

    const char *reason = NULL;
    bool written = config_store_write(&rec, &reason);
    if (written) {
        log_task_log(LOG_LEVEL_INFO, "set_ct_cal", "accepted");
        // Take effect immediately, not after a reboot -- current_task.c's
        // own comment on current_task_reload_ct_cal() explains why a live
        // commissioning session needs this, unlike tc_type (which only ever
        // takes effect via max31856_configure() at boot today).
        current_task_reload_ct_cal();
    } else {
        log_task_log(LOG_LEVEL_WARN, "set_ct_cal", reason ? reason : "refused");
    }
}

// SAFETY_CMD_GET_CT_CAL (0x1A), request only -- CommonFW/docs/LINK_PROTOCOL.md
// section 4. Same shape as SAFETY_CMD_GET_FW_VERSION above: answer every
// copy seen (the ESP is the side allowed to retry), reply via link_task_
// send_ct_cal() under the same wire id, distinguished by direction/length.
static void link_task_handle_get_ct_cal(const kilnlink_frame_t *frame)
{
    kilnlink_get_ct_cal_t msg;
    kilnlink_get_ct_cal_status_t dstatus =
        kilnlink_get_ct_cal_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_GET_CT_CAL_OK) {
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    (void)msg; // no fields
    link_task_send_ct_cal();
}

// SAFETY_CMD_ROLLBACK (0x17), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico half (the ESP
// half, POST /api/ota/esp/rollback, already exists). Same shape as
// link_task_handle_clear_trip()/link_task_handle_set_config() above: decode
// the wire frame here, log accept/refuse, never ACK on the wire -- the PC
// observes the outcome (or, on acceptance, simply a reconnect after the
// reboot) rather than a reply to this frame.
//
// Unlike CLEAR_TRIP/SET_CONFIG, all of the actual policy -- the ARMED check
// AND the "is the other bootloader slot valid to fall back to" gate that is
// this whole feature's load-bearing correctness property -- lives in
// update_task_request_rollback() (update_task.c), called synchronously
// here. See that function's own doc comment for why a synchronous call is
// right for this (small, bounded) flash write, following config_store_
// write()'s own precedent rather than UPDATE_*'s queued-to-update_task
// shape built for up-to-832K transfers.
static void link_task_handle_rollback(const kilnlink_frame_t *frame)
{
    kilnlink_rollback_t msg;
    kilnlink_rollback_status_t dstatus =
        kilnlink_rollback_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_ROLLBACK_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file (see
        // link_task_handle_raw_frame()'s own comments).
        return;
    }

    // Logged BEFORE the call: update_task_request_rollback() does not
    // return on success (watchdog_reboot() resets the board immediately),
    // so this is the only chance to record acceptance at all -- the refusal
    // path below is what actually executes and gets logged when the request
    // does not succeed.
    log_task_log(LOG_LEVEL_WARN, "rollback", "requested");

    const char *reason = NULL;
    bool accepted = update_task_request_rollback(&reason);
    // Reached only on refusal/failure -- see the function's own doc comment.
    if (!accepted) {
        log_task_log(LOG_LEVEL_WARN, "rollback", reason ? reason : "refused");
    }
}

// SAFETY_CMD_ANNOUNCE_REBOOT (0x18), CommonFW/docs/LINK_PROTOCOL.md section
// 4 -- KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP
// reboots" line. Fire-and-forget, never ACKs on the wire, same shape as
// link_task_handle_rollback() above minus the policy call: there is nothing
// to accept or refuse here, only a fact to record. reboot_announce_mark()
// stores the local (Pico) uptime at which this frame was decoded;
// safety_core.c reads it back to compute S6b's bounded grace-window fact
// (safety_guard_input_t::reboot_grace_active) -- this function has no
// opinion about how long that window is or what it suppresses, matching
// this file's "publish the fact, let safety_core decide" division with
// every other context/status field it hands off.
static void link_task_handle_announce_reboot(const kilnlink_frame_t *frame)
{
    kilnlink_announce_reboot_t msg;
    kilnlink_announce_reboot_status_t dstatus =
        kilnlink_announce_reboot_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_ANNOUNCE_REBOOT_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input,
        // discarded silently like every other decode failure in this file.
        return;
    }

    reboot_announce_mark(to_ms_since_boot(get_absolute_time()));
    log_task_log(LOG_LEVEL_INFO, "announce_reboot",
                 "ESP announced an imminent reboot -- S6b's trip is grace-windowed, "
                 "link_up and every other guard are unaffected");
}

// SAFETY_CMD_SET_FIRING_CEILING (0x09), CommonFW/docs/LINK_PROTOCOL.md
// section 4 / SAFETY_MODEL.md section 4, S1 -- "the highest target
// temperature this firing will ever ask for", so S1's absolute ceiling can
// tighten from a single fixed abs_max_temp_c to
// min(abs_max_temp_c, firing_max_c + firing_margin_c) for the firing actually
// running. RAM-only, deliberately NOT config_store: safety_guards.h's own
// header comment on cfg.firing_max_c/firing_max_valid is explicit this is
// "context, not config" -- it changes with whatever firing is running right
// now, unlike tc_type/ct_cal (SAFETY_CMD_SET_CONFIG/SET_CT_CAL above), which
// are bench-commissioned constants that must survive a reboot. Persisting a
// firing's peak target across a power cycle would be actively wrong: the
// next boot may run a completely different profile, and a stale persisted
// ceiling would tighten (or, worse if ever loosened by a future change) S1
// against a firing that is not the one happening.
//
// Never ACKs on the wire, same fire-and-forget shape as every other ESP->Pico
// command in this file -- the ESP has no reason to know this landed beyond
// whatever else it already polls (DIAG/FW_VERSION).
//
// Bounds checked at kilnlink_ceiling_decode() (wire shape: exactly 5 bytes,
// cmd byte matches) AND, separately, by link_frame_ceiling_is_active()
// (value shape: finite and strictly positive) -- the second check exists
// because safety_guards.c's own min() clamp only defends S1 against a
// firing_max_c that is too HIGH; a finite but negative value sails straight
// through that clamp and would silently tighten S1 into nuisance trips. A
// value that fails either check is treated as "no firing / no ceiling known"
// (s_firing_ceiling_have = false), never half-accepted -- see
// link_frame_ceiling_is_active()'s own header comment.
//
// s_firing_ceiling_have/_c only record what the ESP most recently claimed;
// they say nothing about whether the link is currently up or the context
// this ceiling arrived alongside is still fresh. That gating -- link loss
// must revert S1 to abs_max_temp_c alone, not keep honouring a stale ceiling
// from a firing that may no longer be running -- is safety_core.c's job
// (link_frame_firing_ceiling_should_apply(), applied every tick in
// safety_core_build_input() against its own already-computed context_valid),
// the same "link_task publishes the raw fact, safety_core decides staleness"
// split link_task_get_context_snapshot() already established.
static void link_task_handle_set_firing_ceiling(const kilnlink_frame_t *frame)
{
    kilnlink_ceiling_t msg;
    kilnlink_ceiling_status_t dstatus =
        kilnlink_ceiling_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_CEILING_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    if (link_frame_ceiling_is_active(msg.firing_max_c)) {
        s_firing_ceiling_have = true;
        s_firing_ceiling_c = msg.firing_max_c;
    } else {
        // 0/NaN ("no firing", the wire's own convention) or a value this
        // build additionally rejects (negative, +/-Infinity) -- both collapse
        // to "no active ceiling", never a half-accepted number.
        s_firing_ceiling_have = false;
        s_firing_ceiling_c = 0.0f;
    }
}

// SAFETY_CMD_SET_CLOCK (0x0C), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// "The Pico has no RTC... Purely diagnostic -- no guard may ever read this
// clock, or a bad time from the ESP becomes a safety input." This function
// only stores the value (after a plausibility check) for a future log/diag
// consumer; nothing in this codebase reads it back yet, by design (Frame D's
// TRIP_EVENT wire layout is fixed and carries uptime_ms only, not a wall-clock
// field -- adding one would be a wire-format change out of scope here, not a
// consumer-wiring one). No guard, no S-numbered check, and no other safety
// decision anywhere in this codebase is gated on s_wall_clock_have/_epoch_ms
// -- grep-confirmed before writing this comment, and worth stating plainly
// per the protocol doc's own warning.
//
// link_frame_clock_epoch_is_plausible() rejects an epoch far outside a
// generous [2020, 2100) window -- implausible input is dropped rather than
// stored and later confusing a trip-log correlation, but rejection here has
// no safety consequence either way (this field decides nothing), unlike the
// firing-ceiling bounds check above.
static void link_task_handle_set_clock(const kilnlink_frame_t *frame)
{
    kilnlink_set_clock_t msg;
    kilnlink_set_clock_status_t dstatus =
        kilnlink_set_clock_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_SET_CLOCK_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    if (!link_frame_clock_epoch_is_plausible(msg.epoch_ms)) {
        log_task_log(LOG_LEVEL_WARN, "set_clock", "ignored, implausible epoch");
        return;
    }

    s_wall_clock_have = true;
    s_wall_clock_epoch_ms = msg.epoch_ms;
}

// SAFETY_CMD_SET_LOG_LEVEL (0x1B), CommonFW/docs/LINK_PROTOCOL.md section 4 /
// docs/COMMISSIONING.md section 2's table -- "minted in the same pass
// because it was the last unallocated id blocking log_task_set_level() from
// being reachable over the wire." Unrelated to commissioning: this is
// runtime log verbosity, not a staged config field, so unlike SET_PARAM it
// takes effect immediately and is never persisted to flash at all.
static void link_task_handle_set_log_level(const kilnlink_frame_t *frame)
{
    kilnlink_set_log_level_t msg;
    kilnlink_set_log_level_status_t dstatus =
        kilnlink_set_log_level_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_SET_LOG_LEVEL_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    if (msg.level > LOG_LEVEL_VERBOSE) {
        log_task_log(LOG_LEVEL_WARN, "set_log_level", "refused, level out of range");
        return;
    }

    log_task_set_level(msg.level);
    log_task_log(LOG_LEVEL_INFO, "set_log_level", "accepted");
}

// SAFETY_CMD_SET_PARAM (0x1C), docs/COMMISSIONING.md section 2 -- stages one
// (param_id, value) pair into the in-RAM record, per config_params.c's id
// table. Nothing here reaches flash: config_params_set() only mutates
// s_staged_config, and config_store_write() is called ONLY from
// COMMIT_CONFIG's handler below, after that whole staged record passes
// cross-field validation. An unknown id or a type that does not match the
// field's own wire type is refused individually (config_params_set()
// returns false, s_staged_config left untouched) -- COMMISSIONING.md section
// 2: "unknown ids are refused individually... rather than the whole
// transfer failing," and the same treatment extends to a wrong type tag for
// a real id, which is exactly as untrustworthy.
static void link_task_handle_set_param(const kilnlink_frame_t *frame)
{
    kilnlink_set_param_t msg;
    kilnlink_set_param_status_t dstatus = kilnlink_set_param_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_SET_PARAM_OK) {
        // Malformed/wrong-length/bad-type -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    link_task_ensure_staged_config();

    if (!config_params_set(&s_staged_config, msg.param_id, msg.type, msg.value)) {
        log_task_log(LOG_LEVEL_WARN, "set_param", "refused, unknown param_id or type mismatch");
        return;
    }
    log_task_log(LOG_LEVEL_INFO, "set_param", "staged");
}

// SAFETY_CMD_COMMIT_CONFIG (0x1D), docs/COMMISSIONING.md section 2 -- the
// one wire command that can actually reach flash for the commissioning
// surface. Three things happen, IN THIS ORDER, and a failure at any step
// writes NOTHING:
//   1. config_params_validate() checks the staged record's cross-field
//      rules (tc_placement_mode vs tc_source, CONFIG_REFERENCE.md section
//      1) as a whole -- refusal here names the offending field/rule and
//      never touches config_store_write() at all.
//   2. config_params_finalize_ct_channel_map() derives the ct_channel_map
//      group bit from whichever of the three per-channel bits are actually
//      present -- see config_store.h's own comment on why this is derived,
//      not asserted by an individual SET_PARAM.
//   3. calibration_missing is recomputed from config_params_all_required_set():
//      cleared ONLY if every no-safe-default field is now set; left/forced
//      true otherwise (docs/COMMISSIONING.md section 4.1: "a bench preset
//      must not look commissioned" -- a partial commit is exactly that
//      case, and must not silently clear the flag).
// The ARMED refusal is NOT checked here -- it lives inside
// config_store_write() itself (config_store_decide_write(), config_store.h's
// own header comment), so a future second call site into config_store_write()
// cannot forget it. On refusal (ARMED, or a flash failure), s_staged_config
// is left completely UNCHANGED, so a retry (or another SET_PARAM first)
// starts from exactly what was staged, never from a half-written record.
// SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20), docs/COMMISSIONING.md sec 2/3.1
// -- ROADMAP.md "no wire codec carries a per-field COMMIT_CONFIG rejection
// reason back to the ESP" loose end. Sent ONLY from link_task_handle_
// commit_config() below, ONLY on a refusal (never on acceptance -- an
// accepted commit is already visible via FW_VERSION's bumped config_crc).
// Same fire-and-forget broadcast shape as every other reply this file sends
// off its own initiative (STATUS/DIAG/TRIP_EVENT), not a protocol-level ACK
// payload: kilnlink_commit_config.h's own codec carries no fields for this,
// by design (COMMISSIONING.md sec 2: validation happens after the frame is
// already accepted at the wire layer), so the rejection has to be its own
// frame.
static void link_task_send_commit_config_rejected(uint16_t param_id,
                                                    kilnlink_commit_config_reject_reason_t reason)
{
    kilnlink_commit_config_rejected_t msg = { .param_id = param_id, .reason = (uint8_t)reason };
    uint8_t payload[KILNLINK_COMMIT_CONFIG_REJECTED_LEN];
    kilnlink_commit_config_rejected_status_t status;
    size_t len = kilnlink_commit_config_rejected_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // can't happen for a fixed sizeof(payload) == KILNLINK_COMMIT_CONFIG_REJECTED_LEN buffer
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

static void link_task_handle_commit_config(const kilnlink_frame_t *frame)
{
    kilnlink_commit_config_t msg;
    kilnlink_commit_config_status_t dstatus =
        kilnlink_commit_config_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_COMMIT_CONFIG_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }
    (void)msg; // no fields

    link_task_ensure_staged_config();

    const char *field = NULL;
    const char *rule = NULL;
    config_params_reject_reason_t validate_reason = CONFIG_PARAMS_REJECT_NONE;
    if (!config_params_validate_ex(&s_staged_config, &field, &rule, &validate_reason)) {
        log_task_log(LOG_LEVEL_WARN, "commit_config", rule ? rule : "refused, validation failed");
        // COMMISSIONING.md sec 3.1's "names the offending field and the rule
        // it broke" -- validate_reason maps directly onto the wire enum
        // (both are "range" vs "contradiction", nothing else can come out
        // of config_params_validate_ex() here); CONFIG_PARAMS_REJECT_NONE
        // (the NULL-rec case, which link_task.c never actually triggers,
        // since s_staged_config is always a real object) falls through to
        // UNKNOWN rather than silently mislabelling as RANGE.
        kilnlink_commit_config_reject_reason_t wire_reason =
            (validate_reason == CONFIG_PARAMS_REJECT_CONTRADICTION) ? KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION
            : (validate_reason == CONFIG_PARAMS_REJECT_RANGE)       ? KILNLINK_COMMIT_CONFIG_REJECT_RANGE
                                                                     : KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN;
        link_task_send_commit_config_rejected(config_params_id_for_field_name(field), wire_reason);
        return; // writes NOTHING -- s_staged_config is untouched by validate_ex()
    }

    config_store_record_t to_write = s_staged_config;
    config_params_finalize_ct_channel_map(&to_write);
    to_write.calibration_missing = !config_params_all_required_set(&to_write);

    const char *reason = NULL;
    bool written = config_store_write(&to_write, &reason);
    if (written) {
        s_staged_config = to_write; // becomes the new baseline for the next SET_PARAM
        log_task_log(LOG_LEVEL_INFO, "commit_config", "accepted");
    } else {
        log_task_log(LOG_LEVEL_WARN, "commit_config", reason ? reason : "refused");
        // Not field-specific -- config_store_write()'s own refusal is either
        // "relay is ARMED" (config_store_decide_write()) or a flash failure
        // (config_store_flash_rc_reason()), never a single staged field's
        // fault, so this always carries the NO_PARAM_ID sentinel. Match on
        // the ARMED string specifically (config_store_write_decision_
        // reason()'s own literal for CONFIG_STORE_WRITE_REFUSED_ARMED) --
        // anything else here is a storage-layer failure.
        kilnlink_commit_config_reject_reason_t wire_reason =
            (reason && strcmp(reason, "refused: relay is ARMED, config writes are refused while ARMED") == 0)
                ? KILNLINK_COMMIT_CONFIG_REJECT_ARMED
                : KILNLINK_COMMIT_CONFIG_REJECT_STORAGE;
        link_task_send_commit_config_rejected(CONFIG_PARAMS_NO_PARAM_ID, wire_reason);
    }
}

// SAFETY_CMD_PARAM (0x1E) reply -- sent in answer to SAFETY_CMD_GET_PARAM
// (link_task_handle_get_param() below), same shared-id/reply-on-request
// shape as link_task_send_fw_version()/link_task_send_ct_cal(). Reports the
// currently COMMITTED record (config_store_get_full_record()), never the
// in-progress staged one -- CONFIG_REFERENCE.md section 7's "which
// thresholds is the safety processor actually enforcing" must be answerable
// from what is enforced, not from an uncommitted edit in flight.
static void link_task_send_param(uint16_t param_id)
{
    config_store_record_t rec;
    config_store_get_full_record(&rec);

    kilnlink_param_t reply;
    reply.param_id = param_id;
    uint8_t type = 0;
    kilnlink_param_value_t value;
    memset(&value, 0, sizeof(value));
    bool found = config_params_get(&rec, param_id, &type, &value);
    reply.found = found ? 1u : 0u;
    reply.type = found ? type : 0u; // ignored by the reader when found == 0 (kilnlink_param.h)
    reply.value = value;

    uint8_t payload[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t len = kilnlink_param_encode(&reply, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // can't happen for a fixed sizeof(payload) == KILNLINK_PARAM_MAX_LEN buffer
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

static void link_task_handle_get_param(const kilnlink_frame_t *frame)
{
    kilnlink_get_param_t msg;
    kilnlink_get_param_status_t dstatus = kilnlink_get_param_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_GET_PARAM_OK) {
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    link_task_send_param(msg.param_id);
}

// SAFETY_CMD_CONFIG_PAGE (0x1F) reply -- sent in answer to SAFETY_CMD_
// GET_CONFIG_PAGE (link_task_handle_get_config_page() below). Reports the
// currently COMMITTED record, same reasoning as link_task_send_param()
// above. kilnlink_config_page_pack() is stateless/greedy per call
// (kilnlink_config_page.h's own header comment: no server-side cursor to go
// stale across a lost/repeated request, LINK_PROTOCOL.md section 2), so this
// re-derives where `page_index` must resume EVERY call by replaying pack()
// over pages [0, page_index) against the full id list and discarding the
// bytes, keeping only how many entries each replayed page consumed.
static void link_task_send_config_page(uint8_t page_index)
{
    config_store_record_t rec;
    config_store_get_full_record(&rec);

    // 64 is config_params.c's own compile-time upper bound on its id table
    // (config_params_table_fits_64) -- comfortably above its real size
    // today, checked there so this array can never silently truncate.
    kilnlink_config_page_entry_t all[64];
    size_t all_cap = sizeof(all) / sizeof(all[0]);
    size_t total = config_params_count();
    if (total > all_cap) {
        total = all_cap; // defensive only -- see config_params.c's own bound check
    }
    for (size_t i = 0; i < total; i++) {
        uint16_t id = 0;
        uint8_t type = 0;
        config_params_id_at(i, &id, &type);
        uint8_t got_type = 0;
        kilnlink_param_value_t value;
        memset(&value, 0, sizeof(value));
        config_params_get(&rec, id, &got_type, &value); // always succeeds -- id came from this module's own table
        all[i].param_id = id;
        all[i].type = got_type;
        all[i].value = value;
    }

    uint8_t scratch[KILNLINK_FRAME_MAX_PAYLOAD];
    size_t offset = 0;
    for (uint8_t p = 0; p < page_index && offset < total; p++) {
        size_t packed = 0;
        kilnlink_config_page_status_t st;
        size_t n = kilnlink_config_page_pack(p, &all[offset], total - offset, scratch, sizeof(scratch),
                                              &packed, &st);
        if (n == 0 || packed == 0) {
            // Replay ran out of entries before reaching page_index -- the
            // ESP asked for a page past the end. Fall through to pack an
            // empty (entry_count 0, more 0) final page below, rather than
            // looping or guessing at a nonexistent page's contents.
            offset = total;
            break;
        }
        offset += packed;
    }

    kilnlink_config_page_status_t status;
    size_t packed_now = 0;
    uint8_t payload[KILNLINK_FRAME_MAX_PAYLOAD];
    size_t len = kilnlink_config_page_pack(page_index, &all[offset], total - offset, payload,
                                            sizeof(payload), &packed_now, &status);
    if (len == 0) {
        s_diag_page_last_outcome = DIAG_PAGE_OUTCOME_PACK_FAILED; // 2026-08-23 diagnostic
        return; // can't happen -- payload is sized to the wire's own payload cap
    }
    link_task_send_broadcast(payload, (uint8_t)len);
    s_diag_page_last_outcome = DIAG_PAGE_OUTCOME_REPLIED; // 2026-08-23 diagnostic
}

static void link_task_handle_get_config_page(const kilnlink_frame_t *frame)
{
    s_diag_page_last_outcome = DIAG_PAGE_OUTCOME_HANDLER_ENTERED; // 2026-08-23 diagnostic
    kilnlink_get_config_page_t msg;
    kilnlink_get_config_page_status_t dstatus =
        kilnlink_get_config_page_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_GET_CONFIG_PAGE_OK) {
        s_diag_page_last_outcome = DIAG_PAGE_OUTCOME_DECODE_REJECTED; // 2026-08-23 diagnostic
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    link_task_send_config_page(msg.page_index);
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

    // S6b's link_up (snapshots.h's link_task_link_up() doc comment): a
    // frame that decodes cleanly proves the ESP is alive and transmitting,
    // regardless of whether this build goes on to act on it -- recorded
    // before the BROADCAST-only filter just below on purpose.
    s_last_valid_frame_tick = xTaskGetTickCount();
    s_valid_frame_seen = true;

    if (frame.msg_type != KILNLINK_MSG_BROADCAST || frame.length == 0) {
        return; // the Pico never participates in the ACK'd DATA/ACK/NACK transport
    }

    s_diag_dispatch_accepted_count++; // 2026-08-23 diagnostic -- see statics block above

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
    case LINK_FRAME_REQUEST_ENABLE_CMD:
        link_task_handle_request_enable(&frame);
        break;
    case LINK_FRAME_CLEAR_TRIP_CMD:
        link_task_handle_clear_trip(&frame);
        break;
    case LINK_FRAME_SET_CONFIG_CMD:
        link_task_handle_set_config(&frame);
        break;
    case LINK_FRAME_ROLLBACK_CMD:
        link_task_handle_rollback(&frame);
        break;
    case LINK_FRAME_ANNOUNCE_REBOOT_CMD:
        link_task_handle_announce_reboot(&frame);
        break;
    case LINK_FRAME_SET_CT_CAL_CMD:
        link_task_handle_set_ct_cal(&frame);
        break;
    case LINK_FRAME_SET_FIRING_CEILING_CMD:
        link_task_handle_set_firing_ceiling(&frame);
        break;
    case LINK_FRAME_SET_CLOCK_CMD:
        link_task_handle_set_clock(&frame);
        break;
    case LINK_FRAME_GET_CT_CAL_CMD:
        // Same id as the reply (SAFETY_CMD_CT_CAL), distinguished by
        // direction and length: the ESP's request is exactly 1 byte, no
        // arguments -- same convention as LINK_FRAME_FW_VERSION_CMD above.
        if (frame.length == 1) {
            link_task_handle_get_ct_cal(&frame);
        }
        break;
    case KILNLINK_SET_LOG_LEVEL_CMD:
        link_task_handle_set_log_level(&frame);
        break;
    case KILNLINK_SET_PARAM_CMD:
        link_task_handle_set_param(&frame);
        break;
    case KILNLINK_COMMIT_CONFIG_CMD:
        link_task_handle_commit_config(&frame);
        break;
    case KILNLINK_GET_PARAM_CMD:
        // Same id as the reply (SAFETY_CMD_PARAM, KILNLINK_PARAM_CMD --
        // both 0x1E), distinguished by direction and length, same
        // convention as LINK_FRAME_GET_CT_CAL_CMD/GET_FW_VERSION above: the
        // ESP's request is exactly KILNLINK_GET_PARAM_LEN (3) bytes.
        if (frame.length == KILNLINK_GET_PARAM_LEN) {
            link_task_handle_get_param(&frame);
        }
        break;
    case KILNLINK_GET_CONFIG_PAGE_CMD:
        // Same id as the reply (SAFETY_CMD_CONFIG_PAGE, KILNLINK_CONFIG_PAGE_CMD
        // -- both 0x1F), same shared-id convention as GET_PARAM/PARAM above.
        s_diag_get_config_page_seen_count++; // 2026-08-23 diagnostic -- counts regardless of length match
        if (frame.length == KILNLINK_GET_CONFIG_PAGE_LEN) {
            s_diag_get_config_page_handled_count++; // 2026-08-23 diagnostic
            link_task_handle_get_config_page(&frame);
        }
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
        // Everything else this build has no dispatch case for is genuinely
        // out of scope -- an unrecognised type is silently discarded,
        // matching LINK_PROTOCOL.md's own additive-compatibility principle:
        // "a peer that has never heard of it discards it."
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

// --- Frame D (TRIP_EVENT) polling -------------------------------------------

// Called once per link_task_fn() loop iteration (~100ms, LINK_TASK_POLL_MS).
// Two independent jobs:
//   1. Notice a NEW trip (safety_core_get_trip_event()'s trip_seq advanced
//      past what this task has already started a burst for) and, if so,
//      fill s_pending_trip ONCE from safety_core's own captured-at-the-
//      instant values plus this task's own best-effort current/context
//      pulls, then arm a LINK_TRIP_REPEAT_COUNT-copy burst.
//   2. Send the next copy of an already-armed burst once
//      LINK_TRIP_REPEAT_PERIOD_MS has elapsed since the last one.
// Deliberately not folded into the periodic-TX block in link_task_fn() below
// (which all key off "has this fixed period elapsed") -- a trip event is
// edge-triggered, not periodic, and mixing the two shapes into one
// last_*_tx/period pair would make either harder to read than two smaller
// pieces.
static void link_task_poll_trip_event(TickType_t now)
{
    uint8_t trip_seq = 0;
    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    uint32_t trip_uptime_ms = 0;
    float trip_tc_c = NAN;
    float trip_threshold = NAN;
    bool have_trip = safety_core_get_trip_event(&trip_seq, &trip_reason, &trip_uptime_ms,
                                                 &trip_tc_c, &trip_threshold);

    if (have_trip && trip_seq != s_trip_last_seq_seen) {
        s_trip_last_seq_seen = trip_seq;

        // "at the instant of the trip" for uptime_ms/safety_tc_c/
        // deciding_threshold, straight from safety_core's own capture
        // (safety_core_get_trip_event()'s doc comment). current_a[]/
        // relay_recent_mask/context_age_100ms are NOT captured by
        // safety_core at all (safety_guard_input_t carries no raw current or
        // ESP-context data in this build -- see that struct's field
        // comments) -- the best this task can honestly do is pull them here,
        // "at detection time" rather than the literal trip tick, which is at
        // most one LINK_TASK_POLL_MS (~100ms) late. Documented here rather
        // than silently presented as exact.
        current_snapshot_t cur;
        current_task_get_snapshot(&cur);

        context_snapshot_t ctx;
        bool have_ctx = link_task_get_context_snapshot(&ctx);

        s_pending_trip.trip_seq = trip_seq;
        s_pending_trip.trip_reason = (uint8_t)trip_reason;
        s_pending_trip.uptime_ms = trip_uptime_ms;
        s_pending_trip.safety_tc_c = trip_tc_c;
        s_pending_trip.deciding_threshold = trip_threshold;
        s_pending_trip.current_a[0] = cur.amps[0];
        s_pending_trip.current_a[1] = cur.amps[1];
        s_pending_trip.current_a[2] = cur.amps[2];
        s_pending_trip.relay_recent_mask = have_ctx ? ctx.relay_recent_mask : 0u;
        s_pending_trip.context_age_100ms = link_task_context_age_100ms();

        s_trip_repeats_pending = LINK_TRIP_REPEAT_COUNT;
        // Force the first copy out this same iteration rather than waiting a
        // full LINK_TRIP_REPEAT_PERIOD_MS -- "pushed immediately on trip" is
        // the frame's whole point (LINK_PROTOCOL.md sec 6).
        s_last_trip_tx_tick = now - pdMS_TO_TICKS(LINK_TRIP_REPEAT_PERIOD_MS);
    }

    if (s_trip_repeats_pending > 0 &&
        (now - s_last_trip_tx_tick) >= pdMS_TO_TICKS(LINK_TRIP_REPEAT_PERIOD_MS)) {
        link_task_send_trip_event(&s_pending_trip);
        s_trip_repeats_pending--;
        s_last_trip_tx_tick = now;
    }
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
        link_task_poll_trip_event(now);

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

    s_trip_last_seq_seen = 0;
    s_trip_repeats_pending = 0;
    s_last_trip_tx_tick = 0;

    s_last_valid_frame_tick = 0;
    s_valid_frame_seen = false;
    s_relay_on_since_tick = 0;
    s_relay_on_continuous = false;

    s_firing_ceiling_have = false;
    s_firing_ceiling_c = 0.0f;
    s_wall_clock_have = false;
    s_wall_clock_epoch_ms = 0;

    s_staged_config_init = false;

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

bool link_task_link_up(void)
{
    if (!s_valid_frame_seen) {
        return false;
    }
    TickType_t age_ticks = xTaskGetTickCount() - s_last_valid_frame_tick;
    uint32_t age_ms = (uint32_t)age_ticks * portTICK_PERIOD_MS;
    return age_ms < LINK_UP_RECENCY_MS;
}

uint32_t link_task_get_relay_on_continuous_ms(void)
{
    if (!s_relay_on_continuous) {
        return 0u;
    }
    TickType_t elapsed_ticks = xTaskGetTickCount() - s_relay_on_since_tick;
    return (uint32_t)elapsed_ticks * portTICK_PERIOD_MS;
}

bool link_task_get_firing_ceiling(float *out_firing_max_c)
{
    if (out_firing_max_c) {
        *out_firing_max_c = 0.0f;
    }
    if (!s_firing_ceiling_have) {
        return false;
    }
    if (out_firing_max_c) {
        *out_firing_max_c = s_firing_ceiling_c;
    }
    return true;
}

bool link_task_get_wall_clock_epoch_ms(uint64_t *out_epoch_ms)
{
    if (out_epoch_ms) {
        *out_epoch_ms = 0;
    }
    if (!s_wall_clock_have) {
        return false;
    }
    if (out_epoch_ms) {
        *out_epoch_ms = s_wall_clock_epoch_ms;
    }
    return true;
}
