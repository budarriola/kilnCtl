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
// construction implementations of the same layout (26 bytes then; 30 since
// KILNLINK_PROTOCOL_VERSION 16 appended log_frames_dropped), and this removes
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
#include <stdio.h> // snprintf -- clear-trip received-count log line, see link_task_handle_clear_trip()
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "pico/time.h"

#include "task_priorities.h"
#include "watchdog_task.h"

#include "hal_uart_pico_internal.h"  // HAL Phase 1a: was uart_owner.h; moved+renamed to firmware/hwAbstraction/pico/uart/hal_uart_pico_internal.h
#include "link_frame.h"

#include "boot_reason.h"
#include "watchdog_overdue_diag.h" // watchdog_fatal_diag_get_cached() -- 2026-09-09, kilnlink_diag.h boot_reason bits 3-5
#include "config_params.h" // param_id <-> config_store_record_t field mapping, see SET_PARAM/GET_PARAM/COMMIT_CONFIG/GET_CONFIG_PAGE handlers below
#include "config_store.h" // SAFETY_CMD_SET_CONFIG, see link_task_handle_set_config()
#include "link_diag_flags.h" // pure Frame B `flags` assembly, see link_task_send_diag()
#include "clear_trip_diag.h" // clear_trip_diag_get_cached() -- Frame B flags bit3, see link_task_send_diag()
#include "current_task.h"
#include "discrete_task.h"
#include "log_task.h" // CLEAR_TRIP/SET_CONFIG outcome logging, see link_task_handle_clear_trip()/_set_config()
#include "max31856.h" // MAX31856_TC_TYPE_* range check, see link_task_handle_set_config()
#include "reboot_announce.h" // SAFETY_CMD_ANNOUNCE_REBOOT (0x18), see link_task_handle_announce_reboot()
#include "safety_core.h"
#include "link_task_announce_eval.h" // pure ANNOUNCE_VERSION -> degraded verdict, see link_task_handle_announce_version()
#include "link_task_commit_reject.h" // pure write-decision -> wire-reason mapping, see link_task_handle_commit_config()
#include "link_task_tc_type_gate.h"
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
// The build-identity record compiled into this image. link_task_send_fw_version()
// packs the commit/dirty fields it reports on the wire straight out of this
// record so "what the Pico says it is" and "what the image says it is" cannot
// drift apart -- see the comment at that call site, and
// kilnlink/saftyfw_image_identity.h for why the ESP needs them identical.
#include "saftyfw_image_identity_record.h"

#include "kilnlink/kilnlink_ceiling.h" // SAFETY_CMD_SET_FIRING_CEILING, see link_task_handle_set_firing_ceiling()
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_commit_config.h" // SAFETY_CMD_COMMIT_CONFIG (0x1D), see link_task_handle_commit_config()
#include "kilnlink/kilnlink_apply_config_volatile.h" // SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D), KILN_PROFILES_PLAN.md item 15 -- see link_task_handle_apply_config_volatile()
#include "tc_type_reapply_policy.h" // tc_type_reapply_policy_should_reapply(), see all three config-write handlers below
#include "kilnlink/kilnlink_commit_config_rejected.h" // SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20), see link_task_send_commit_config_rejected()
#include "kilnlink/kilnlink_config_page.h" // SAFETY_CMD_CONFIG_PAGE reply, see link_task_send_config_page()
#include "kilnlink/kilnlink_ct_cal.h" // SAFETY_CMD_CT_CAL reply, see link_task_send_ct_cal()
#include "kilnlink/kilnlink_diag.h"
#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_ct_auto_zero_status.h" // SAFETY_CMD_CT_AUTO_ZERO_STATUS (0x28), see link_task_send_ct_auto_zero_status()
#include "kilnlink/kilnlink_ct_auto_zero_begin.h" // SAFETY_CMD_CT_AUTO_ZERO_BEGIN (0x26), see link_task_handle_ct_auto_zero_begin()
#include "kilnlink/kilnlink_get_ct_auto_zero.h" // SAFETY_CMD_GET_CT_AUTO_ZERO (0x27), see link_task_handle_get_ct_auto_zero()
#include "kilnlink/kilnlink_get_config_page.h" // SAFETY_CMD_GET_CONFIG_PAGE (0x24), see link_task_handle_get_config_page()
#include "kilnlink/kilnlink_get_ct_cal.h" // SAFETY_CMD_GET_CT_CAL (0x22), see link_task_handle_get_ct_cal()
#include "kilnlink/kilnlink_get_stack_margin.h" // SAFETY_CMD_GET_STACK_MARGIN (0x2B), see link_task_handle_get_stack_margin()
#include "kilnlink/kilnlink_stack_margin.h" // SAFETY_CMD_STACK_MARGIN (0x2C) reply, see link_task_send_stack_margin()
#include "stack_margin_poller.h"
#include "kilnlink/kilnlink_get_param.h" // SAFETY_CMD_GET_PARAM (0x23), see link_task_handle_get_param()
#include "kilnlink/kilnlink_inject_tc.h" // SAFETY_CMD_INJECT_TC (0x21), see link_task_handle_inject_tc()
#include "kilnlink/kilnlink_param.h" // SAFETY_CMD_PARAM (0x1E reply), see link_task_send_param()
#include "kilnlink/kilnlink_power.h"
#include "kilnlink/kilnlink_reboot.h" // SAFETY_CMD_REBOOT, see link_task_handle_reboot()
#include "kilnlink/kilnlink_reboot_result.h" // SAFETY_CMD_REBOOT_RESULT, see link_task_handle_reboot()
#include "kilnlink/kilnlink_rollback.h" // SAFETY_CMD_ROLLBACK, see link_task_handle_rollback()
#include "kilnlink/kilnlink_rollback_result.h" // SAFETY_CMD_ROLLBACK_RESULT, see link_task_send_rollback_result()
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
// 2026-09-10 (owner decision "raise both stacks"): check_saftyfw_task_stack_
// budgets.py's regsp-margin check (2x REGSP_MARGIN_FACTOR against an
// unresolved register-computed frame adjust it cannot size exactly) FAILED
// at *6 -- measured 4736 B lower bound needs declared >= 9472 B for that
// check to clear, and *6 (6144 B) was only 77% of that. Bumped to *10
// (10240 B) rather than the bare minimum *10 threshold-clearing value, same
// "RAM is cheap, a Pico stack overflow corrupts a neighbour and is hours to
// diagnose" reasoning as the other five stack overflows this project has
// hit -- see check_saftyfw_task_stack_budgets.py's CEILING_BYTES comment
// for the incident history. configTOTAL_HEAP_SIZE (FreeRTOSConfig.h) raised
// in the same commit to keep non-stack heap headroom sane after this and
// update_task's stacks both grew.
#define LINK_TASK_STACK_WORDS      (configMINIMAL_STACK_SIZE * 10)
// Bounded wait, not a blocking read: this task also owns the 500 ms TX
// cadence and must check in with watchdog_task, so it polls uart_owner's RX
// ring on a short period rather than blocking on a queue receive.
//
// LOWERED from 100 to 10 (2026-08-28, live-hardware commissioning defect).
// This period gates how long a request byte sitting in uart_owner's RX ring
// can wait before link_task_fn() even LOOKS at it -- uart_owner_h's own
// header comment says so explicitly ("link_task drains it by polling...
// rather than blocking on a queue receive"). KilnFW's SAFETY_LINK_REPLY_
// TIMEOUT_MS (safety_link.h) budgets for exactly this behaviour with "a
// fixed 100 ms for the Pico's own task latency" -- a number that assumed
// this constant's OLD value and left zero slack for anything else (wire
// time for the actual small request/reply frames, FreeRTOS scheduling
// jitter, the ESP's own dispatch). At 100/100 the two constants raced to a
// photo finish that the Pico's own polling almost always lost: measured live
// with commit ddbd024 flashed, GET_CONFIG_PAGE page 1 timed out on
// essentially every fetch attempt (safety_get_link_stats(): sent 60,
// timeouts 54) while page 0 only ever arrived via the ESP's own stash-
// adoption of a reply that missed its own window -- both symptoms of a
// reply that is consistently, not randomly, late by an amount close to this
// constant's old value. Every other producer/consumer this task owns (the
// 500 ms status cadence, the trip-event burst, watchdog_task_checkin) stays
// correct at the faster period -- they all key off elapsed-tick comparisons
// against periods far longer than 10 ms, not off this constant's absolute
// value. Do not raise this back toward 100 without re-deriving KilnFW's
// SAFETY_LINK_REPLY_TIMEOUT_MS margin at the same time -- the two are
// coupled even though they live in separate firmware trees.
#define LINK_TASK_POLL_MS          10
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
// Periodic in-RAM config re-CRC (config_check_period_s, CONFIG_REFERENCE.md
// section 6). The actual period is operator-configured seconds, not a
// compile-time cadence like the frames above -- this is just how often
// link_task looks at the clock to decide whether that many seconds have
// elapsed. 1s is coarse enough that reading config_check_period_s here is
// cheap relative to LINK_TASK_POLL_MS's own per-iteration cost, and fine
// enough that a period of "10s" (the compiled default) is not itself
// perceptibly delayed by this polling granularity.
#define LINK_CONFIG_CHECK_POLL_MS  1000
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
// log lines (firmware/KilnFW/App/drivers/common/uart_task_ids.h:58,
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

// The peer ESP's own protocol_version, as last announced via ANNOUNCE_VERSION
// -- cached alongside s_degraded_no_context (same writer, same single-word/
// volatile-read pattern), but a raw number rather than a compatibility bool,
// so link_task_send_status() can ask a narrower, additive-feature-specific
// question ("has this peer proven it understands V2?") independent of
// link_frame_versions_compatible()'s own min_compatible-range verdict. 0
// (no real protocol_version this codebase has ever shipped) means "no
// ANNOUNCE_VERSION received yet this boot" -- the same safe-default-to-old
// state as a peer that predates ANNOUNCE_VERSION entirely, see
// link_frame_pack_status()'s own doc comment (link_frame.h) for why "unknown"
// and "known old" must behave identically here.
static volatile uint16_t s_peer_protocol_version = 0;

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

// 2026-08-24: link_task_send_broadcast_to()'s raw[]/stuffed[] used to be
// locals on THIS function's own stack -- KILNLINK_FRAME_RAW_MAX (263B) +
// KILNLINK_FRAME_STUFFED_MAX (528B), 791 bytes in one frame. That cost was
// invisible to whichever task called in: link_task itself (fine, LINK_TASK_
// STACK_WORDS is sized for far more), log_task (NOT fine -- this was the
// exact mechanism behind the 2026-08-23 log_task stack overflow that made
// CLEAR_TRIP reboot the board whenever it took the refusal branch and tried
// to log the outcome), and update_task (fine today, only by luck of a
// generous stack nobody sized against this function's real cost).
//
// Moved to static storage instead: every caller's stack requirement drops
// by 791 bytes permanently, and no future caller can add itself to this
// function's call graph without re-deriving a budget nobody will remember
// to check. Cost: 791 bytes of static RAM (trivial -- this is a 264KB
// RP2040) and one new bounded mutex.
//
// All three current callers (link_task_send_broadcast(), log_task's
// link_task_send_log(), update_task's link_task_send_safety()) are pinned
// to SAFTYFW_CORE_LINK_PATH (core 0) -- confirmed by grep -- so there is no
// SMP race on these buffers. But FreeRTOS's preemptive scheduler on that one
// core still means a higher-priority caller (link_task itself, SAFTYFW_PRIO_
// LINK_TASK) could preempt a lower-priority one (log_task or update_task)
// mid-use without a real lock -- same-core reasoning alone only rules out
// hardware races, not preemption. Hence the mutex, not a bare "one core,
// no lock needed" argument.
//
// 50ms bound matches this file's own s_context_lock convention (and
// thermo_task.c's s_snapshot_lock) -- same "never wait longer than a
// fraction of the tightest caller's watchdog deadline" reasoning. The
// shortest deadline among the three callers is link_task's/update_task's
// 300ms (WATCHDOG_CHECKIN_LINK_TASK/_UPDATE_TASK, watchdog_gate.c); 50ms
// leaves ample margin even in the worst case of two callers colliding.
static SemaphoreHandle_t s_broadcast_buf_lock = NULL;
static uint8_t s_broadcast_raw[KILNLINK_FRAME_RAW_MAX];
static uint8_t s_broadcast_stuffed[KILNLINK_FRAME_STUFFED_MAX];

// Single-writer bookkeeping: touched only from link_task_fn / functions it
// calls (all running on this task), read back only by link_task_send_diag()
// (also this task) -- no lock needed, same reasoning as s_degraded_no_context
// above but for a handful of scalars instead of one bool.
static uint32_t s_context_frames_ok = 0;
static uint32_t s_context_frames_bad = 0;

// CLEAR_TRIP frames this task has decoded successfully, single-writer/no-lock
// same as the pair above -- added alongside safety_core.c's own
// s_clear_trip_requested/_processed counters (safety_core_get_clear_trip_
// stats()) so a hardware run can tell "did the frame even reach the Pico"
// apart from "did safety_core's queue receive it" apart from "did
// safety_core resolve it" -- see link_task_handle_clear_trip()'s own comment
// for where each count is logged.
static uint32_t s_clear_trip_rx_count = 0;
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

// Frame C (FW_VERSION) boot push burst state -- same shape as the TRIP_EVENT
// burst above, and for the same reason: LINK_PROTOCOL.md sec 6 calls the
// boot push "unsolicited once at boot", but the whole point of that push is
// telling a link that has JUST come out of reset "the safety processor
// restarted, here is its new boot_id" -- exactly the moment a fresh link is
// least likely to have both ends synced. A single frame with no ACK and no
// retry means one lost byte silently strands the ESP's peer_version_known
// (and therefore its rollback-outcome evidence) for the rest of this boot,
// per the KilnFW-side audit that found this. Mirrors ANNOUNCE_VERSION's own
// tuning (safety_link_send_announce_version_burst(), KilnFW's safety_link.c:
// 4 copies, 250ms apart) rather than inventing a new number -- same
// "loss-tolerant unsolicited burst" precedent LINK_TRIP_REPEAT_COUNT/
// _PERIOD_MS above already cites for the identical reason. Sent from inside
// link_task_fn's main poll loop (NOT via vTaskDelay before the loop starts),
// so this task keeps checking in with the watchdog (WATCHDOG_CHECKIN_
// LINK_TASK's deadline is 30ms) the whole time the burst is going out. */
#define LINK_BOOT_FW_VERSION_REPEAT_COUNT     4u
#define LINK_BOOT_FW_VERSION_REPEAT_PERIOD_MS 250u
static unsigned s_boot_fw_version_repeats_pending = 0;
static TickType_t s_last_boot_fw_version_tx_tick = 0;

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

// Same threshold, and the same "stale context is no context" reasoning, as
// safety_core.c's own CONTEXT_MAX_AGE_MS -- link_task.c cannot #include
// safety_core.c (isolation runs the other direction: safety_core.c is
// forbidden from including anything link-shaped, snapshots.h's own header
// comment), so this is a deliberate duplicate of the same constant rather
// than a shared one. Used only by link_task_heat_is_safe_for_tc_type_change()
// below (2026-09-15 Opus review G2 fix) to decide whether the ESP's own
// heat-enable/firing facts in the context frame are fresh enough to trust.
#define LINK_TASK_CONTEXT_MAX_AGE_MS 5000u

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
// for the config-page reply specifically (cmd 0x24,
// KILNLINK_GET_CONFIG_PAGE_CMD -- was 0x1F before KILNLINK_PROTOCOL_VERSION
// 7 split it off the reply's id), since the 500 ms status broadcast almost
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

// --- DIAGNOSTIC: 2026-08-23 size-window investigation ---------------------
// Coordinator's own lead: the DIAG/POWER-never-arrives boundary sits between
// 34 and 36 RAW bytes, next to the RP2040's 32-byte TX FIFO depth and
// exactly where uart_owner_send()'s priming write operates (see that
// function's own comment, uart_owner.c). uart_owner_get_last_send_remainder()
// reports how many bytes of the frame THIS call just sent were left
// dependent on the TX ISR rather than the priming write -- latched here into
// DIAG/POWER-SPECIFIC statics (not the generic s_last_tx_* above, which the
// 500ms status broadcast overwrites long before anyone can read a 2s-cadence
// DIAG/POWER sample over SWD) alongside a snapshot of
// uart_owner_get_tx_bytes_from_isr() taken at the same instant. A caller
// polling this pair a few hundred ms after a DIAG send, and finding
// s_last_diag_isr_bytes_from_isr unchanged from the value latched at send
// time despite s_last_diag_remainder > 0, has direct proof the ISR never
// drained that frame's tail. Safe to delete once the size-window question is
// settled.
static volatile uint32_t s_last_diag_remainder = 0;
static volatile uint32_t s_last_diag_stuffed_len = 0;
static volatile uint32_t s_last_diag_isr_bytes_from_isr_at_send = 0;
static volatile uint32_t s_last_power_remainder = 0;
static volatile uint32_t s_last_power_stuffed_len = 0;
static volatile uint32_t s_last_power_isr_bytes_from_isr_at_send = 0;
// --- end diagnostic statics ----------------------------------------------

// --- DIAGNOSTIC: 2026-08-23 round 6, coordinator's ring-pointer-bug
// hypothesis -- DIAG/POWER now arrive at the right cadence with valid CRCs
// but every decoded DIAG carries byte-identical, frozen content (uptime_ms
// stuck at one value for 90+ seconds on the bench) despite diag_applied
// climbing and the tick clock confirmed advancing normally. Two things
// worth separating: (1) is fresh content genuinely being BUILT and ENQUEUED
// each call (this file's job), or (2) is fresh content built and enqueued
// correctly but the RING/ISR replays a stale window instead of draining it
// (uart_owner.c's job, see its own s_tx_head/s_tx_tail getters added this
// round). s_last_diag_payload_snapshot answers (1) directly: it is the
// first 10 bytes of `payload` -- the RAW (pre-stuffing) diag payload this
// exact call built -- which per kilnlink_diag.c's own OFF_UPTIME=6 layout
// covers cmd/trip_reason/warn_mask/trip_mask/the first two uptime_ms bytes.
// If successive reads of this snapshot show the SAME bytes every time,
// content generation itself is frozen (bug is in link_task_send_diag() or
// what it reads from); if the snapshot visibly changes each read but the
// ESP still decodes stale content, the bug is downstream in uart_owner.c's
// ring, exactly the coordinator's hypothesis. s_last_diag_send_seq is a
// plain monotonic call counter (distinct from uart_owner's own
// s_tx_priming_calls) so "is link_task_send_diag() even being called at the
// right rate" is answerable from this file alone. The head/tail pair are
// uart_owner_get_tx_head()/_tail() read immediately after THIS send
// returns, for direct correlation against uart_owner.c's own free-running
// getters of the same values.
#define LINK_TASK_DIAG_SNAPSHOT_LEN 10u
static volatile uint8_t  s_last_diag_payload_snapshot[LINK_TASK_DIAG_SNAPSHOT_LEN];
static volatile uint32_t s_last_diag_send_seq = 0;
static volatile uint32_t s_last_diag_tx_head_after = 0;
static volatile uint32_t s_last_diag_tx_tail_after = 0;
// --- end diagnostic statics ----------------------------------------------

// --- DIAGNOSTIC: 2026-08-23 call-path investigation -----------------------
// Coordinator's own next step: the size-window theory is dead (a 51-byte
// send drained an 18-byte remainder through the ISR just fine), and the
// DIAG/POWER-specific remainder latches above read all-zero -- meaning
// link_task_send_broadcast_to() is never even CALLED with a DIAG or POWER
// payload. That leaves exactly three places the chain from
// link_task_fn()'s due-check to the wire could be silently exiting:
// (1) the send function is never entered at all, (2) it is entered but its
// kilnlink_*_encode() call returns len == 0 (the "can't happen" comment is
// wrong), or (3) len is fine but something between the encode and the
// link_task_send_broadcast() call still exits. These three counters, plus
// the encode outcome, pin it exactly -- one triplet per frame type, and one
// more for CONFIG_PAGE (also codec-built, also observed dark) to learn
// whether this is one bug or three. `volatile`, SWD-only, safe to delete
// once the call-path question is settled.
static volatile uint32_t s_diag_encode_len = 0;
static volatile uint8_t  s_diag_encode_status = 0;

static volatile uint32_t s_power_encode_len = 0;
static volatile uint8_t  s_power_encode_status = 0;

// 2026-08-28 diagnostic -- which page_index was actually requested and what
// page_index the OUTGOING reply's header actually carries, for the MOST
// RECENT link_task_send_config_page() call. Read over SWD (no firmware
// consumer) to settle whether the ESP genuinely never gets a page 1 REQUEST
// dispatched to this function, or gets one but the reply's own header ends
// up carrying the wrong index.
static volatile uint32_t s_last_config_page_requested_index = 0xFFu;
static volatile uint32_t s_last_config_page_reply_index = 0xFFu;
static volatile uint32_t s_config_page_encode_len = 0;
static volatile uint8_t  s_config_page_encode_status = 0;
// --- end diagnostic statics ----------------------------------------------

// --- DIAGNOSTIC: 2026-08-23 GET_CONFIG_PAGE stage1-vs-stage2 investigation -
// The statics above proved this Pico never TRANSMITS a config-page reply
// (s_last_page_* stayed all-zero over minutes of runtime), but that alone
// does not say whether the ESP's SAFETY_CMD_GET_CONFIG_PAGE request never
// arrives/dispatches (stage 1: link_task_handle_raw_frame()'s decode/
// BROADCAST-filter/switch) or whether it dispatches into
// link_task_handle_get_config_page() but that handler (or
// link_task_send_config_page() below it) declines to reply (stage 2).
// Three call-path counters that latched exactly that boundary over SWD
// (s_diag_dispatch_accepted_count, s_diag_get_config_page_seen_count,
// s_diag_get_config_page_handled_count) settled the question and were
// removed 2026-09-03 -- nothing in firmware ever read them.
//   s_diag_page_last_outcome -- one of the DIAG_PAGE_OUTCOME_* values below,
//     set at every exit point link_task_handle_get_config_page()/
//     link_task_send_config_page() can take, so the LAST one latched shows
//     exactly why the last request got no reply.
// `volatile` for the same reason as the block above (nothing reads these
// back in firmware, only SWD). Safe to delete once this is settled --
// purely diagnostic, no effect on behavior.
/* GET_CONFIG_PAGE service latency, in microseconds, measured 2026-08-25.
 *
 * WHY THIS EXISTS. safety_cfg_store_refetch() on the ESP has been failing with
 * ESP_ERR_TIMEOUT on every attempt for days, and the reason had been recorded
 * as safety_drain_inbox_ex() collapsing its wait to zero when an unrelated
 * broadcast arrived first. That bug is real but was FIXED on 2026-08-23, and
 * the failure outlived the fix, so that explanation is now stale.
 *
 * What the bench actually shows is stranger: over a 45 second window the ESP
 * RECEIVED 91 CONFIG_PAGE frames while reporting a timeout for every single
 * attempt. So the Pico answers and the ESP discards the answer as late. Its
 * budget is SAFETY_LINK_REPLY_TIMEOUT_MS, about 146 ms at 230400 baud, of
 * which the wire time for a 189-byte stuffed frame is only about 8 ms. That
 * leaves roughly 138 ms unaccounted for, and the only place it can be spent is
 * on this side.
 *
 * These three numbers close that gap with a measurement instead of an
 * argument. The bracket is around the dispatch of the handler, so it covers
 * building the page and handing the frame to uart_owner_send() -- i.e. exactly
 * the part of the latency this firmware owns. It does NOT cover time the
 * finished frame then spends queued in the TX ring behind other traffic, which
 * is the other candidate and is governed by log_task.c's
 * LOG_TX_RESERVE_FRACTION. If these read small, the delay is queueing and that
 * fraction is the thing to re-derive; if they read large, the handler itself is
 * slow and the fraction is innocent. Deciding that without measuring is
 * precisely what LOG_TX_RESERVE_FRACTION's own comment forbids.
 *
 * volatile and read only over SWD, same pattern as the diagnostic block below.
 * Purely observational: no behaviour depends on them. */
static volatile uint32_t s_page_reply_us_last = 0;
static volatile uint32_t s_page_reply_us_max = 0;
static volatile uint32_t s_page_reply_samples = 0;

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

    // s_broadcast_raw/s_broadcast_stuffed are shared across every caller of
    // this function (link_task, log_task, update_task) -- see their own
    // declaration comment for why they moved off the stack. Held for the
    // whole encode-stuff-send sequence, released before returning on every
    // path (including the two early-return failure cases below), matching
    // this file's own s_context_lock discipline. Fails CLOSED: if the lock
    // can't be taken within its bound, this frame is dropped exactly like a
    // full TX ring would drop it -- LINK_PROTOCOL.md section 2 rule 3 again,
    // just at a different layer, and no caller treats a dropped broadcast
    // as fatal (that has always been true of uart_owner_send() itself).
    if (xSemaphoreTake(s_broadcast_buf_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, s_broadcast_raw, sizeof(s_broadcast_raw), &status);
    if (raw_len == 0) {
        // encode failure -- shouldn't happen for a well-formed frame we built ourselves
        xSemaphoreGive(s_broadcast_buf_lock);
        return false;
    }

    size_t stuffed_len = kilnlink_stuff(s_broadcast_raw, raw_len, s_broadcast_stuffed, sizeof(s_broadcast_stuffed));
    if (stuffed_len == 0) {
        xSemaphoreGive(s_broadcast_buf_lock);
        return false;
    }

    // uart_owner_send() is itself non-blocking and drops the WHOLE frame if
    // the TX ring has no room (its own counter tracks that) -- exactly
    // LINK_PROTOCOL.md section 2 rule 3. Nothing here retries or escalates.
    // s_broadcast_stuffed is copied into the TX ring by uart_owner_send()
    // before it returns (uart_owner.c's own contract), so it's safe to
    // release the lock immediately after this call, before touching the
    // diagnostic statics below -- nothing past this point reads the shared
    // buffers again.
    bool accepted = uart_owner_send(s_broadcast_stuffed, stuffed_len);
    xSemaphoreGive(s_broadcast_buf_lock);

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
    // 2026-08-23 size-window diagnostic -- see statics' own comment above.
    // Latched AFTER uart_owner_send() returns, same call, so remainder and
    // the isr-bytes snapshot describe exactly this frame.
    if (length > 0 && payload[0] == KILNLINK_DIAG_CMD) {
        s_last_diag_remainder = (uint32_t)uart_owner_get_last_send_remainder();
        s_last_diag_stuffed_len = (uint32_t)stuffed_len;
        s_last_diag_isr_bytes_from_isr_at_send = uart_owner_get_tx_bytes_from_isr();
        // 2026-08-23 round 6 diagnostic -- see statics' own comment above.
        {
            uint32_t n = (uint32_t)length < LINK_TASK_DIAG_SNAPSHOT_LEN
                             ? (uint32_t)length
                             : LINK_TASK_DIAG_SNAPSHOT_LEN;
            for (uint32_t i = 0; i < n; i++) {
                s_last_diag_payload_snapshot[i] = payload[i];
            }
            s_last_diag_send_seq++;
            s_last_diag_tx_head_after = uart_owner_get_tx_head();
            s_last_diag_tx_tail_after = uart_owner_get_tx_tail();
        }
    }
    if (length > 0 && payload[0] == KILNLINK_POWER_CMD) {
        s_last_power_remainder = (uint32_t)uart_owner_get_last_send_remainder();
        s_last_power_stuffed_len = (uint32_t)stuffed_len;
        s_last_power_isr_bytes_from_isr_at_send = uart_owner_get_tx_bytes_from_isr();
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
    // cj_valid is tracked INDEPENDENTLY of temp_valid (2026-09-08,
    // safety_tc_warn_mask_disagreement audit) -- the cold junction is a
    // separate on-chip sensor from the external thermocouple probe
    // temp_valid describes, so a probe/CR1-verify fault must not blank a
    // genuinely good cj_c. See snapshots.h's thermo_snapshot_t.cj_valid doc
    // comment and link_frame.h's LINK_FLAG2_CJ_VALID for the wire side.
    bool cj_valid = th_present && th.cj_valid;
    float cj_c = cj_valid ? th.cj_c : NAN;
    uint8_t fault_bits = th_present ? th.fault_bits : 0;

    current_snapshot_t cur;
    current_task_get_snapshot(&cur);

    bool estop = discrete_task_estop_pressed();

    // safety_tc_installed (0x0211) / injection status -- item 4/5's status
    // reporting, so an operator or the ESP/MCP can tell "declared not
    // installed, heat blocked" and "this reading is synthetic" apart from a
    // clean status. config_store_get_full_record() is the same cheap
    // cached-copy read used everywhere else this field is consulted.
    config_store_record_t status_cfg;
    config_store_get_full_record(&status_cfg);
    bool tc_not_installed = (status_cfg.safety_tc_installed == 0u);
    bool tc_injected = thermo_task_injection_active();

    // BORROWED status (2026-09-03) -- true iff this board's reported reading
    // is (at least partly) sourced from another zone's probe rather than its
    // own J7 input. Mirrors safety_core.c's own S13 gating (tc_source ==
    // CONFIG_STORE_TC_SOURCE_BORROWED_ZONE or _BOTH), read from the same
    // cached config_store record already fetched above for tc_not_installed
    // -- no extra flash/NVS access. borrowed_zone_index is sent verbatim only
    // when it has actually been commissioned (CONFIG_STORE_SET_BORROWED_
    // ZONE_INDEX set); otherwise LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN is
    // sent rather than the field's uninitialized-to-0 default, which would
    // otherwise look exactly like a real, commissioned "zone 0".
    bool is_borrowed = (status_cfg.tc_source == CONFIG_STORE_TC_SOURCE_BORROWED_ZONE) ||
                        (status_cfg.tc_source == CONFIG_STORE_TC_SOURCE_BOTH);
    bool borrowed_zone_index_known =
        (status_cfg.fields_set & CONFIG_STORE_SET_BORROWED_ZONE_INDEX) != 0u;
    uint8_t borrowed_zone_index_wire = borrowed_zone_index_known
                                            ? status_cfg.borrowed_zone_index
                                            : LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN;

    // V2 (24-byte, tx_dropped_sat) status frame gate -- see
    // link_frame_pack_status()'s own doc comment (link_frame.h) for the full
    // skew-safety argument. Only ever true once this boot has positively
    // received an ANNOUNCE_VERSION naming a peer protocol_version >=
    // LINK_FRAME_STATUS_V2_MIN_PROTOCOL; s_peer_protocol_version's own
    // "0 == unknown" default makes "never announced" and "announced, but
    // old" collapse to the same safe (false) outcome here without a separate
    // check.
    bool peer_supports_status_v2 = link_frame_status_v2_supported(s_peer_protocol_version);
    uint8_t tx_dropped_sat = link_frame_saturate_tx_dropped(uart_owner_get_tx_dropped());
    // Same gate, one protocol version higher -- see link_frame_status_v3_
    // supported()'s own doc comment (link_frame.h) for why this must never
    // be true unless peer_supports_status_v2 is also true (numerically
    // guaranteed by LINK_FRAME_STATUS_V3_MIN_PROTOCOL > _V2_MIN_PROTOCOL, but
    // link_frame_pack_status() itself does not trust that ordering blindly).
    bool peer_supports_status_v3 = link_frame_status_v3_supported(s_peer_protocol_version);

    uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
    bool energized_bit = false;
    bool enabled_bit = false;
    safety_core_get_output_status(&energized_bit, &enabled_bit);
    size_t len = link_frame_pack_status(payload, estop, energized_bit, enabled_bit, temp_valid, tc_c, cj_c,
                                         fault_bits, cur.amps[0], cur.amps[1], cur.amps[2],
                                         tc_not_installed, tc_injected,
                                         peer_supports_status_v2, tx_dropped_sat,
                                         peer_supports_status_v3, is_borrowed, borrowed_zone_index_wire,
                                         cj_valid);

    if (link_task_send_broadcast(payload, (uint8_t)len)) {
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
    //
    // COMMIT/DIRTY COME FROM THE EMBEDDED IMAGE-IDENTITY RECORD, NOT FROM THE
    // GENERATED MACROS DIRECTLY (docs/PICO_AUTO_UPDATE_PLAN.md G2). The ESP
    // answers "is this Pico out of date?" by comparing what THIS frame
    // reports against the identity it scans out of the image it is about to
    // push at us. Those two have to be the same bytes or the comparison means
    // nothing -- and two files each reading the same macros independently is
    // exactly how they would silently stop being the same bytes one day. So
    // src/update/saftyfw_image_identity_record.c is the single source and
    // this frame reads it. Doing so also gives that record a live reference,
    // which is what stops --gc-sections deleting it out of the image.
    const saftyfw_image_identity_t *ident = saftyfw_image_identity_get();
    static const char datetime_str[] = SAFTYFW_BUILD_DATE " " SAFTYFW_BUILD_TIME;
    uint8_t payload[16 + SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u + sizeof(datetime_str)];
    size_t len = link_frame_pack_fw_version(
        payload, sizeof(payload), KILNLINK_PROTOCOL_VERSION, KILNLINK_MIN_COMPATIBLE,
        ident->dirty ? 1u : 0u, ident->commit, ident->commit_len, datetime_str,
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
    uint16_t warn_mask = 0u;
    safety_core_get_diag_status(&trip_reason, &warn_active, &diag_state, &warn_mask);

    // trip_mask: this build's safety_guards.c only tracks ONE is_tripped/
    // reason pair for the whole module (5 of 13 guards implemented, see
    // safety_guards.h's own header comment), so this stays a single-bit
    // degraded approximation of LINK_PROTOCOL.md's "one bit per guard"
    // wording -- bit (reason-1) when tripped, matching safety_trip_t's own
    // numbering, so the one bit that IS set at least identifies the right
    // guard. TODO.md records this as the honest state of Frame B's trip_mask.
    //
    // warn_mask, by contrast, IS now the real per-guard mask (Opus review of
    // 51c084f/c49bb0e, finding 1) -- safety_core_get_diag_status() derives it
    // from safety_guards_warn_mask(), which has real per-guard identity for
    // every WARN-capable guard in this build (S4/S5/S9/S10/S12/S13/S14/S15).
    // See safety_guards.h's doc comment on that function for the bit
    // numbering, and LINK_PROTOCOL.md's Frame B table for the wire spec.
    uint16_t trip_mask = link_frame_trip_mask_for_reason(trip_reason);

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

    // 2026-09-09: bits 3-5, from the fatal-fault latch main.c read (and
    // cached) at boot step 3c, before watchdog_overdue_diag_clear() zeroed
    // the physical register -- see kilnlink_diag.h's own comment on these
    // bits for why this needs no protocol-version bump. Read from the RAM
    // cache (watchdog_fatal_diag_get_cached()), not the register itself,
    // since link_task_send_diag() runs repeatedly for the life of this boot
    // while the register was cleared once, at boot, same convention as
    // boot_reason_get_cached() immediately above.
    watchdog_fatal_diag_t fatal = watchdog_fatal_diag_get_cached();
    if (fatal.magic_ok) {
        switch ((watchdog_fatal_kind_t)fatal.kind) {
        case WATCHDOG_FATAL_KIND_STACK_OVERFLOW:
            boot_reason_byte |= KILNLINK_DIAG_BOOT_STACK_OVERFLOW;
            break;
        case WATCHDOG_FATAL_KIND_MALLOC_FAILED:
            boot_reason_byte |= KILNLINK_DIAG_BOOT_MALLOC_FAILED;
            break;
        case WATCHDOG_FATAL_KIND_ASSERT:
            boot_reason_byte |= KILNLINK_DIAG_BOOT_ASSERT_FAILED;
            break;
        default:
            break;
        }
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
    // flags bit1 calibration_missing: WIRED 2026-08-24 to the real
    // config_store record via config_store_is_calibration_missing() --
    // previously hard-coded to 1 unconditionally (TODO.md Phase 8; the wire
    // format already had this bit, so this is a wiring fix, not a protocol
    // change). bit2 estop_unwired_suspect stays 0 -- no detection heuristic
    // is specified in SAFETY_MODEL.md/HARDWARE.md or built anywhere in this
    // codebase yet. bit3 clear_trip_diag_present: WIRED 2026-09-16, same
    // shape as bit1 -- clear_trip_diag_get_cached() (main.c step 3b already
    // reads and caches it at boot, before the register is cleared) reports
    // whether LAST boot left behind a valid CLEAR_TRIP checkpoint; true here
    // means the previous boot reset mid-way through a CLEAR_TRIP drain. This
    // read is safe from link_task's own context: s_clear_trip_diag_cached is
    // written once, before the scheduler starts (see that variable's own
    // declaration), so no torn-read/lifetime hazard the way a mid-boot write
    // from another task would have. Assembly itself lives in
    // link_diag_flags.c, pure and host-tested (test/test_link_diag_flags.c),
    // since this file cannot be.
    //
    // Note what parsing PUSH_CONTEXT does NOT yet mean: there is still no
    // context-consuming correlation guard (S2/S3/S4/S6/S10) to disable on
    // sim_context_seen or reset on a boot_id change -- this frame reports
    // that the fact is known, not that anything downstream acts on it yet.
    // bit4 tc_reconfig_gave_up: WIRED 2026-09-16, same shape as bit3 just
    // above -- thermo_task_reconfig_gave_up() reads thermo_task.c's
    // s_reconfig_gave_up (a plain volatile bool, safe to read from this
    // task's own context the same way thermo_task_injection_active() is).
    // bit5/bit6 abs_max_temp_disabled/rate_guard_disabled: WIRED 2026-09-22
    // -- SAFETY_MODEL.md "guards with no defensible default ship disabled,
    // and say so in telemetry" had no distinct signal for S1/S8's
    // disabled-by-zero config before this (the bundled CALIBRATION_MISSING
    // bit does not answer "is S1/S8 itself armed"). See kilnlink_diag.h's
    // bit5/bit6 comments and config_store.h's config_store_is_abs_max_temp_
    // disabled()/config_store_is_rate_guard_disabled() for the distinction.
    // bit7 config_volatile_dirty: WIRED 2026-09-23 -- config_store_write_
    // volatile() (item 15) lands its config_version/config_crc bump in RAM
    // only; this bit tells a reader (the ESP, or a bench operator) that a
    // reboot right now would NOT reproduce the config_version this same
    // frame reports. See kilnlink_diag.h's bit7 comment and config_store.h's
    // config_store_is_volatile_dirty().
    uint8_t diag_flags =
        link_diag_flags_compute(config_store_is_calibration_missing(), s_context_sim_seen,
                                 clear_trip_diag_get_cached().magic_ok,
                                 thermo_task_reconfig_gave_up(),
                                 config_store_is_abs_max_temp_disabled(),
                                 config_store_is_rate_guard_disabled(),
                                 config_store_is_volatile_dirty());
    //
    // Built via the shared kilnlink_diag_encode() codec (CommonFW/src/
    // kilnlink_diag.c, host-tested test_diag.c) rather than this file's own
    // former hand-rolled link_frame_pack_diag() -- ROADMAP.md M5: the two
    // were duplicate implementations of the identical layout (26 bytes then;
    // 30 since KILNLINK_PROTOCOL_VERSION 16), and this removes the
    // duplication now that the shared codec exists (it
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
        .log_frames_dropped = log_task_get_dropped(),
    };
    uint8_t payload[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t len = kilnlink_diag_encode(&dg, payload, sizeof(payload), &status);
    // 2026-08-23 call-path diagnostic, checkpoint 2 -- recorded regardless of
    // outcome, before the early-return can act on it.
    s_diag_encode_len = (uint32_t)len;
    s_diag_encode_status = (uint8_t)status;
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

    // counts_avg comes from the UNFILTERED snapshot (current_snapshot_t),
    // not current_sense_power_t -- see snapshots.h's field comment. Pulled
    // as its own getter call rather than added to current_sense_power_t,
    // matching current_sense.h's own split: the wire-facing power struct is
    // deliberately built from the filtered/derived reporting quantities
    // only, and counts_avg is neither -- it is the raw pre-conversion value.
    current_snapshot_t snap_for_counts;
    current_task_get_snapshot(&snap_for_counts);

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
        frame.counts_avg[ch] = snap_for_counts.counts_avg[ch];
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
    // 2026-08-23 call-path diagnostic, checkpoint 2 -- recorded regardless of
    // outcome, before the early-return can act on it.
    s_power_encode_len = (uint32_t)len;
    s_power_encode_status = (uint8_t)status;
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
    // built; acting on it is deliberately NOT -- this is not a stale TODO,
    // it is a considered decision to stay one-sided (see the "reset one side
    // of a pair" bug class this codebase has hit four times before; this is
    // the fifth candidate, examined and rejected).
    //
    // safety_guards.c's context-consuming guards (S2, S3, S4, S10, S13; see
    // its "Context-dependent guards" block, ~line 795) already reset their
    // own elapsed-time accumulators whenever in->context_valid goes false --
    // and context_valid itself goes false on any gap of
    // LINK_TASK_CONTEXT_MAX_AGE_MS (5000 ms, see its #define below) between
    // PUSH_CONTEXT frames. An ESP reboot slow enough to matter -- anything
    // at or past that 5 s gap -- is already covered: the accumulators reset
    // on staleness before boot_id_changed would ever need to fire.
    //
    // The only case boot_id_changed could add coverage for is an ESP reboot
    // FAST enough to resume PUSH_CONTEXT within that 5 s window with a new
    // boot_id -- fast enough that context never goes stale in between. Not
    // resetting the accumulators in that residual case is the SAFE
    // direction: a guard mid-way through accumulating toward a trip keeps
    // that progress across the reboot, so the guard fails toward tripping
    // sooner, never later. Wiring boot_id_changed to force a reset here
    // would LOOSEN every one of those guards by giving a fast-rebooting ESP
    // a free correlation-window reset on demand -- exactly the wrong
    // direction for a safety guard. So `boot_id_changed` stays computed and
    // discarded on purpose; this comment is the record that the omission was
    // considered, not overlooked.
    bool boot_id_changed = s_context_boot_id_known && snap.boot_id != s_last_context_boot_id;
    (void)boot_id_changed; // suppress unused-variable-as-error; see comment above
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

    // The decode-to-verdict step is factored into
    // link_task_evaluate_announce_version() (link_task_announce_eval.c) so it
    // can be host-tested: this file pulls in FreeRTOS/pico-sdk and is not
    // built for the host test executable, which had left the actual
    // ANNOUNCE_VERSION wiring -- as opposed to the pure
    // link_frame_versions_compatible() formula it calls, which already was
    // tested -- with no coverage at all.
    link_task_announce_eval_t eval = link_task_evaluate_announce_version(
        &msg, KILNLINK_PROTOCOL_VERSION, KILNLINK_MIN_COMPATIBLE);

    // Cached unconditionally, even if this peer turns out incompatible below
    // -- an incompatible peer's own protocol_version is still real,
    // meaningful data (it is, after all, why the compatibility check just
    // failed), and link_task_send_status() needs it regardless of the
    // DEGRADED_NO_CONTEXT verdict: LINK_FRAME_STATUS_V2_MIN_PROTOCOL gating
    // is deliberately a narrower, additive-feature-specific question than
    // "are we fully compatible" (see link_frame_pack_status()'s doc comment,
    // link_frame.h).
    s_peer_protocol_version = eval.peer_protocol_version;

    // LINK_PROTOCOL.md section 4, "What each side does about a mismatch":
    // the Pico enters DEGRADED_NO_CONTEXT and does NOT latch a trip. This is
    // the ONLY effect a version mismatch has from in here -- no relay/trip
    // call, by design (and this file could not make one anyway, see the
    // header comment).
    s_degraded_no_context = eval.degraded_no_context;
}

// SAFETY_CMD_REQUEST_ENABLE (0x02), CommonFW/docs/LINK_PROTOCOL.md section 4
// ("Kept, converted to BROADCAST. Advisory only -- the Pico's interlocks
// always win"). 2-byte payload: cmd (0x02) + a 0/1 enable byte, exactly what
// firmware/KilnFW/App/drivers/safety/safety_link.c's safety_link_request_enable()
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
// the GUI's path to acknowledging a trip. This function decodes and
// validates the wire frame and forwards a valid, matching request to
// safety_core's own queue; the actual refuse/clear policy (still-tripped
// retick) is safety_core_task's job now, not this function's -- see
// safety_core_request_clear_trip()'s doc comment in safety_core.h for why
// this changed from a direct, synchronous call (2026-08-23: that direct
// call was writing safety_core's un-locked s_guard_state from this task's
// own core while safety_core_task ticked the same struct on the other core,
// and reproduced a hardware watchdog reboot). link_task calling INTO
// safety_core (never the reverse) is otherwise unchanged -- this file still
// never needs to be called by, or export anything to, safety_core.c.
//
// Two refusal paths per the protocol doc, both checked here, entirely
// locally, before safety_core's queue is ever touched:
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
// LOGGING CONTRACT: this function only ever logs that a frame arrived, was
// screened, and was queued (or dropped/refused locally) -- never
// "accepted"/"cleared". The actual accept/refuse decision happens inside
// safety_core_task once it dequeues the request, and IT logs that outcome
// (safety_core.c's own "clear_trip" log line, with req/proc counts and the
// resolved outcome) -- matching LINK_PROTOCOL.md's documented CLEAR_TRIP
// contract of "refused, with the reason reported in the next diagnostic
// frame", which was always async on the wire; this file's log line is
// diagnostic-only telemetry, not the wire-visible outcome.
//
// s_clear_trip_rx_count (incremented on every successfully DECODED frame,
// before either local refusal check) plus safety_core_get_clear_trip_stats()'s
// requested/processed/outcome triple together answer, from the log alone,
// exactly how far a given CLEAR_TRIP got: received here, queued to
// safety_core, dequeued by safety_core, resolved -- so a repeat of the
// hardware reboot can be localized to a specific hop instead of guessed at.
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
    s_clear_trip_rx_count++;

    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    safety_core_get_diag_status(&trip_reason, NULL, NULL, NULL);

    // link_frame_decide_clear_trip() (src/tasks/link_frame.c) is the pure,
    // host-tested extraction of the two refusal checks documented above --
    // this function only acts on its verdict now.
    link_clear_trip_decision_t decision = link_frame_decide_clear_trip(trip_reason, msg.trip_mask);

    // A switch with no default, deliberately. This was an if-chain that tested
    // the two refusal values it knew about and let everything else fall
    // through to the accept path below -- so 2026-08-27's new
    // LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE (S9, welded contactor: the one alarm
    // whose required response is to go to the breaker) arrived and was
    // silently ACCEPTED here. It happened to be refused anyway, deeper down in
    // safety_guards_try_clear(), which is precisely what makes the shape
    // dangerous: the defence that saved it was somewhere else, and nothing
    // here would have said so. Enumerating every case without a default means
    // the next value added to link_clear_trip_decision_t is a -Wswitch
    // compile error at this line instead of a permissive fall-through.
    switch (decision) {
    case LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED:
        log_task_log(LOG_LEVEL_INFO, "clear_trip", "ignored, nothing tripped");
        return;
    case LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH:
        log_task_log(LOG_LEVEL_WARN, "clear_trip", "refused, trip_mask mismatch");
        return;
    case LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE:
        // ERROR, not WARN: every other refusal here means "your request did not
        // apply". This one means mains may still be flowing through fused
        // contacts and the operator is at the wrong end of the building trying
        // to clear it from a screen. ARCHITECTURE.md sec 9 and SAFETY_MODEL.md
        // sec 4 both say this trip has no exit except power removal at the
        // breaker; the log line has to say that too, since it is the only
        // thing the person clicking Clear will see.
        log_task_log(LOG_LEVEL_ERROR, "clear_trip",
                     "REFUSED: trip ineffective (S9) -- contactor may be welded, "
                     "remove power at the breaker; not clearable from here");
        return;
    case LINK_CLEAR_TRIP_ACCEPT:
        break;
    }

    bool queued = safety_core_request_clear_trip();
    char msg_buf[64];
    snprintf(msg_buf, sizeof(msg_buf), "%s, rx=%lu",
             queued ? "queued" : "dropped, safety_core queue full",
             (unsigned long)s_clear_trip_rx_count);
    log_task_log(queued ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, "clear_trip", msg_buf);
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
// F1 (2026-09-15 owner decision on the Opus review): "as far as the Pico
// can tell from its own inputs" that heat is not currently being
// delivered.
//
// 2026-09-15 Opus review G2 fix: this used to be `!s_relay_on_continuous &&
// !current_task_any_current_present()` -- s_relay_on_continuous tracks the
// context frame's INSTANT relay_now_mask, which reads false in every PWM
// off-window even mid-firing (a duty-cycled heater spends most ticks with
// the relay actually open), so this read "safe" between pulses of a firing
// that was very much still running.
//
// 2026-09-15 Opus RE-review N1 fix: G2's fix based the gate on
// CONTEXT_FLAG_HEAT_REQUESTED/PROFILE_RUNNING -- both still literal/narrow:
// HEAT_REQUESTED is STILL the instant relay-commanded state (same PWM-chop
// problem, one level up: the ESP sets it from the same relay_commanded_on
// bits), so it read false in every off-window too, and it does not cover
// autotune, a PAUSED firing, danger-mode K4 requests, or the deferred-
// release window (heat_enable_release() flips heat_enable_is_granted() to
// false SYNCHRONOUSLY, before the wire REQUEST_ENABLE(false) send that
// actually asks the Pico to drop K4 -- see heat_enable.h's own doc comment
// on heat_enable_service_pending_release()). The owner rule per that
// review: "the Pico owns that pole" -- base the gate on the Pico's OWN
// K4-grant ground truth, not on trying to infer the ESP's instantaneous
// relay state from a wire flag that shares its literal-snapshot problem.
//
// The gate is now three independent, ALL-must-pass checks, none trusted
// exclusively (defense in depth, same philosophy as the s_relay_on_
// continuous/current_task_any_current_present() heuristics below):
//   1. safety_core_get_output_status()'s relay_energized -- is GPIO6
//      ACTUALLY high right now. This is the Pico's own hardware ground
//      truth, immune to any ESP-side bookkeeping race.
//   2. safety_core_ms_since_last_enable_true_request() -- did THIS
//      processor accept a REQUEST_ENABLE(true) recently. relay_owner_
//      command_energize() is a queued command (relay_owner.c drains it on
//      its own tick), so there is a real window, right after an accepted
//      enable request, where check 1 can still read false even though the
//      Pico has committed to energizing -- "treat a pending or recent
//      enable as not safe" (the review's own wording). ENABLE_RECENT_
//      SAFE_WINDOW_MS below is chosen well above one relay_owner tick
//      period so a genuinely-settled disable clears it.
//   3. The ESP's CONTEXT_FLAG_HEAT_OWNER_ACTIVE (kilnlink_context.h) --
//      "no active heat owner", ORing together executor RUNNING/PAUSED,
//      either heat_enable claimant (autotune's claimant also covers CT
//      sweep / relay-identification), and danger mode. This is the
//      "require the ESP report no active heat owner" half of the rule --
//      kept as the SECOND, ESP-reported line of defense, not the primary
//      one, since checks 1-2 alone already answer the question "the Pico
//      owns that pole" poses without needing to trust the ESP at all; an
//      ESP that lies or glitches about this flag still has to also fool
//      the Pico's own relay/current-sense heuristics below.
//
// Stale or missing context data is treated as NOT safe for check 3, matching
// every other context-consuming guard in this codebase (safety_core.c's own
// context_valid: age >= LINK_TASK_CONTEXT_MAX_AGE_MS, never received this
// boot, or link_task's own DEGRADED_NO_CONTEXT state, all collapse to the
// conservative default) -- this function only ever WIDENS a refusal into an
// acceptance, so any doubt about freshness must fail closed, never open.
// s_relay_on_continuous and current_task_any_current_present() remain as
// additional, independent Pico-local heuristics on top of checks 1-3
// (defense in depth) -- none of the checks is trusted exclusively.
//
// Reads only the handful of scalars this needs under s_context_lock,
// deliberately not a whole context_snapshot_t copy via link_task_get_
// context_snapshot() -- this sits on link_task's SET_CONFIG/COMMIT_CONFIG
// call chain, and check_saftyfw_task_stack_budgets.py already grades
// link_task against a tight regsp margin (see current_task_any_current_
// present()'s own header comment for the same reasoning applied there).

// 2026-09-15 Opus re-review N2: set when a SET_CONFIG/COMMIT_CONFIG committed
// a new tc_type to flash but could not reconfigure the physical MAX31856 to
// match (heat became enabled in the write-to-reapply window) -- the flash
// record and the chip are diverged at that point. link_task_fn()'s main loop
// calls link_task_retry_pending_tc_type_reapply() every poll to keep trying
// until heat is safe again and the reapply is actually re-requested, instead
// of leaving the divergence stranded silently forever. single-writer: only
// link_task's own task context ever touches these two (set in the two
// handlers above, cleared only here).
//
// 2026-09-15 (Opus adversarial re-review, F5): this flag clears as soon as
// the reapply is re-requested (thermo_task_request_tc_type_reapply(), which
// only sets s_force_tc_reconfigure), NOT when the chip is confirmed
// reconfigured -- it is a "retry was dispatched" latch, not a "verified"
// latch. What actually finishes the job is thermo_task's own retry loop:
// a failed max31856_configure() leaves max31856_tc_type_verified() false,
// and max31856_reconfig_retry_should_attempt() keeps retrying independently
// of this flag.
static bool s_tc_type_reapply_pending = false;
static uint8_t s_tc_type_reapply_pending_value = 0;

// Decision core moved to link_task_tc_type_gate.c (pure, host-testable --
// this function itself pulls in FreeRTOS/pico-sdk headers and cannot be
// built for the host test executable). This function only reads live
// state and hands it to the pure decider.
static bool link_task_heat_is_safe_for_tc_type_change(void)
{
    link_task_tc_type_gate_input_t in;
    memset(&in, 0, sizeof(in));

    // Check 1: Pico's own hardware ground truth -- is K4 actually energized
    // right now.
    safety_core_get_output_status(&in.relay_energized, NULL);

    // Check 2: did this processor recently accept a REQUEST_ENABLE(true)
    // that may not have physically closed the relay yet.
    in.enable_age_ms = safety_core_ms_since_last_enable_true_request(&in.enable_ever_seen);

    // Check 3 inputs: the ESP's own "no active heat owner" report.
    in.degraded_no_context = s_degraded_no_context;
    in.context_ever_received = (s_context_lock != NULL) && s_context_published;
    in.context_max_age_ms = LINK_TASK_CONTEXT_MAX_AGE_MS;

    if (in.context_ever_received) {
        if (xSemaphoreTake(s_context_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            in.context_lock_available = true;
            in.context_valid = s_context_snapshot.valid;
            in.context_flags = s_context_snapshot.flags;
            uint32_t ctx_timestamp_ms = s_context_snapshot.timestamp_ms;
            xSemaphoreGive(s_context_lock);

            // Same wraparound-safe unsigned subtraction as safety_core.c's
            // own context_valid computation -- both operands come from the
            // same to_ms_since_boot() clock.
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            in.context_age_ms = now_ms - ctx_timestamp_ms;
        }
        // else: context_lock_available stays false (could not confirm
        // freshness) -- not safe, per the pure decider's fail-closed rule.
    }

    // Additional Pico-local heuristics, defense in depth.
    in.relay_on_continuous = s_relay_on_continuous;
    in.any_current_present = current_task_any_current_present();

    return link_task_tc_type_gate_decide(&in, CONTEXT_FLAG_HEAT_REQUESTED,
                                          CONTEXT_FLAG_PROFILE_RUNNING,
                                          CONTEXT_FLAG_HEAT_OWNER_ACTIVE);
}

// 2026-09-15 Opus re-review N2: called every poll from link_task_fn()'s main
// loop. While s_tc_type_reapply_pending is set, the persisted flash record
// and the physically configured MAX31856 may be diverged -- keep retrying
// the *request* until heat is confirmed safe again, rather than the
// one-shot skip silently stranding the divergence for the rest of this boot
// (the review's "keep retrying the reapply, and report it" option). This
// function only re-requests the reapply once heat is safe; it does not by
// itself confirm the chip actually picked it up -- see the comment on
// s_tc_type_reapply_pending above (F5) and thermo_task's own
// verified/retry-should-attempt loop for that.
static void link_task_retry_pending_tc_type_reapply(void)
{
    if (!s_tc_type_reapply_pending) {
        return;
    }
    if (!link_task_heat_is_safe_for_tc_type_change()) {
        return; // still diverged -- try again next poll
    }
    log_task_log(LOG_LEVEL_WARN, "tc_type_reapply",
                 "retry: heat now safe, re-requesting previously-skipped tc_type reapply "
                 "(thermo_task's own verify/retry loop confirms the chip picked it up)");
    thermo_task_request_tc_type_reapply();
    (void)s_tc_type_reapply_pending_value; // carried only for future diagnostics/logging
    s_tc_type_reapply_pending = false;
}

// Ticks CONFIG_REFERENCE.md's config_check_period_s: every LINK_CONFIG_
// CHECK_POLL_MS (1s), read the live config_check_period_s field and, once
// that many seconds have actually elapsed, run config_store_check_ram_
// integrity(). link_task is config_store's sole writer/owner on this
// processor (every config_store_write()/config_store_write_volatile() call
// is already reached only from this task, per config_store_seqlock_write()'s
// own comment), so ticking the re-CRC from here needs no new task and no new
// lock.
//
// config_check_period_s == 0 disables the check -- CONFIG_REFERENCE.md's
// documented behavior -- by simply never accumulating toward it below.
// s_config_check_elapsed_s is this function's only state and its only
// owner; nothing else reads or resets it (the "reset one side of a pair"
// class this codebase watches for doesn't apply -- there is exactly one
// side here, and it owns both halves of its own reset).
static void link_task_config_check_poll(TickType_t now)
{
    static TickType_t s_last_poll = 0;
    static uint32_t s_config_check_elapsed_s = 0u;
    static bool s_initialized = false;

    if (!s_initialized) {
        s_last_poll = now;
        s_initialized = true;
        return;
    }
    if ((now - s_last_poll) < pdMS_TO_TICKS(LINK_CONFIG_CHECK_POLL_MS)) {
        return;
    }
    s_last_poll = now;

    config_store_record_t snap;
    if (!config_store_get_full_record(&snap) || snap.config_check_period_s == 0u) {
        // Not loaded yet, no stable snapshot this poll, or the check is
        // disabled -- keep the accumulator at rest rather than let it carry
        // stale progress into a later period that might be reconfigured.
        s_config_check_elapsed_s = 0u;
        return;
    }

    s_config_check_elapsed_s++;
    if (s_config_check_elapsed_s < snap.config_check_period_s) {
        return;
    }
    s_config_check_elapsed_s = 0u;

    if (!config_store_check_ram_integrity()) {
        log_task_log(LOG_LEVEL_ERROR, "config_check",
                     "periodic RAM integrity check FAILED -- reset to compiled defaults, "
                     "calibration_missing set");
    }
}

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

    // Read-modify-write against the committed record, NOT config_store_
    // default() -- see link_frame_apply_set_config()'s own header comment
    // (link_frame.h) for the fleet-affecting bug this replaced: starting
    // from config_store_default() and only overwriting tc_type/
    // calibration_missing meant config_store_write()'s no-merge, whole-
    // record replace silently wiped every OTHER commissioned field on
    // every SET_CONFIG, including the ones KilnFW's safety_link.c fires
    // automatically on every link reconnect.
    config_store_record_t committed;
    config_store_get_full_record(&committed);
    config_store_record_t rec;
    link_frame_apply_set_config(&committed, msg.tc_type, &rec);

    const char *reason = NULL;
    // 2026-09-15 Opus re-review N2: bracket the write-plus-reconfigure span
    // so a REQUEST_ENABLE cannot land between the flash write above and the
    // MAX31856 reconfigure below and race them -- see safety_core_set_tc_
    // type_apply_in_progress()'s own doc comment. Cleared on every exit path
    // below (accepted-and-reapplied, accepted-and-skipped, and refused).
    safety_core_set_tc_type_apply_in_progress(true);
    bool written =
        config_store_write_ex(&rec, link_task_heat_is_safe_for_tc_type_change(), &reason, NULL);
    if (written) {
        log_task_log(LOG_LEVEL_INFO, "set_config", "accepted");
        // Take effect immediately, not after a reboot -- thermo_task.c's own
        // comment on thermo_task_request_tc_type_reapply() explains the
        // fail-safe path this rides: max31856_configure() clears
        // max31856_tc_type_verified() unconditionally at entry, so every
        // snapshot published between now and a confirmed CR1 readback of the
        // NEW type is already reported invalid, the same way a dead/unplugged
        // part would be. config_store_write() above has already refused this
        // whole call if the relay is armed, so this can only run while the
        // Pico is not currently firing. Gated through tc_type_reapply_
        // policy_should_reapply() (2026-09-15 review, F5) so a SET_CONFIG
        // that leaves tc_type unchanged does not needlessly re-open the
        // verification window.
        // 2026-09-15 Opus review G2: re-check immediately before the
        // reconfigure, not just before the flash write above -- the flash
        // write itself takes real time (config_store_flash.c's erase/program
        // path), during which heat could become enabled. Reapplying the TC
        // type mid-firing means max31856_configure() clears max31856_tc_
        // type_verified() and reopens the verification window while heat may
        // now be on, which this function exists specifically to prevent.
        if (tc_type_reapply_policy_should_reapply(committed.tc_type, rec.tc_type)) {
            if (link_task_heat_is_safe_for_tc_type_change()) {
                thermo_task_request_tc_type_reapply();
            } else {
                // 2026-09-15 Opus re-review N2: this is exactly the "flash
                // and chip now disagree" case the review named -- the flash
                // record already holds the new tc_type but the physical
                // MAX31856 was never reconfigured to match, because heat
                // became enabled in the window between the write and here.
                // Escalated from WARN to ERROR and marked pending so link_
                // task_retry_pending_tc_type_reapply() (called from the main
                // loop below) keeps retrying until it verifies, instead of
                // this being a one-shot skip that silently strands the
                // divergence forever.
                log_task_log(LOG_LEVEL_ERROR, "set_config",
                             "DIVERGED: tc_type committed to flash but chip reapply skipped "
                             "(heat enabled) -- will retry");
                s_tc_type_reapply_pending = true;
                s_tc_type_reapply_pending_value = rec.tc_type;
            }
        }
    } else {
        log_task_log(LOG_LEVEL_WARN, "set_config", reason ? reason : "refused");
    }
    safety_core_set_tc_type_apply_in_progress(false);
}

// SAFETY_CMD_SET_CT_CAL (0x19), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// firmware/SimFW/tools/ct_calibration/README.md's documented gap: "no MCP
// tool exists to push calibration constants ... to the RP2040's own flash."
// Same fire-and-forget shape as link_task_handle_set_config() immediately
// above: decode, validate, log accept/refuse, never ACK on the wire.
//
// Read-modify-write: config_store_write() replaces the ENTIRE record, so
// this fetches the FULL committed record via config_store_get_full_record()
// (link_frame_apply_set_ct_cal(), link_frame.h/.c) and overwrites only the
// ONE channel this frame named -- setting channel 1 must never disturb
// channel 0 or 2's stored constants, and must never re-arm calibration_
// missing, change tc_type, or touch any section 1-5 threshold or fields_set
// bit, none of which are a SEPARATE commissioning concern (thermocouple/
// commissioning state, not CT current) that SET_CT_CAL has no business
// touching. 2026-08-24: this handler previously rebuilt those "leave alone"
// fields individually off config_store_default() instead of reading the
// committed record, which silently wiped everything it did not explicitly
// name (same class of bug as SET_CONFIG's, see link_task_handle_set_config()
// above) -- fixed by reading the whole record once instead.
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

    // Read-modify-write against the committed record via config_store_
    // get_full_record(), not the piecemeal getters + config_store_default()
    // this used to build on -- see link_frame_apply_set_ct_cal()'s own
    // header comment (link_frame.h) for why: this handler already
    // preserved tc_type/calibration_missing/ct_cal individually, but still
    // started from config_store_default() underneath that, so every
    // section 1-5 threshold and fields_set bit was still silently wiped on
    // every SET_CT_CAL.
    config_store_record_t committed;
    config_store_get_full_record(&committed);
    config_store_record_t rec;
    link_frame_apply_set_ct_cal(&committed, msg.channel, (msg.calibrated != 0u), msg.gain,
                                 msg.offset, &rec);

    const char *reason = NULL;
    bool written = config_store_write(&rec, &reason);
    if (written) {
        log_task_log(LOG_LEVEL_INFO, "set_ct_cal", "accepted");
        // Take effect immediately, not after a reboot -- current_task.c's
        // own comment on current_task_reload_ct_cal() explains why a live
        // commissioning session needs this. tc_type now gets the same
        // treatment via thermo_task_request_tc_type_reapply(), see
        // link_task_handle_set_config() above.
        current_task_reload_ct_cal();
    } else {
        log_task_log(LOG_LEVEL_WARN, "set_ct_cal", reason ? reason : "refused");
    }
}

// SAFETY_CMD_GET_CT_CAL (0x22, its own id since KILNLINK_PROTOCOL_VERSION 7),
// request only -- CommonFW/docs/LINK_PROTOCOL.md section 4. Same shape as
// SAFETY_CMD_GET_FW_VERSION above: answer every copy seen (the ESP is the
// side allowed to retry), reply via link_task_send_ct_cal() under the
// reply's own id (0x1A, unchanged).
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

// SAFETY_CMD_STACK_MARGIN (0x2C) reply -- sent in answer to
// SAFETY_CMD_GET_STACK_MARGIN (link_task_handle_get_stack_margin() below).
// KILNLINK_PROTOCOL_VERSION 13. Reports stack_margin_poller.c's CACHED
// snapshot, never a fresh synchronous measurement -- see that module's own
// header comment for why (the 2026-08-23 watchdog-timing regression this
// design avoids repeating).
static void link_task_send_stack_margin(void)
{
    kilnlink_stack_margin_t snapshot;
    stack_margin_poller_snapshot(&snapshot);

    uint8_t payload[KILNLINK_STACK_MARGIN_LEN];
    kilnlink_stack_margin_status_t status;
    size_t len = kilnlink_stack_margin_encode(&snapshot, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // shouldn't happen for a well-formed frame built from a fixed-size local buffer
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

// SAFETY_CMD_GET_STACK_MARGIN (0x2B), request only -- CommonFW/docs/
// LINK_PROTOCOL.md sec 4, KILNLINK_PROTOCOL_VERSION 13. Same shape as
// SAFETY_CMD_GET_CT_CAL above: answer every copy seen (the ESP is the side
// allowed to retry/poll at its own slow cadence), reply via
// link_task_send_stack_margin() under the reply's own id (0x2C).
static void link_task_handle_get_stack_margin(const kilnlink_frame_t *frame)
{
    kilnlink_get_stack_margin_t msg;
    kilnlink_get_stack_margin_status_t dstatus =
        kilnlink_get_stack_margin_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_GET_STACK_MARGIN_OK) {
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    (void)msg; // no fields
    link_task_send_stack_margin();
}

// SAFETY_CMD_CT_AUTO_ZERO_BEGIN (0x26), CT_COMMISSIONING_PLAN.md step 2.
// Fire-and-forget, like SET_CT_CAL: never ACKed on the wire. Only ARMS
// current_task.c's own accumulator (current_task_ct_auto_zero_begin()) --
// see link_frame.h's LINK_FRAME_CT_AUTO_ZERO_BEGIN_CMD comment for why the
// actual multi-second measurement must never run synchronously here. The
// ESP learns whether the arm succeeded (channel in range, not already
// IN_PROGRESS) via its own follow-up GET_CT_AUTO_ZERO poll, same
// "outcome via a subsequent read" convention CLEAR_TRIP/SET_CONFIG use.
static void link_task_handle_ct_auto_zero_begin(const kilnlink_frame_t *frame)
{
    kilnlink_ct_auto_zero_begin_t msg;
    kilnlink_ct_auto_zero_begin_status_t dstatus =
        kilnlink_ct_auto_zero_begin_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_CT_AUTO_ZERO_BEGIN_OK) {
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    if (!current_task_ct_auto_zero_begin(msg.channel)) {
        log_task_log(LOG_LEVEL_WARN, "ct_auto_zero", "refused, channel out of range or already in progress");
    } else {
        log_task_log(LOG_LEVEL_INFO, "ct_auto_zero", "armed");
    }
}

// SAFETY_CMD_CT_AUTO_ZERO_STATUS (0x28) reply -- sent in answer to
// SAFETY_CMD_GET_CT_AUTO_ZERO (link_task_handle_get_ct_auto_zero() below).
// Reports current_task.c's own accumulator state exactly, no interpretation
// here -- the ESP decides refusal/100mV/manual-wins policy from this raw
// measurement (CT_COMMISSIONING_PLAN.md step 2), same "this codec only
// serializes bytes, the receiver decides what they mean" split every other
// kilnlink reply in this file already uses.
static void link_task_send_ct_auto_zero_status(void)
{
    current_task_auto_zero_status_t st;
    current_task_ct_auto_zero_poll(&st);

    kilnlink_ct_auto_zero_status_t msg = {
        .state = (uint8_t)st.state,
        .channel = st.channel,
        .samples_taken = st.samples_taken,
        .samples_target = st.samples_target,
        .zero_counts = st.zero_counts,
    };
    uint8_t payload[KILNLINK_CT_AUTO_ZERO_STATUS_LEN];
    kilnlink_ct_auto_zero_status_codec_t status;
    size_t len = kilnlink_ct_auto_zero_status_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        return;
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

static void link_task_handle_get_ct_auto_zero(const kilnlink_frame_t *frame)
{
    kilnlink_get_ct_auto_zero_t msg;
    kilnlink_get_ct_auto_zero_status_t dstatus =
        kilnlink_get_ct_auto_zero_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_GET_CT_AUTO_ZERO_OK) {
        return; // malformed/wrong-length/wrong-cmd -- untrusted wire input
    }
    (void)msg; // no fields
    link_task_send_ct_auto_zero_status();
}

// Forward declaration -- link_task_handle_rollback() below calls this on
// its refusal path, but the function itself (defined right after) also
// wants to sit next to link_task_handle_rollback() in the file for
// locality, same trade-off this file already makes elsewhere.
static void link_task_send_rollback_result(uint8_t reason_code);

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
    uint8_t reason_code = KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN;
    bool accepted = update_task_request_rollback(&reason, &reason_code);
    // Reached only on refusal/failure -- see the function's own doc comment.
    if (!accepted) {
        log_task_log(LOG_LEVEL_WARN, "rollback", reason ? reason : "refused");
        link_task_send_rollback_result(reason_code);
    }
}

// SAFETY_CMD_ROLLBACK_RESULT (0x25), CommonFW/docs/LINK_PROTOCOL.md sec 4 --
// the missing wire-visible reply for a refused SAFETY_CMD_ROLLBACK, sent
// ONLY from link_task_handle_rollback() above's refusal path (never on
// acceptance -- update_task_request_rollback() does not return in that
// case; see kilnlink_rollback_result.h's own "ASYMMETRIC BY DESIGN"
// comment). Gated by link_frame_rollback_result_supported() on this boot's
// cached s_peer_protocol_version, the EXACT same skew-safety discipline
// link_task_send_status() already applies to its own V2 (24-byte) frame --
// an ESP that has not positively announced protocol_version >= 9 never
// receives a frame its dispatch switch has no case for.
static void link_task_send_rollback_result(uint8_t reason_code)
{
    if (!link_frame_rollback_result_supported(s_peer_protocol_version)) {
        // Peer never announced (0, the safe default) or announced an old
        // version -- stay silent, same as this frame not existing at all
        // for that peer. The refusal is still fully recorded in THIS boot's
        // own log line above; only the wire visibility is skipped.
        return;
    }

    kilnlink_rollback_result_t msg = {0};
    msg.accepted = 0; // only ever sent on refusal, see this function's own doc comment
    msg.reason = reason_code;

    uint8_t payload[KILNLINK_ROLLBACK_RESULT_LEN];
    kilnlink_rollback_result_status_t status;
    size_t len = kilnlink_rollback_result_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        return; // shouldn't happen for a well-formed frame built from a fixed-size local buffer
    }
    link_task_send_broadcast(payload, (uint8_t)len);
}

// How long link_task_handle_reboot() below will wait for the TX ring to
// drain after queuing its SAFETY_CMD_REBOOT_RESULT reply, before resetting
// the chip out from under it. A 3-byte payload is ~12 stuffed bytes on the
// wire; at this link's 230400 baud that is well under 1 ms, so 10 ms is
// ~20x margin. It is also deliberately BELOW link_task's own 30 ms
// watchdog check-in deadline (watchdog_task.c's WATCHDOG_CHECKIN_LINK_TASK
// row): blocking this task past that deadline would reset the board via the
// watchdog instead of via update_task_reboot_now(), which would still be a
// reboot but an unexplained one, and would land BEFORE the reply the
// operator is waiting on ever left the ring.
#define LINK_TASK_REBOOT_TX_DRAIN_MS 10u

// SAFETY_CMD_REBOOT (0x29), CommonFW/docs/LINK_PROTOCOL.md section 4 -- the
// Pico half of KilnFW's POST /api/sw_reset: "reboot yourself, in place,
// into the SAME firmware slot you are running now."
//
// NOT link_task_handle_rollback() above: that one writes a bootloader
// metadata record and comes back on the OTHER image, and is refusable by
// bootloader_decide_rollback() for reasons that have nothing to do with
// this command. NOT link_task_handle_announce_reboot() below either: that
// one is a courtesy notice about the ESP's own reboot and does nothing to
// this processor.
//
// Unlike EVERY other consequential ESP->Pico command in this file, this one
// ACKs on the wire in BOTH directions -- accepted and refused. It can,
// where a rollback cannot: update_task_request_rollback() never returns on
// acceptance, but update_task_reboot_allowed() is pure policy, so there is
// a real instant here between deciding and resetting in which the reply can
// be queued and drained. See kilnlink_reboot_result.h's "SYMMETRIC" comment.
//
// Order matters and is the whole design: decide, LOG, send the reply, drain
// the TX ring, and only then reset. Anything after update_task_reboot_now()
// would never run.
static void link_task_handle_reboot(const kilnlink_frame_t *frame)
{
    kilnlink_reboot_t msg;
    kilnlink_reboot_status_t dstatus = kilnlink_reboot_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_REBOOT_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input,
        // discarded silently like every other decode failure in this file.
        return;
    }

    const char *reason = NULL;
    uint8_t reason_code = KILNLINK_REBOOT_RESULT_REASON_UNKNOWN;
    bool allowed = update_task_reboot_allowed(&reason, &reason_code);

    // Logged BEFORE the reset below, for the same reason
    // link_task_handle_rollback() logs before its own call: nothing after
    // update_task_reboot_now() runs, so this is the last chance to record
    // the decision in THIS boot's log.
    log_task_log(LOG_LEVEL_WARN, "reboot", reason ? reason : (allowed ? "ok" : "refused"));

    kilnlink_reboot_result_t result = {0};
    result.accepted = allowed ? 1u : 0u;
    result.reason = allowed ? (uint8_t)KILNLINK_REBOOT_RESULT_REASON_NONE : reason_code;

    uint8_t payload[KILNLINK_REBOOT_RESULT_LEN];
    kilnlink_reboot_result_status_t estatus;
    size_t len = kilnlink_reboot_result_encode(&result, payload, sizeof(payload), &estatus);
    if (len == 0) {
        // Shouldn't happen for a fixed-size local buffer. Refuse to reboot
        // rather than reset a board whose operator will never learn whether
        // the command landed -- the ESP's own bounded wait then reports
        // NO_REPLY, which is honest. Never reboot silently.
        return;
    }
    bool sent = link_task_send_broadcast(payload, (uint8_t)len);

    if (!allowed) {
        return; // refused: the reply above is the entire outcome
    }

    if (!sent) {
        // The TX ring had no room (uart_owner_send() drops whole frames, it
        // never partially writes -- LINK_PROTOCOL.md sec 2 rule 3). The
        // reboot was accepted by policy, so it still happens; the ESP will
        // simply see NO_REPLY and report the Pico's reboot as unconfirmed
        // rather than as accepted. Recorded so a bench log can tell this
        // case apart from a peer that was never listening.
        log_task_log(LOG_LEVEL_WARN, "reboot",
                     "REBOOT_RESULT was dropped by a full TX ring -- rebooting anyway, "
                     "the ESP will report the safety processor's reboot as unconfirmed");
    }

    // Let the reply actually leave the wire before the chip resets. Bounded
    // and short -- see LINK_TASK_REBOOT_TX_DRAIN_MS's own comment for the
    // baud arithmetic and the watchdog-deadline ceiling. Exits early the
    // moment the ring is empty; a ring that never empties (a wedged ISR)
    // must not block the reboot forever, so the bound is a hard cap, not a
    // retry loop.
    for (unsigned waited = 0; waited < LINK_TASK_REBOOT_TX_DRAIN_MS; waited++) {
        if (uart_owner_get_tx_head() == uart_owner_get_tx_tail()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    update_task_reboot_now(); // does not return
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

// SAFETY_CMD_INJECT_TC (0x21), CommonFW/docs/LINK_PROTOCOL.md section 4 --
// item 5 of the safety-TC-not-installed pass: feeds a synthetic reading into
// thermo_task.c so S1/S5/S11/S12 can be exercised on real hardware before
// the physical safety MAX31856 exists. Deliberately thin: ALL gating
// (accepted only while safety_tc_installed == 0, never persisted) lives in
// thermo_task_inject_reading() itself (thermo_task.h's own doc comment has
// the full argument for why that makes the gate structural rather than
// advisory) -- this handler does not duplicate that check, the same "the
// codec/handler split does not own the policy" division kilnlink_clear_
// trip.h documents for its own refusal logic. If a caller ever forgets to
// close this off in some other reachable path, thermo_task_inject_reading()
// still refuses on its own.
static void link_task_handle_inject_tc(const kilnlink_frame_t *frame)
{
    kilnlink_inject_tc_t msg;
    kilnlink_inject_tc_status_t dstatus =
        kilnlink_inject_tc_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_INJECT_TC_OK) {
        // Malformed/wrong-length/wrong-cmd -- untrusted wire input, discarded
        // silently like every other decode failure in this file.
        return;
    }

    bool accepted = thermo_task_inject_reading(msg.valid != 0u, msg.tc_c, msg.cj_c, msg.fault_bits);
    if (!accepted) {
        log_task_log(LOG_LEVEL_WARN, "inject_tc",
                     "refused -- safety_tc_installed != 0 (a real sensor is expected)");
        return;
    }
    log_task_log(LOG_LEVEL_WARN, "inject_tc",
                 "accepted -- reporting a SYNTHETIC reading, not the real MAX31856");
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

    // Captured BEFORE this commit's staged changes are finalized/written --
    // config_params_finalize_i_normal_a_invalidation() below needs the
    // effective CT channel map as it stood BEFORE this commit, to detect a
    // topology/channel remap that must invalidate i_normal_a[]. s_staged_
    // config already carries this commit's own SET_PARAM changes, so it
    // cannot serve as "before"; the last record actually committed can.
    config_store_record_t before_commit;
    config_store_get_full_record(&before_commit);

    config_store_record_t to_write = s_staged_config;
    config_params_finalize_ct_channel_map(&to_write);
    config_params_finalize_zone_ct_channel(&to_write); // CT_CHANNEL_MASK_PLAN.md step 2
    // Must run AFTER finalize_zone_ct_channel() (so to_write's group bit is
    // settled) and BEFORE finalize_i_present_a() (so an invalidated zone
    // cannot still win that function's smallest-normal search). Closes the
    // CT-commissioning HIGH finding: a topology/channel-map change must not
    // leave a stale per-zone i_normal_a feeding S14/S15.
    config_params_finalize_i_normal_a_invalidation(&before_commit, &to_write);
    config_params_finalize_i_present_a(&to_write); // CT_COMMISSIONING_PLAN.md step 3
    to_write.calibration_missing = !config_params_all_required_set(&to_write);

    uint8_t prev_tc_type = config_store_get_tc_type(); // captured BEFORE the write, see tc_type_reapply_policy.h
    const char *reason = NULL;
    config_store_write_decision_t decision = CONFIG_STORE_WRITE_OK;
    // 2026-09-15 Opus re-review N2: see link_task_handle_set_config()'s
    // matching comment -- cleared on every exit path below.
    safety_core_set_tc_type_apply_in_progress(true);
    bool written = config_store_write_ex(&to_write, link_task_heat_is_safe_for_tc_type_change(), &reason,
                                          &decision);
    if (written) {
        s_staged_config = to_write; // becomes the new baseline for the next SET_PARAM
        log_task_log(LOG_LEVEL_INFO, "commit_config", "accepted");
        // Take effect immediately, not after a reboot -- same reasoning as
        // link_task_handle_set_ct_cal()'s own call to current_task_reload_
        // ct_cal() just above. COMMIT_CONFIG is the only wire path that can
        // change i_present_a/zero_counts/k_ct_v_per_a/gain/mains_voltage_v
        // (SET_PARAM stages them into s_staged_config; this is where they
        // actually land in config_store), so this is the one call site that
        // needs current_task_reload_cal() -- current_task_fn()'s own boot
        // sequence is the only other caller.
        current_task_reload_cal();
        // 2026-09-15 review (F2, HIGH): this is the path the owner's
        // commissioning web page actually uses to set tc_type
        // (safety_commissioning_page.html field 261 -> SET_PARAM + this
        // COMMIT_CONFIG) -- it previously never called the live reapply at
        // all, so a type change from the UI took effect only at the next
        // boot. Same fail-safe reasoning as link_task_handle_set_config()'s
        // own call above.
        // 2026-09-15 Opus review G2: same immediate re-check as link_task_
        // handle_set_config()'s own call above -- see that comment for why
        // the flash write's own duration makes this necessary, not redundant.
        if (tc_type_reapply_policy_should_reapply(prev_tc_type, to_write.tc_type)) {
            if (link_task_heat_is_safe_for_tc_type_change()) {
                thermo_task_request_tc_type_reapply();
            } else {
                // 2026-09-15 Opus re-review N2: see link_task_handle_set_
                // config()'s matching comment -- same diverged-flash-vs-chip
                // condition, same retry-until-verified fix.
                log_task_log(LOG_LEVEL_ERROR, "commit_config",
                             "DIVERGED: tc_type committed to flash but chip reapply skipped "
                             "(heat enabled) -- will retry");
                s_tc_type_reapply_pending = true;
                s_tc_type_reapply_pending_value = to_write.tc_type;
            }
        }
    } else {
        log_task_log(LOG_LEVEL_WARN, "commit_config", reason ? reason : "refused");
        // Not field-specific -- always carries the NO_PARAM_ID sentinel.
        // 2026-09-15 (Opus re-review N3): map the REAL config_store_write_
        // decision_t `decision` the write computed internally, not a
        // strcmp() against one specific reason string -- the old string
        // match caught only the plain ARMED case and silently collapsed
        // both HEAT_ON and HEAT_UNKNOWN (and any future new reason) into
        // the generic STORAGE bucket, hiding the real cause from the page.
        // 2026-09-15 (Opus adversarial re-review of d43e96b2, defect 1):
        // the mapping moved wholesale into link_task_commit_reject.c so it
        // is host-testable (test_link_task_commit_reject.c) -- it was
        // previously an untestable switch inside this FreeRTOS/pico-sdk
        // translation unit, which is why F2's original form shipped with a
        // wrong input and no test.
        //
        // The MIXED classification is fed the PERSISTED tc_type, NOT
        // `prev_tc_type` (which is config_store_get_tc_type(), the RAM
        // cache a prior config_store_write_volatile() install can have
        // moved off flash truth). config_store_write_ex() built its own
        // MIXED log sentence by comparing s_persisted_record.tc_type
        // against rec->tc_type; feeding this mapping anything else lets
        // the wire reason and that log line contradict each other for one
        // single refusal. `prev_tc_type` stays correct for the reapply
        // policy above, which genuinely wants the live/cached value.
        kilnlink_commit_config_reject_reason_t wire_reason =
            link_task_commit_config_reject_reason_for(decision, config_store_get_persisted_tc_type(),
                                                       to_write.tc_type);
        link_task_send_commit_config_rejected(CONFIG_PARAMS_NO_PARAM_ID, wire_reason);
    }
    safety_core_set_tc_type_apply_in_progress(false);
}

// SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D), KILN_PROFILES_PLAN.md item 15 --
// the RAM-only sibling of link_task_handle_commit_config() just above.
// Deliberately mirrors that function's validation steps 1-3 EXACTLY (same
// config_params_validate_ex(), same finalize_ct_channel_map()/
// finalize_i_present_a(), same calibration_missing recomputation) -- a
// volatile install is not a less-checked install, it is the same checked
// install with config_store_write_volatile() in place of config_store_
// write() as the last step. config_store_write_volatile() never calls
// config_store_decide_write() -- the Pico never has to leave ARMED to
// accept an ordinary kiln-package swap via this frame, which is the entire
// reason this sibling command exists instead of a flag that would have to
// thread an ARMED-bypass through config_store_write() itself.
//
// CORRECTION (2026-09-14 review, Finding A): this comment used to say "there
// is no ARMED refusal branch here at all" -- that was true of the flash-
// stall gate, but config_store_write_volatile() now DOES refuse a narrow
// class of installs while ARMED (raising/clearing abs_max_temp_c or
// max_rate_c_per_min, or any tc_type change -- see that function's own doc
// comment in config_store.h). This handler now has to check its return
// value for exactly that reason, reported on the existing SAFETY_CMD_
// COMMIT_CONFIG_REJECTED frame with KILNLINK_COMMIT_CONFIG_REJECT_ARMED --
// no new wire value, no protocol bump: that reason already existed for
// COMMIT_CONFIG and was simply unreachable via this path until now.
static void link_task_handle_apply_config_volatile(const kilnlink_frame_t *frame)
{
    kilnlink_apply_config_volatile_t msg;
    kilnlink_apply_config_volatile_status_t dstatus =
        kilnlink_apply_config_volatile_decode(frame->payload, frame->length, &msg);
    if (dstatus != KILNLINK_APPLY_CONFIG_VOLATILE_OK) {
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
        log_task_log(LOG_LEVEL_WARN, "apply_config_volatile", rule ? rule : "refused, validation failed");
        kilnlink_commit_config_reject_reason_t wire_reason =
            (validate_reason == CONFIG_PARAMS_REJECT_CONTRADICTION) ? KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION
            : (validate_reason == CONFIG_PARAMS_REJECT_RANGE)       ? KILNLINK_COMMIT_CONFIG_REJECT_RANGE
                                                                     : KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN;
        link_task_send_commit_config_rejected(config_params_id_for_field_name(field), wire_reason);
        return; // writes NOTHING -- s_staged_config is untouched by validate_ex()
    }

    // See link_task_handle_commit_config()'s matching comment: this is the
    // record actually installed (flash or a prior volatile install) before
    // THIS commit's staged changes are applied, needed by config_params_
    // finalize_i_normal_a_invalidation() below to detect a topology/channel
    // remap.
    config_store_record_t before_commit;
    config_store_get_full_record(&before_commit);

    config_store_record_t to_write = s_staged_config;
    config_params_finalize_ct_channel_map(&to_write);
    config_params_finalize_zone_ct_channel(&to_write); // CT_CHANNEL_MASK_PLAN.md step 2
    // Same ordering requirement as link_task_handle_commit_config(): after
    // finalize_zone_ct_channel(), before finalize_i_present_a(). Closes the
    // CT-commissioning HIGH finding for the volatile-install path too --
    // APPLY_CONFIG_VOLATILE can change ct_topology/zone_ct_channel exactly
    // like COMMIT_CONFIG can.
    config_params_finalize_i_normal_a_invalidation(&before_commit, &to_write);
    config_params_finalize_i_present_a(&to_write);
    to_write.calibration_missing = !config_params_all_required_set(&to_write);

    // config_store_write_volatile() can still fail for exactly one reason
    // now (Finding A's ARMED-loosening carve-out): it cannot fail flash I/O
    // (never touches flash), so any refusal here is that carve-out, never a
    // storage failure -- report it the same way COMMIT_CONFIG's ARMED
    // refusal is reported.
    uint8_t prev_tc_type = config_store_get_tc_type(); // captured BEFORE the write, see tc_type_reapply_policy.h
    const char *reason = NULL;
    if (!config_store_write_volatile(&to_write, &reason)) {
        log_task_log(LOG_LEVEL_WARN, "apply_config_volatile",
                     reason ? reason : "refused: would loosen a safety threshold while ARMED");
        link_task_send_commit_config_rejected(CONFIG_PARAMS_NO_PARAM_ID, KILNLINK_COMMIT_CONFIG_REJECT_ARMED);
        return; // writes NOTHING -- s_staged_config is untouched
    }
    s_staged_config = to_write; // becomes the new baseline for the next SET_PARAM
    log_task_log(LOG_LEVEL_INFO, "apply_config_volatile", "accepted (volatile, no flash write)");
    // Same "take effect immediately" reasoning as link_task_handle_commit_
    // config()'s own call: CT cal / i_present_a etc. must be live the moment
    // this returns, not after a reboot.
    current_task_reload_cal();
    // 2026-09-15 review (F4, MEDIUM): a volatile install landing a new
    // tc_type into the RAM cache without this call left the MAX31856
    // configured for the OLD type while max31856_tc_type_verified() stayed
    // true -- "plausible and wrong" readings linearized under the wrong
    // curve, exactly what that flag exists to prevent. Same policy gate and
    // fail-safe reasoning as the other two config-write handlers above.
    if (tc_type_reapply_policy_should_reapply(prev_tc_type, to_write.tc_type)) {
        thermo_task_request_tc_type_reapply();
    }
}

// SAFETY_CMD_PARAM (0x1E) reply -- sent in answer to SAFETY_CMD_GET_PARAM
// (0x23, its own id since KILNLINK_PROTOCOL_VERSION 7; link_task_handle_
// get_param() below). Reports the
// currently COMMITTED record (config_store_get_full_record()), never the
// in-progress staged one -- CONFIG_REFERENCE.md section 7's "which
// thresholds is the safety processor actually enforcing" must be answerable
// from what is enforced, not from an uncommitted edit in flight.
// GET-only diagnostic, id 0x0505 (0x0501-0x0504 are the section-5 config
// params in config_params.c's CONFIG_PARAM_TABLE[] -- see that file). This
// one is NOT in that table and never will be: it reports a runtime counter
// (config_store_get_ram_integrity_fail_count()), not a config_store_
// record_t field, so config_params_get()'s pure `(rec, id) -> value`
// signature has no way to answer it -- special-cased here, before that
// call, instead. Deliberately un-enumerable (absent from CONFIG_PARAM_
// TABLE[] means it never appears under GET_CONFIG_PAGE) and automatically
// refused by config_params_set() (no SET case exists for it) -- a monotonic
// fault counter has no business being settable. Clamped to UINT16_MAX
// (kilnlink's U16 param type) rather than silently wrapping if it somehow
// exceeds that in one boot's lifetime.
#define LINK_TASK_PARAM_ID_RAM_INTEGRITY_FAIL_COUNT 0x0505u

static void link_task_send_param(uint16_t param_id)
{
    if (param_id == LINK_TASK_PARAM_ID_RAM_INTEGRITY_FAIL_COUNT) {
        uint32_t count = config_store_get_ram_integrity_fail_count();
        kilnlink_param_t reply;
        reply.param_id = param_id;
        reply.found = 1u;
        reply.type = KILNLINK_PARAM_TYPE_U16;
        memset(&reply.value, 0, sizeof(reply.value));
        reply.value.u16_val = (count > 0xFFFFu) ? 0xFFFFu : (uint16_t)count;

        uint8_t payload[KILNLINK_PARAM_MAX_LEN];
        kilnlink_param_status_t status;
        size_t len = kilnlink_param_encode(&reply, payload, sizeof(payload), &status);
        if (len == 0) {
            return;
        }
        link_task_send_broadcast(payload, (uint8_t)len);
        return;
    }

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
// GET_CONFIG_PAGE (0x24, its own id since KILNLINK_PROTOCOL_VERSION 7;
// link_task_handle_get_config_page() below). Reports the
// currently COMMITTED record, same reasoning as link_task_send_param()
// above. kilnlink_config_page_pack() is stateless/greedy per call
// (kilnlink_config_page.h's own header comment: no server-side cursor to go
// stale across a lost/repeated request, LINK_PROTOCOL.md section 2), so this
// re-derives where `page_index` must resume EVERY call by replaying pack()
// over pages [0, page_index) against the full id list and discarding the
// bytes, keeping only how many entries each replayed page consumed.
static void link_task_send_config_page(uint8_t page_index)
{
    s_last_config_page_requested_index = page_index; // 2026-08-28 diagnostic

    config_store_record_t rec;
    config_store_get_full_record(&rec);

    // 72 is config_params.c's own compile-time upper bound on its id table
    // (config_params_table_fits_72) -- comfortably above its real size
    // today (65 after ct_installed/0x0109), checked there so this array can
    // never silently truncate. Change the two together, never one alone.
    kilnlink_config_page_entry_t all[72];
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
        // 2026-08-27 audit fix (commissioning-write defect d): this used to
        // leave `set` unpopulated (implicitly zero-initialized `all[64]` had
        // no `.set` field to zero at all -- kilnlink_config_page_entry_t
        // gained one in this same fix), which meant kilnlink_config_page_
        // pack() sent every field's compiled/staged value with NO way to say
        // "this one is still unset" -- config_params_is_set() is the ONE
        // place that actually knows (fields_set-gated ids only; every other
        // id is always set, see that function's own comment).
        all[i].set = config_params_is_set(&rec, id);
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
    // 2026-08-23 call-path diagnostic, checkpoint 2 -- the FINAL pack call's
    // outcome (the one whose payload/len actually matter), recorded before
    // the early-return can act on it.
    s_config_page_encode_len = (uint32_t)len;
    s_config_page_encode_status = (uint8_t)status;
    // offset 1 = page_index per kilnlink_config_page.h's wire layout comment
    s_last_config_page_reply_index = (len > 1) ? payload[1] : 0xFFu; // 2026-08-28 diagnostic
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
    case LINK_FRAME_REBOOT_CMD:
        link_task_handle_reboot(&frame);
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
        // Own id (0x22) since KILNLINK_PROTOCOL_VERSION 7 -- no longer
        // shared with the reply (SAFETY_CMD_CT_CAL, 0x1A). The length check
        // is kept as a cheap sanity gate (kilnlink_get_ct_cal_decode()
        // enforces it too either way): the ESP's request is exactly 1 byte,
        // no arguments -- same convention as LINK_FRAME_FW_VERSION_CMD above.
        if (frame.length == 1) {
            link_task_handle_get_ct_cal(&frame);
        }
        break;
    case LINK_FRAME_GET_STACK_MARGIN_CMD:
        // Own id (0x2B), KILNLINK_PROTOCOL_VERSION 13. The ESP's request is
        // exactly 1 byte, no arguments -- same convention as
        // LINK_FRAME_GET_CT_CAL_CMD above.
        if (frame.length == 1) {
            link_task_handle_get_stack_margin(&frame);
        }
        break;
    case LINK_FRAME_CT_AUTO_ZERO_BEGIN_CMD:
        link_task_handle_ct_auto_zero_begin(&frame);
        break;
    case LINK_FRAME_GET_CT_AUTO_ZERO_CMD:
        if (frame.length == 1) {
            link_task_handle_get_ct_auto_zero(&frame);
        }
        break;
    case KILNLINK_SET_LOG_LEVEL_CMD:
        link_task_handle_set_log_level(&frame);
        break;
    case KILNLINK_INJECT_TC_CMD:
        link_task_handle_inject_tc(&frame);
        break;
    case KILNLINK_SET_PARAM_CMD:
        link_task_handle_set_param(&frame);
        break;
    case KILNLINK_COMMIT_CONFIG_CMD:
        link_task_handle_commit_config(&frame);
        break;
    case KILNLINK_APPLY_CONFIG_VOLATILE_CMD:
        link_task_handle_apply_config_volatile(&frame);
        break;
    case KILNLINK_GET_PARAM_CMD:
        // Own id (0x23) since KILNLINK_PROTOCOL_VERSION 7 -- no longer
        // shared with the reply (SAFETY_CMD_PARAM, KILNLINK_PARAM_CMD,
        // 0x1E). The length check is kept as a cheap sanity gate
        // (kilnlink_get_param_decode() enforces it too either way): the
        // ESP's request is exactly KILNLINK_GET_PARAM_LEN (3) bytes.
        if (frame.length == KILNLINK_GET_PARAM_LEN) {
            link_task_handle_get_param(&frame);
        }
        break;
    case KILNLINK_GET_CONFIG_PAGE_CMD:
        // Own id (0x24) since KILNLINK_PROTOCOL_VERSION 7 -- no longer
        // shared with the reply (SAFETY_CMD_CONFIG_PAGE, KILNLINK_CONFIG_
        // PAGE_CMD, 0x1F).
        if (frame.length == KILNLINK_GET_CONFIG_PAGE_LEN) {
            // 2026-08-25 measurement, see s_page_reply_us_max's declaration:
            // brackets the handler so the ESP-observable service latency for
            // this one command can be read as a number instead of inferred.
            {
                uint32_t t0 = time_us_32();
                link_task_handle_get_config_page(&frame);
                uint32_t dt = time_us_32() - t0;
                s_page_reply_us_last = dt;
                if (dt > s_page_reply_us_max) {
                    s_page_reply_us_max = dt;
                }
                s_page_reply_samples++;
            }
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

// Called once per link_task_fn() loop iteration (~10ms, LINK_TASK_POLL_MS).
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
        // most one LINK_TASK_POLL_MS (~10ms) late. Documented here rather
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
    // restarted" without polling for it). Sent as the first copy of a burst
    // (see LINK_BOOT_FW_VERSION_REPEAT_COUNT's own comment above) -- the
    // remaining copies go out from inside the main loop below so this task
    // keeps checking in with the watchdog between them.
    link_task_send_fw_version();
    s_boot_fw_version_repeats_pending = LINK_BOOT_FW_VERSION_REPEAT_COUNT - 1u;
    s_last_boot_fw_version_tx_tick = xTaskGetTickCount();

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
        link_task_retry_pending_tc_type_reapply();
        link_task_config_check_poll(now);

        if (s_boot_fw_version_repeats_pending > 0 &&
            (now - s_last_boot_fw_version_tx_tick) >= pdMS_TO_TICKS(LINK_BOOT_FW_VERSION_REPEAT_PERIOD_MS)) {
            link_task_send_fw_version();
            s_boot_fw_version_repeats_pending--;
            s_last_boot_fw_version_tx_tick = now;
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
    s_peer_protocol_version = 0; // unknown until this boot's own ANNOUNCE_VERSION arrives
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

    s_boot_fw_version_repeats_pending = 0;
    s_last_boot_fw_version_tx_tick = 0;

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

    // s_broadcast_buf_lock -- see its own declaration comment. Same
    // fail-closed handling as s_context_lock: if the mutex can't be
    // created, refuse to start rather than let link_task_send_broadcast_to()
    // run unguarded later.
    s_broadcast_buf_lock = xSemaphoreCreateMutex();
    if (!s_broadcast_buf_lock) {
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
