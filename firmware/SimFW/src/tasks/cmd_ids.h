// cmd_ids.h -- SimFW's own benchproto task/device/command-id table (docs/
// DESIGN_NOTES.md section 5's command-group table, section 5.1's task-registration
// model). This is SimFW-specific numbering layered on top of `benchproto`
// (firmware/CommonFW/include/benchproto/), exactly the split
// BENCHPROTO.md section 7 describes: "SimFW's own task table ... is defined
// by SimFW itself, not by this library." The authoritative human-readable
// copy of this table is docs/PROTOCOL.md -- keep the two in sync in the same
// commit, the same discipline task_priorities.h's header comment asks of
// docs/DESIGN_NOTES.md section 4.1.
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

// Command-group task ids (DESIGN_NOTES.md sec 5's table, sec 5.1: "Each command
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

// SYS group command ids (DESIGN_NOTES.md sec 5's row: PING, GET_VERSION, RESET_SIM,
// SET_TIMESCALE, SET_SEED, GET_CAPS). All six now have real handlers in
// cmd_task.c (gap-closure pass, docs/PROTOCOL.md section 4): RESET_SIM/
// SET_TIMESCALE/SET_SEED are thin decode-then-call wrappers over
// sim_engine.h's sim_engine_reset()/_set_timescale()/_set_seed(). GET_SIM_STATE
// is a new id this pass adds (not in DESIGN_NOTES.md sec 5's original sketch, same
// "first-class getter for what a client just set" reasoning cmd_ids.h
// already documents for IO_ESTOP_GET/IO_DUT_POWER_GET above) so a client can
// read back the seed/timescale it set (or that a fresh boot defaulted to)
// without depending on the TELEMETRY frame's own seed field.
#define SIMFW_CMD_SYS_PING          0x01u
#define SIMFW_CMD_SYS_GET_VERSION   0x02u
#define SIMFW_CMD_SYS_RESET_SIM     0x03u
#define SIMFW_CMD_SYS_SET_TIMESCALE 0x04u
#define SIMFW_CMD_SYS_SET_SEED      0x05u
#define SIMFW_CMD_SYS_GET_CAPS      0x06u
#define SIMFW_CMD_SYS_GET_SIM_STATE 0x07u
// REBOOT_BOOTLOADER (gap-closure pass, docs/BENCH_RUNBOOK.md's "flashing
// without BOOTSEL" work): drops the RP2040 into its ROM USB bootloader so a
// PC-side tool can reflash over the same USB port, without the user
// physically pressing BOOTSEL. Handled by cmd_task.c's
// handle_sys_reboot_bootloader(), which delegates the actual safe-state
// sequencing + reset to safe_reboot.h/.c (single shared implementation --
// the 1200-baud host-tooling convention in usb_owner.c's
// tud_cdc_line_coding_cb()/tud_cdc_line_state_cb() calls the exact same
// function, see that file's header comment). See safe_reboot.h's own header
// comment for the full safety rationale: this fixture drives the DUT's
// E-stop loop and two 12V power relays through I2C expanders that do NOT
// reset when the RP2040 does, so a naive reboot could leave the DUT powered
// with the E-stop loop reporting "healthy" and nothing running on the
// fixture to fix it.
//
// request: {u32 confirm} -- must equal SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC
// exactly, or the command is refused with ERR_BAD_ARGS. This is on top of
// benchproto's own CRC-16/addressing (a malformed or misrouted frame is
// already discarded well before cmd_task ever sees it) as defense in depth
// specifically for this one command: it is the only command in this whole
// table whose success path ends the current firmware session outright, so a
// bit-flip landing on a *different*, structurally similar command's cmd_id
// byte and happening to decode as this one is exactly the accidental-
// trigger case worth a second, explicit guard against.
//
// reply: {status} only -- and only ever sent on the REFUSAL path
// (safe_reboot_into_bootloader() returned false: the safe state could not be
// confirmed within its bounded timeout) or the bad-magic path. On the
// SUCCESS path there is no reply at all: reset_usb_boot() never returns, so
// cmd_task_fn()'s usb_owner_send_reply() call after cmd_task_dispatch()
// simply never executes. A client must not treat "no ACK ever arrived" as a
// failure for this one command -- see docs/PROTOCOL.md section 4 for the
// full wire spec.
#define SIMFW_CMD_SYS_REBOOT_BOOTLOADER 0x08u
#define SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC 0xB007B007u

// SESSION_RESET (bug-fix pass, "PC reconnect misclassified as duplicate
// traffic"): clears THIS side's benchproto dedup ring and usb_owner's
// per-task ACK cache for the requesting src_device -- LINK state only. See
// "LINK state only" below for exactly what that does and does not mean.
//
// The bug this exists to fix: kilnsim's BenchprotoLink restarts its own
// msg_index counter at 0 on every connect() (tools/PcTools/src/kilnsim/
// benchproto_codec.py, BenchprotoLink.__init__), while this firmware's
// dedup ring (benchproto_link_t, firmware/CommonFW/include/benchproto/
// benchproto_link.h) is initialized exactly ONCE, at usb_owner_start()
// (MCU boot) -- it has no idea a USB reconnect ever happened. So after a
// reconnect, the PC's first BENCHPROTO_DEDUP_DEPTH requests (same fixed
// src_device=SIMFW_DEVICE_HOST/src_task=0 identity, msg_index 0..3) can
// collide with leftover entries from the *previous* session at that same
// literal (src_device, src_task, msg_index) tuple. A colliding request is
// classified DUPLICATE_REACK, never reaches cmd_task, and usb_owner replies
// with whatever it last cached for that dst_task under
// usb_owner_resend_cached_ack() -- a stale, unrelated command's answer that
// still decodes cleanly, or nothing at all if that task's cache slot was
// never populated. Observed on real hardware as the first 2-3 commands
// after every reconnect either timing out or getting back a wrong answer.
//
// THE DEDUP-BYPASS HAZARD -- read this before touching the dispatch path:
// this command is itself just another benchproto DATA frame to SYS, so on
// an ordinary reconnect it is exactly as likely to collide with a stale
// ring entry as anything else -- the very state this command exists to
// clear could swallow it as a duplicate before it ever runs. usb_owner.c's
// usb_owner_handle_raw_frame() special-cases this one (dst_task, cmd_id)
// pair: it recognizes a SESSION_RESET frame by peeking at
// frame->payload[0] and calls benchproto_link_reset_device() +
// usb_owner_reset_ack_cache() UNCONDITIONALLY, BEFORE benchproto_link_on_frame()
// ever classifies the frame -- so by the time dedup classification runs,
// there is nothing left for this exact frame to collide with, regardless of
// what was in the ring a moment earlier. This is not a generic "skip dedup
// for cmd X" escape hatch: the side effect (wiping the *entire* device's
// dedup ring + ACK cache) is unconditional and total, so the check cannot be
// abused to sneak one specific duplicate frame past dedup while leaving
// dedup intact for everything else -- exercising this path always costs the
// caller its whole session's worth of dedup/ACK-cache state, which is
// exactly what a client asking for a session reset should expect. Every
// other command id still goes through benchproto_link_on_frame() completely
// unmodified.
//
// LINK state only -- do NOT confuse this with SIMFW_CMD_SYS_RESET_SIM
// above: RESET_SIM reinitializes the *simulation* (thermal state, from
// either T0 or the last-selected preset); SESSION_RESET touches NEITHER the
// simulation NOR any fixture I/O -- it never reaches sim_engine.h,
// fault_sched.h, i2c_owner.h, or wave_owner.h at all. It never changes sim
// time, seed, timescale, fault schedule/slots, relay outputs, DUT power, or
// E-stop state. A client reconnecting mid-run (e.g. a scenario runner that
// lost its USB connection and reopened the port) can send this safely
// without disturbing anything the run has done so far.
//
// Idempotent and safe to send at any time, any number of times: clearing an
// already-empty ring/cache is a no-op, and this command has no side effect
// beyond that clearing (handle_sys_session_reset(), cmd_task.c, only builds
// the ordinary {status} reply -- the actual reset already happened in
// usb_owner.c by the time cmd_task ever sees this frame). A client SHOULD
// send it once, as its very first command after every (re)connect, before
// even PING -- see kilnsim.link's _FramedSimLink.connect() -- so PING and
// everything after it are also protected from the same collision, not just
// this command itself.
//
// request: none. reply: {status} only (SIMFW_CMD_STATUS_OK always -- this
// command has no failure mode of its own to report). An older firmware
// build that predates this id answers ERR_NOT_IMPL via cmd_task_dispatch()'s
// ordinary fallthrough (SYS's group table simply has no entry for 0x09) --
// a client must tolerate that without failing its connect (see
// kilnsim.link's own handling).
#define SIMFW_CMD_SYS_SESSION_RESET 0x09u

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
// Added this pass (MODEL/TC/CT/RELAY/IO/FAULT real handlers, docs/PROTOCOL.md
// section 5): the command was well-formed and its target in range, but the
// owning task could not apply it right now -- its internal command queue was
// full (i2c_owner.h/wave_owner.h/sim_engine.h's "queue-then-apply-next-tick"
// setters all return false on this condition, indistinguishably from a
// genuinely out-of-range argument at the C API level) or an immediate action
// (fault_sched_fire_now() on a slot that is not ARMED) could not complete.
// Distinct from ERR_BAD_ARGS so a client can tell "try again" apart from
// "fix your request."
#define SIMFW_CMD_STATUS_ERR_BUSY         0x04u
// Added this pass (i2c_owner_io_read()'s three-way failure ambiguity --
// docs/PROTOCOL.md IO/READ, section 5.5): the command and its arguments
// were valid (target in range, out-params non-NULL), but the fixture has no
// sample to answer with yet. Typically means the hardware the read depends
// on has never responded -- no MCP23017 physically attached to J20, or the
// bus otherwise never ACKed -- or i2c_owner's scan loop simply hasn't
// completed a tick since boot (`s_exp1_raw_valid`/`s_exp2_raw_valid` still
// false). Distinct from ERR_BUSY: ERR_BUSY means a transient condition an
// immediate retry will likely clear (a full command queue); ERR_NO_SAMPLE
// means retrying alone will never help -- the expander has to actually
// start answering (or a scan tick has to run) first. Distinct from
// ERR_BAD_ARGS: ERR_BAD_ARGS means the caller's own request is wrong (an
// out-of-range or reserved exp/pin) and no amount of waiting or retrying
// will ever make it succeed.
#define SIMFW_CMD_STATUS_ERR_NO_SAMPLE    0x05u

// --- MODEL group command ids (SIMFW_TASK_ID_MODEL) -- docs/PROTOCOL.md
// section 5.1, backed by sim_engine.h's MODEL-group command surface. ---
#define SIMFW_CMD_MODEL_SET_ZONE_PARAMS 0x01u
#define SIMFW_CMD_MODEL_GET_ZONE_PARAMS 0x02u
#define SIMFW_CMD_MODEL_SET_AMBIENT     0x03u
#define SIMFW_CMD_MODEL_LOAD_PRESET     0x04u
#define SIMFW_CMD_MODEL_SET_TEMP        0x05u
#define SIMFW_CMD_MODEL_SET_TC_LAG      0x06u

// --- TC group command ids (SIMFW_TASK_ID_TC) -- docs/PROTOCOL.md section 5.2.
// TC_GET_MASTER_CONFIG (0x06) now has a real handler: spi_emu_a.h/
// spi_emu_b.h export coherent register-image and "has-the-master-ever-
// written" getters (spi_emu_a_get_reg_image()/spi_emu_a_channel_configured()
// and their b-bus twins), so cmd_task.c's handle_tc_get_master_config()
// reports CR0/CR1/MASK plus a configured flag without reaching into either
// task's private state. See PROTOCOL.md section 5.2 for the reply layout
// and the reg_image_valid busy/retry semantics shared with TC_GET_REGS. ---
#define SIMFW_CMD_TC_GET_REGS          0x01u
#define SIMFW_CMD_TC_FORCE_TEMP        0x02u
#define SIMFW_CMD_TC_SET_MODE          0x03u
#define SIMFW_CMD_TC_INJECT_FAULT      0x04u
#define SIMFW_CMD_TC_CLEAR_FAULT       0x05u
#define SIMFW_CMD_TC_GET_MASTER_CONFIG 0x06u

// --- CT group command ids (SIMFW_TASK_ID_CT) -- docs/PROTOCOL.md section 5.3,
// backed by wave_owner.h. CT_SET_PHASE is not in DESIGN_NOTES.md section 5's original
// sketch but wave_owner.h exposes ct_wave_set_phase() as a first-class public
// setter, so it is wired up too rather than left stranded. ---
#define SIMFW_CMD_CT_SET_MODE       0x01u
#define SIMFW_CMD_CT_SET_AMPS       0x02u
#define SIMFW_CMD_CT_SET_DISTORTION 0x03u
#define SIMFW_CMD_CT_GET_STATE      0x04u
#define SIMFW_CMD_CT_SET_PHASE      0x05u

// --- RELAY group command ids (SIMFW_TASK_ID_RELAY) -- docs/PROTOCOL.md
// section 5.4, backed by i2c_owner.h's relay-sense snapshot/edge-log readers.
// RELAY_SET_CONTACT_FAULT (DESIGN_NOTES.md section 5's sketch) is deliberately NOT
// allocated here: i2c_owner.h exposes no such setter (relay sense is
// read-only from this task's perspective by design -- a "welded contact" is
// modeled at sim_engine's duty-override level), so the FAULT group's
// WELDED_RELAY/STUCK_OPEN_RELAY fault types (FAULT_SCHEDULE) are the real
// path for that behavior; see PROTOCOL.md section 5.4. ---
#define SIMFW_CMD_RELAY_GET_STATES 0x01u
#define SIMFW_CMD_RELAY_GET_EDGES  0x02u

// --- IO group command ids (SIMFW_TASK_ID_IO) -- docs/PROTOCOL.md section 5.5,
// backed by i2c_owner.h's generic expander I/O plus its E-stop/DUT-power
// setters. DUT_POWER_SET is DESIGN_NOTES.md section 3.4's post-section-5 addition,
// added to this group per this pass's instructions. ESTOP_GET/DUT_POWER_GET
// are not in DESIGN_NOTES.md section 5's sketch but i2c_owner.h exposes
// i2c_owner_get_estop_open()/_get_dut_power_on() as first-class public
// getters, so they are wired up too (a client cannot otherwise read back
// what it last commanded). DUT_POWER_SAFETY_SET/GET (0x09/0x0A) are this
// pass's addition, backing relay #2 (docs/HARDWARE.md section 3.7 /
// docs/BOM.md section 6's two-independent-relays resolution) -- DUT_POWER_SET/
// GET (0x06/0x08) keep their original main-domain-only meaning rather than
// being redefined to address both relays; see this file's comment on those
// two ids. ---
#define SIMFW_CMD_IO_SET_DIR       0x01u
#define SIMFW_CMD_IO_WRITE         0x02u
#define SIMFW_CMD_IO_READ          0x03u
#define SIMFW_CMD_IO_ESTOP_SET     0x04u
#define SIMFW_CMD_IO_FAULT_LINE_GET 0x05u
// DUT_POWER_SET/GET (0x06/0x08): deprecated aliases for the main-domain
// relay only (i2c_owner_set/_get_dut_power_main_on()) -- kept exactly as-is
// for backward compatibility. Deliberately NOT redefined to mean "both
// relays": the two independent relays (docs/HARDWARE.md section 3.7 /
// docs/BOM.md section 6) exist for independent per-domain power-cycle/
// brownout testing, not to avoid bonding GND_Main/GND_Safty (that rationale
// is superseded, docs/DESIGN_NOTES.md section 3.5, 2026-08-23). Ganging
// them by default here would still be wrong -- it would make a two-domain
// decision on the caller's behalf implicitly. Use DUT_POWER_MAIN_SET/GET or
// DUT_POWER_SAFETY_SET/GET (0x09/0x0A) for explicit, single-domain control
// -- see docs/PROTOCOL.md section 5.5.
#define SIMFW_CMD_IO_DUT_POWER_SET 0x06u
#define SIMFW_CMD_IO_ESTOP_GET     0x07u
#define SIMFW_CMD_IO_DUT_POWER_GET 0x08u
#define SIMFW_CMD_IO_DUT_POWER_SAFETY_SET 0x09u
#define SIMFW_CMD_IO_DUT_POWER_SAFETY_GET 0x0Au

// --- FAULT group command ids (SIMFW_TASK_ID_FAULT) -- docs/PROTOCOL.md
// section 5.6, backed by fault_sched.h's schedule/cancel/fire_now/list API.
// SET_UNTIL_TRIGGER (gap-closure pass): FAULT_SCHEDULE's UNTIL_TRIGGER
// duration kind needs a second, full nested trigger that does not fit
// alongside everything else in one 128-byte frame -- see PROTOCOL.md section
// 5.6 for the two-frame design this id completes (FAULT_SCHEDULE parks
// everything but the release trigger, this command supplies it and performs
// the actual arm). ---
#define SIMFW_CMD_FAULT_SCHEDULE  0x01u
#define SIMFW_CMD_FAULT_CANCEL    0x02u
#define SIMFW_CMD_FAULT_LIST      0x03u
#define SIMFW_CMD_FAULT_FIRE_NOW  0x04u
#define SIMFW_CMD_FAULT_SET_UNTIL_TRIGGER 0x05u

// --- EVT group frame-kind byte (SIMFW_TASK_ID_EVT) -- docs/PROTOCOL.md
// section 6, backed by telemetry.c's real body (this pass). EVT is not a
// request/reply group (no SIMFW_CMD_EVT_* ids exist, or ever will -- see
// PROTOCOL.md section 6): it is unsolicited BROADCAST-only traffic, and
// telemetry.c sends two different frame shapes from the same
// SIMFW_TASK_ID_EVT source, disambiguated by this byte 0 of the payload
// (DESIGN_NOTES.md 5.3's "Telemetry frame" and "EVT frame" are two distinct
// layouts sharing one wire source task). Kept in its own block, separate
// from every SIMFW_CMD_* table above, since it is not a command id at all
// -- do not add it to any group's command dispatch table in cmd_task.c. ---
#define SIMFW_EVT_FRAME_KIND_TELEMETRY 0x01u /* periodic state frame, DESIGN_NOTES.md 5.3 */
#define SIMFW_EVT_FRAME_KIND_EVENT     0x02u /* one sim_snapshot.h sim_event_t, DESIGN_NOTES.md 5.3 */

// GET_CAPS's fixed capability numbers (DESIGN_NOTES.md sec 4.1's task map and sec
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
// One bit per command group now that MODEL/TC/CT/RELAY/IO/FAULT all have
// real handlers (this pass, docs/PROTOCOL.md section 5) -- "this group has a
// real implementation, not just a stub", per DESIGN_NOTES.md 5.1's own reasoning for
// GET_CAPS existing at all. TC's group is now fully implemented, including
// TC_GET_MASTER_CONFIG (PROTOCOL.md section 5.2). Bit ordering matches
// SIMFW_TASK_ID_* minus 2 (SYS has no bit -- "always fully present by
// construction," docs/PROTOCOL.md's existing GET_CAPS note) and EVT has no
// bit (not a request/reply group, section 6).
#define SIMFW_CAPS_FEATURE_BIT_MODEL  (1u << 0) /* SIMFW_TASK_ID_MODEL = 2 */
#define SIMFW_CAPS_FEATURE_BIT_TC     (1u << 1) /* SIMFW_TASK_ID_TC    = 3 */
#define SIMFW_CAPS_FEATURE_BIT_CT     (1u << 2) /* SIMFW_TASK_ID_CT    = 4 */
#define SIMFW_CAPS_FEATURE_BIT_RELAY  (1u << 3) /* SIMFW_TASK_ID_RELAY = 5 */
#define SIMFW_CAPS_FEATURE_BIT_IO     (1u << 4) /* SIMFW_TASK_ID_IO    = 6 */
#define SIMFW_CAPS_FEATURE_BIT_FAULT  (1u << 5) /* SIMFW_TASK_ID_FAULT = 7 */
#define SIMFW_CAPS_FEATURE_BITMASK    (SIMFW_CAPS_FEATURE_BIT_MODEL | SIMFW_CAPS_FEATURE_BIT_TC | \
                                         SIMFW_CAPS_FEATURE_BIT_CT | SIMFW_CAPS_FEATURE_BIT_RELAY | \
                                         SIMFW_CAPS_FEATURE_BIT_IO | SIMFW_CAPS_FEATURE_BIT_FAULT)

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_CMD_IDS_H
