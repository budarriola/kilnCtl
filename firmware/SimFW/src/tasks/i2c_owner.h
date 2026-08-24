// i2c_owner.h -- single owner of I2C0 and both on-board MCP23017 expanders
// (docs/DESIGN_NOTES.md section 4.1 task map): "All expander traffic; relay-sense
// debounced scan (5-10 ms), E-stop/DUT-power outputs." Covers both
// MCP23017s (section 3.7's allocation table); the optional PCA9685
// (section 3.7) is not wired by this pass. Single-owner-per-peripheral
// doctrine, DESIGN_NOTES.md section 4's opening paragraph: no other task file may
// touch I2C0, its GPIO (SDA/SCL), or either mcp23017_t handle -- everything
// outside this .c file goes through the queue-based command API and the
// mutex-guarded snapshot/edge-log readers declared below, exactly the same
// shape ../SaftyFW's owner tasks use for their own owned peripherals (e.g.
// thermo_task being the only caller of max31856_read()/_configure()).
//
// Pin mapping (docs/HARDWARE.md section 3.7, docs/BOM.md section 6):
//   MCP23017 #1 (0x20): GPA0-4 = relay sense K1/K2/K3/K5/K4 (in),
//     GPA5 = `Fault` line sense (in), GPA6 = E-stop loop direct drive
//     (output-low = closed / input = open) -- see i2c_owner_set_estop()'s
//     comment below; there is no longer a switching element between GPA6
//     and SaftyFW's GPIO9 besides a 1 kOhm series protection resistor,
//     GPA7 = DUT 12V power relay #1 / main domain / J18 (out),
//     GPB0/GPB1 = J20 IO_3/IO_4 (i/o, default in+pullup),
//     GPB2 = DUT 12V power relay #2 / safety domain / J19 (out) --
//     resolved 2026-08-20 (docs/BOM.md section 6): two independent relays,
//     not one shared feed, so each domain can be power-cycled/browned-out
//     independently of the other for scenario testing (docs/DESIGN_NOTES.md
//     section 3.5 -- rationale updated 2026-08-23: this is no longer about
//     avoiding a GND_Main/GND_Safty bond, since the fixture's ground is
//     commoned elsewhere anyway; the two relays earn their keep on
//     independent-domain test coverage alone). GPB3-7 = 5 spare (default
//     in+pullup).
//   MCP23017 #2 (0x21): all 16 pins spare/generic (default in+pullup) --
//     DESIGN_NOTES.md section 3.6's documented fallback (moving the main-side
//     `~FAULT` x3 lines here if Pico GPIO ever runs out) is speculative
//     future work and is deliberately NOT hard-wired here; i2c_owner_io_*()
//     below is generic enough that a later pass can adopt that fallback
//     without touching this file's pin-role table for exp1.
//
// I2C0 GPIO pins: PROVISIONAL. docs/DESIGN_NOTES.md section 3.6's pin budget table
// lists "I2C0 SDA/SCL | 2 | both MCP23017s, optional PCA9685" but does not
// assign specific GPIO numbers (no docs/HARDWARE.md traced pinout exists
// yet -- DESIGN_NOTES.md section 9 lists it as not-yet-written). This file picks
// GPIO4 (SDA) / GPIO5 (SCL), the RP2040's conventional I2C0 default pins,
// as a placeholder -- see I2C_OWNER_SDA_GPIO/I2C_OWNER_SCL_GPIO in
// i2c_owner.c. Confirm or correct against the real board once HARDWARE.md
// exists (PLAN.md section 14 bring-up step 2).
#ifndef SIMFW_TASKS_I2C_OWNER_H
#define SIMFW_TASKS_I2C_OWNER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates i2c_owner at SIMFW_PRIO_I2C_OWNER, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Also creates this module's
// internal command queue and snapshot mutex. Returns false if task/queue/
// mutex creation failed.
bool i2c_owner_start(void);

// --- Relay-sense / fault-line snapshot ------------------------------------
// One debounced reading of every signal MCP23017 #1's relay-sense scan
// covers. Filled by i2c_owner's scan loop only (docs/DESIGN_NOTES.md section 4.1);
// i2c_owner_get_relay_states() returns a mutex-protected copy, safe to call
// from any other task (e.g. the future cmd_task RELAY_GET_STATES handler,
// DESIGN_NOTES.md section 5).
typedef struct {
    bool     k1_closed; // relay sense inputs: true = contact sensed closed
    bool     k2_closed;
    bool     k3_closed;
    bool     k5_closed;
    bool     k4_closed;
    bool     fault_line_asserted; // ESP GPIO6 -> U1 opto, sensed
    uint64_t sample_time_us;      // time_us_64() at the last debounced update
    bool     valid;               // false until the first debounce settle
} i2c_owner_relay_states_t;

i2c_owner_relay_states_t i2c_owner_get_relay_states(void);

// --- Edge log (DESIGN_NOTES.md section 5.2 "RELAY_GET_EDGES") ---------------------
// Which signal an edge belongs to -- covers the same six debounced bits as
// i2c_owner_relay_states_t above (relay sense k1..k4 plus the fault line),
// named "relay edge" to match PLAN.md's RELAY_GET_EDGES command even though
// the fault line rides along in the same debounced scan and ring buffer.
typedef enum {
    I2C_OWNER_SIGNAL_K1 = 0,
    I2C_OWNER_SIGNAL_K2 = 1,
    I2C_OWNER_SIGNAL_K3 = 2,
    I2C_OWNER_SIGNAL_K5 = 3,
    I2C_OWNER_SIGNAL_K4 = 4,
    I2C_OWNER_SIGNAL_FAULT_LINE = 5,
} i2c_owner_signal_t;

typedef struct {
    uint32_t seq;      // monotonic, gap = report generator's loss detection (DESIGN_NOTES.md section 8.2)
    i2c_owner_signal_t signal;
    bool     level;    // new (post-edge) level
    uint64_t time_us;  // time_us_64() at the debounced confirmation -- wall clock, not sim clock
                        // (i2c_owner does not own the sim clock; a later pass that wires this
                        // into sim_engine's snapshot can translate, DESIGN_NOTES.md section 4.5)
} i2c_owner_relay_edge_t;

// Edge log ring buffer size. 64 entries: generous headroom for a burst of
// relay activity between two PC polls at the default 2 Hz telemetry rate
// (DESIGN_NOTES.md section 5.3) without wrapping, while staying tiny next to the 32
// KiB heap budget (FreeRTOSConfig.h) -- revisit if a scenario is found that
// legitimately produces more than 64 edges between polls.
#define I2C_OWNER_EDGE_LOG_CAPACITY 64u

// Copies up to max_out entries with seq > since_seq (0 = "from the oldest
// entry still in the ring") into out, oldest-first. Returns the number of
// entries copied. If more entries exist than fit in max_out, the caller's
// next call should pass the seq of the last entry it received -- if the
// ring has since wrapped past what the caller last saw, the returned run
// will start after a gap, which is exactly the loss condition DESIGN_NOTES.md
// section 8.2 says report generation must flag, not silently paper over.
size_t i2c_owner_get_relay_edges(i2c_owner_relay_edge_t *out, size_t max_out, uint32_t since_seq);

// --- Commanded outputs ------------------------------------------------------
// Both post a command to i2c_owner's internal queue and return immediately;
// the output is actually driven on i2c_owner's next scan tick (docs/DESIGN_NOTES.md
// section 4.1's ~8 ms cadence -- see mcp23017.h's MCP23017_DEBOUNCE_SCAN_MS),
// not synchronously with this call, because only i2c_owner's own task
// context may touch I2C0 (single-owner doctrine, file header above). Returns
// false if the command could not be queued (queue full); true means queued,
// not yet applied.
//
// open == true: E-stop loop is opened (tripped). open == false: loop closed
// (healthy) -- DESIGN_NOTES.md section 3.4: the fixture drives J1's loop
// directly from this GPA6 bit through a 1 kOhm series protection resistor,
// same treatment every other fixture signal gets (no switching element in
// between any more -- the CPC1017N optoMOS that used to sit here is removed,
// DESIGN_NOTES.md section 3.5/14, since crossing the ground boundary was its
// only real job and the fixture's ground is commoned with the DUT's now).
//
// This is a DIRECTION toggle, not a level write, because SaftyFW's GPIO9
// side is a fail-safe normally-closed loop with its own R10 1k pull-up to
// 3.3v_Safty (SaftyFW/docs/HARDWARE.md section 5): driving GPA6 high when
// "open" would fight R10 into a different supply rail instead of just
// releasing the line. So: open == false -> GPA6 configured as OUTPUT driving
// LOW (pulls the loop closed); open == true -> GPA6 configured as INPUT,
// i.e. high-Z, letting R10 pull GPIO9 (and this side of the resistor) high.
// See i2c_owner.c's init comment for the boot-time default (input/high-Z,
// matching both the MCP23017's own POR default and the board's fail-safe
// intent).
bool i2c_owner_set_estop(bool open);

// on == true: DUT 12V power relay closed (board powered). DESIGN_NOTES.md section
// 3.4's `power_blip` scenario support.
//
// Two independent relays, two independent setters -- docs/HARDWARE.md
// section 3.7 / docs/BOM.md section 6 (resolved 2026-08-20): the main board
// has two electrically separate 12V inputs (J18/main, J19/safety) with no
// shared copper downstream. Originally justified because a single relay
// switching both would bond GND_Main and GND_Safty through the shared
// return path; that rationale is superseded (docs/DESIGN_NOTES.md section
// 3.5, 2026-08-23) now that the fixture's ground is commoned elsewhere
// anyway. The two relays are kept regardless, for independent per-domain
// power-cycle/brownout testing -- a real capability a single shared relay
// could never produce. There is deliberately NO "set both" convenience
// call -- a caller that wants both domains powered must call both setters
// explicitly, so a scenario author (or reviewer) always sees two separate
// decisions, never one that happens as a side effect of a single call.
bool i2c_owner_set_dut_power_main(bool on);   // relay #1 -> J18 (GND_Main)
bool i2c_owner_set_dut_power_safety(bool on); // relay #2 -> J19 (GND_Safty)

// Deprecated alias for i2c_owner_set_dut_power_main() / _get_dut_power_main_on(),
// kept only for source/protocol backward compatibility with callers written
// before relay #2 existed (this is also SIMFW_CMD_IO_DUT_POWER_SET/GET's
// underlying call, docs/PROTOCOL.md section 5.5). Deliberately NOT redefined
// to mean "both relays": the old single command keeps its original,
// narrower meaning -- main domain only -- rather than growing new (and
// easy-to-miss) side effects on the safety domain. New
// callers should use the explicit *_main()/*_safety() names above; this
// alias may be removed once no caller depends on the unqualified name.
bool i2c_owner_set_dut_power(bool on);

// Last-commanded (not sensed -- there is no separate feedback line for
// either output per DESIGN_NOTES.md section 3.4) state, mutex-read like the relay
// snapshot above. Reflects the state after the most recent applied command,
// not necessarily the most recent queued one if several are in flight.
bool i2c_owner_get_estop_open(void);
bool i2c_owner_get_dut_power_main_on(void);
bool i2c_owner_get_dut_power_safety_on(void);
bool i2c_owner_get_dut_power_on(void); // deprecated alias for _main_on() -- see i2c_owner_set_dut_power()'s comment

// --- Generic expander I/O (DESIGN_NOTES.md section 5 IO group: IO_SET_DIR,
// IO_WRITE, IO_READ) -------------------------------------------------------
// Addresses any pin on either expander by (expander, pin 0..15: 0..7 = port
// A, 8..15 = port B) -- covers J20 IO_3/IO_4 and exp1's 6 spares (pins
// 8..15 on I2C_OWNER_EXP_1) and all 16 of exp2's pins generically
// (I2C_OWNER_EXP_2), per this pass's instruction to expose MCP23017 #2
// generically rather than hard-wire its documented-but-speculative `~FAULT`
// fallback role (DESIGN_NOTES.md section 3.6).
//
// Exp1 pins 0..7 (relay sense, fault-line sense, E-stop drive, DUT-power
// relay #1/main) and pin 10 (DUT-power relay #2/safety) are reserved for the
// fixed roles above and are rejected here (returns false) -- reconfiguring
// them through the generic path would let a test silently break the
// relay-sense scan or one of the three safety-relevant outputs. Pins 8/9
// (J20 IO_3/IO_4) are generic, not reserved.
typedef enum {
    I2C_OWNER_EXP_1 = 0, // 0x20
    I2C_OWNER_EXP_2 = 1, // 0x21
} i2c_owner_expander_t;

// Distinguishes i2c_owner_io_set_dir()/i2c_owner_io_write()'s two distinct
// failure causes -- both report the same plain `false` return, but a caller
// mapping to a protocol-level status (cmd_task.c's handle_io_set_dir()/
// handle_io_write(), SIMFW_CMD_STATUS_ERR_BAD_ARGS vs
// SIMFW_CMD_STATUS_ERR_BUSY) needs to tell them apart. This is a PARALLEL
// enum to i2c_owner_io_read_status_t above, not a reuse of it: read()'s
// third case (NO_SAMPLE -- "valid target, no scan sample yet") and these
// setters' third case (QUEUE_FULL -- "valid target, command queue is
// transiently full") are different failure classes needing different wire
// status codes (ERR_NO_SAMPLE vs ERR_BUSY), so they get different enum
// values here too.
typedef enum {
    I2C_OWNER_IO_SET_OK = 0,
    I2C_OWNER_IO_SET_BAD_ARGS,   // exp/pin out of range or reserved (permanent -- retry never helps)
    I2C_OWNER_IO_SET_QUEUE_FULL, // valid target, but the command queue is transiently full (retry likely helps)
} i2c_owner_io_set_status_t;

// Same queue-then-apply-next-tick contract as the estop/dut-power setters
// above. out_status (may be NULL) reports which of the two distinct false
// causes applied -- see i2c_owner_io_set_status_t above.
bool i2c_owner_io_set_dir(i2c_owner_expander_t exp, uint8_t pin, bool input, bool pullup,
                           i2c_owner_io_set_status_t *out_status);
bool i2c_owner_io_write(i2c_owner_expander_t exp, uint8_t pin, bool level,
                         i2c_owner_io_set_status_t *out_status);

// Distinguishes i2c_owner_io_read()'s two distinct failure causes -- both
// report the same plain `false` return, but a caller mapping to a protocol-
// level status (cmd_task.c's handle_io_read(), SIMFW_CMD_STATUS_ERR_BAD_ARGS
// vs SIMFW_CMD_STATUS_ERR_NO_SAMPLE) needs to tell them apart.
typedef enum {
    I2C_OWNER_IO_READ_OK = 0,
    I2C_OWNER_IO_READ_BAD_ARGS,  // exp/pin out of range or reserved, or level == NULL
    I2C_OWNER_IO_READ_NO_SAMPLE, // valid target, but no scan sample taken yet
} i2c_owner_io_read_status_t;

// Reads the raw (non-debounced) level last seen for this pin during
// i2c_owner's scan loop -- mutex-read, does not itself touch I2C0. Returns
// false if no sample has been taken yet (immediately after boot, or no
// expander has ever ACKed) or the arguments are out of range; out_status
// (may be NULL) reports which of those two applied -- see
// i2c_owner_io_read_status_t above.
bool i2c_owner_io_read(i2c_owner_expander_t exp, uint8_t pin, bool *level,
                        i2c_owner_io_read_status_t *out_status);

// --- Bus scan (SIMFW_CMD_IO_BUS_SCAN, bench incident pass 2026-08-24) ------
// See cmd_ids.h's comment on SIMFW_CMD_IO_BUS_SCAN for the motivating
// incident. Sweeps the 7-bit I2C0 address range
// [I2C_OWNER_BUS_SCAN_ADDR_MIN, I2C_OWNER_BUS_SCAN_ADDR_MAX] and reports
// which addresses ACKed a 1-byte probe write, alongside the two addresses
// this build is configured to use (MCP23017_ADDR_1/_2, i2c_owner.c).
#define I2C_OWNER_BUS_SCAN_ADDR_MIN 0x08u
#define I2C_OWNER_BUS_SCAN_ADDR_MAX 0x77u
#define I2C_OWNER_BUS_SCAN_ADDR_COUNT \
    (I2C_OWNER_BUS_SCAN_ADDR_MAX - I2C_OWNER_BUS_SCAN_ADDR_MIN + 1u) // 112
// 112 addresses / 8 bits-per-byte = 14 bytes exactly, no partial byte.
#define I2C_OWNER_BUS_SCAN_BITMAP_BYTES ((I2C_OWNER_BUS_SCAN_ADDR_COUNT + 7u) / 8u)

typedef struct {
    // bit n (n = 0..I2C_OWNER_BUS_SCAN_ADDR_COUNT-1) set = address
    // (I2C_OWNER_BUS_SCAN_ADDR_MIN + n) ACKed the probe write.
    uint8_t found_bitmap[I2C_OWNER_BUS_SCAN_BITMAP_BYTES];
    uint8_t configured_addr1; // MCP23017_ADDR_1, i2c_owner.c
    uint8_t configured_addr2; // MCP23017_ADDR_2, i2c_owner.c
} i2c_owner_bus_scan_result_t;

// Runs a full bus scan and blocks the CALLING task (cmd_task, not
// i2c_owner) until it completes -- deliberately synchronous, unlike the
// queue-then-apply-next-tick setters above, because the entire point of this
// command is a client getting an immediately useful answer in one round
// trip (see cmd_ids.h's comment); a fire-and-forget queue send would just
// push the "when is it ready" problem onto the client as a second poll.
// The actual I2C0 traffic still only ever happens on i2c_owner's own task
// (single-owner doctrine, this file's header comment) -- this function only
// enqueues the request and waits on a completion semaphore i2c_owner's task
// signals once its own scan finishes; see i2c_owner.c's
// perform_bus_scan()/apply_pending_commands() for where the actual probing
// happens and for the worst-case duration estimate.
//
// Returns false (and zeroes *out) if the scan could not be requested
// (i2c_owner not started / command queue full) or if i2c_owner did not
// signal completion within a generous 1-second timeout -- the latter should
// never happen in normal operation (the scan itself is a few ms, see
// i2c_owner.c) and exists only so a genuinely wedged i2c_owner task cannot
// hang cmd_task forever. out must not be NULL.
bool i2c_owner_bus_scan(i2c_owner_bus_scan_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_I2C_OWNER_H
