// safe_reboot.h -- the one place that is allowed to call pico-sdk's
// reset_usb_boot() (pico/bootrom.h), so a USB-triggered reflash (either
// SIMFW_CMD_SYS_REBOOT_BOOTLOADER, cmd_task.c, or the 1200-baud host-tooling
// touch convention, usb_owner.c's tud_cdc_line_coding_cb()/
// tud_cdc_line_state_cb()) can never jump into the ROM bootloader without
// first driving the fixture to a CONFIRMED safe state.
//
// --- Why this matters more than a normal reboot ----------------------------
// This fixture drives the DUT's E-stop loop and both DUT 12V power relays
// through two MCP23017 I2C expanders (src/tasks/i2c_owner.c/.h). Dropping
// the RP2040 into its ROM bootloader does NOT reset those expanders --
// nothing in src/tasks/i2c_owner.c, src/drivers/mcp23017.{c,h}, or the
// board docs (docs/HARDWARE.md section 3.7 pin table, docs/BOM.md section 6)
// documents an MCP23017 hardware RESET pin tied to the Pico's RUN or to any
// Pico GPIO -- searched, and it simply is not there. Per this project's own
// safe-default doctrine (unwritten hazards are treated as present, not
// absent), that means: **the expanders keep whatever output state they had
// at the moment of reboot.** A reboot commanded while the E-stop loop is
// closed (GPA6 driving low -- "healthy") and a DUT power relay is on would
// leave the DUT powered, its E-stop loop reporting fine, with *nothing*
// running on the fixture's own RP2040 to change that until a new image is
// flashed and boots -- exactly the hazard a bench fixture must never create
// for itself.
//
// --- The sequence ------------------------------------------------------
// safe_reboot_into_bootloader() below:
//   1. Queues (i2c_owner.h/wave_owner.h's own queue-then-apply-next-tick
//      contract -- neither setter takes effect synchronously with this
//      call):
//        - i2c_owner_set_estop(true)              -- loop OPEN/STOP, the
//          fail-safe direction (i2c_owner.h's own doc comment: GPA6 goes to
//          INPUT/high-Z, letting SaftyFW's own R10 pull GPIO9 high).
//        - i2c_owner_set_dut_power_main(false)     -- relay #1 / J18 off.
//        - i2c_owner_set_dut_power_safety(false)   -- relay #2 / J19 off.
//        - ct_wave_set_amps(ch, 0.0f) for every CT channel, THEN
//          ct_wave_set_mode(ch, CT_WAVE_MODE_MANUAL) -- wave_owner.h's own
//          doc comment on ct_wave_set_amps(): "a pending MANUAL amps value
//          is still stored while in MODEL mode", so setting the target to 0
//          BEFORE switching to MANUAL means the channel goes silent the
//          instant MANUAL takes effect, never passing through "MANUAL at
//          some stale nonzero amps" even for one zero-crossing.
//   2. Polls (bounded: SAFE_REBOOT_TIMEOUT_MS total, SAFE_REBOOT_POLL_MS
//      between checks) until every one of those is CONFIRMED applied --
//      i2c_owner_get_estop_open()/_get_dut_power_main_on()/
//      _get_dut_power_safety_on() and ct_wave_get_state() all read back the
//      commanded values, not just "the setter returned true" (a queued
//      command can still be sitting in i2c_owner's/wave_owner's own command
//      queue, not yet applied to the physical GPIO/PIO state, when this
//      function is called -- confirming the *readback*, not the enqueue, is
//      the whole point).
//   3. Only once ALL of the above are confirmed does it call
//      reset_usb_boot(0, 0) (pico/bootrom.h) -- which never returns.
//
// --- Refuse, never reboot anyway --------------------------------------------
// If the timeout elapses without every condition confirmed, this function
// returns false and does NOT reboot. This is a deliberate choice, not a
// missing feature: the entire point of this sequence is that a reboot must
// never leave the fixture in an unconfirmed state. Rebooting anyway on a
// timeout would risk exactly the hazard this file exists to prevent (the DUT
// left powered, loop closed, with the one thing that could still be driving
// those outputs about to disappear into the bootloader). A refusal is
// recoverable -- the caller (cmd_task.c) reports SIMFW_CMD_STATUS_ERR_BUSY
// and the operator can retry, investigate the I2C bus, or fall back to
// physical BOOTSEL -- while a reboot into an unconfirmed unsafe state is
// not recoverable by software at all once the RP2040 is in the bootloader.
#ifndef SIMFW_TASKS_SAFE_REBOOT_H
#define SIMFW_TASKS_SAFE_REBOOT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Drives the fixture to the safe state described above, confirms it within
// a bounded timeout, and then reboots into the RP2040's ROM USB bootloader.
//
// Returns false if the safe state could not be confirmed within the
// timeout -- the fixture is left exactly as it was commanded (still trying
// to reach the safe state; i2c_owner/wave_owner keep applying the queued
// commands on their own next ticks regardless of this function's return),
// and no reboot happens. Never returns at all on success: reset_usb_boot()
// jumps straight into the ROM bootloader.
//
// Safe to call from any FreeRTOS task context that is allowed to block for
// up to SAFE_REBOOT_TIMEOUT_MS (cmd_task's own dispatch thread, or
// usb_owner's RX/TX task via the 1200-baud line-coding callback -- both are
// acceptable places to block briefly for a rare, user-initiated event like
// this one).
bool safe_reboot_into_bootloader(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SAFE_REBOOT_H
