// thermo_task.h -- reads the MAX31856 (Phase 3). Event-driven on ~DRDY
// (GPIO12), a real Pico IRQ per docs/ARCHITECTURE.md section 3 ("unlike the
// main board, this is a direct Pico GPIO, so do not poll it").
#ifndef SAFTYFW_TASKS_THERMO_TASK_H
#define SAFTYFW_TASKS_THERMO_TASK_H

#include <stdbool.h>

#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the ~DRDY (GPIO12) falling-edge IRQ and creates thermo_task at
// SAFTYFW_PRIO_THERMO_TASK, pinned to SAFTYFW_CORE_TRIP_PATH. main.c must
// have already brought up spi_owner and probed/configured the MAX31856
// (docs/ARCHITECTURE.md section 5, steps 5-6) before calling this -- a
// missing/dead part is not fatal (this task just keeps publishing invalid
// snapshots), but the interface has to exist first. Returns false if task
// creation failed.
bool thermo_task_start(void);

// Copies the most recently published snapshot into *out under a mutex (no
// torn reads). Returns false (and leaves *out zeroed/invalid) only if
// thermo_task has not published anything yet, e.g. called before
// thermo_task_start(). Callable from any task -- this is how safety_core
// pulls the reading, matching ARCHITECTURE.md section 2's "safety_core
// pulls, nothing pushes" rule.
bool thermo_task_get_snapshot(thermo_snapshot_t *out);

// --- TC value injection, bench dev switch (config_store.h's
// safety_tc_installed, param 0x0211) --------------------------------------
// Exercises the whole S1/S5/S11/S12 guard chain on real hardware before the
// physical safety MAX31856 exists, by substituting a synthetic reading for
// what thermo_task_get_snapshot() reports. Same "gated dev switch, reported
// in status, refused by default" shape as KilnFW's set_watchdog_panic_
// disabled (firmware/KilnFW/App/drivers/safety/watchdog_cfg.c) -- see that
// function for the precedent this follows.
//
// Gated STRUCTURALLY, not just at the call site that wires the wire
// command: thermo_task_inject_reading() itself re-reads config_store's
// current safety_tc_installed and refuses (returns false, changes nothing)
// unless it is 0. Because safety_core_request_enable() (safety_core.c)
// unconditionally refuses the ON direction whenever that SAME field is 0,
// injection can never be active in a state where heat is enable-able: the
// two checks read the identical config field, so there is no window where
// safety_tc_installed flips from 0 to 1 (a real sensor being commissioned)
// while an old injected reading is still being reported as live. See
// thermo_task.c's own comment on this re-check for exactly where that
// closes -- thermo_task_get_snapshot() re-verifies the same gate on every
// call, not just at injection time, so flipping the config field back to 1
// silently stops the override on the very next read, even if nobody
// explicitly cleared it first.
//
// NEVER persisted -- this is pure RAM state in thermo_task.c, with no
// config_store_write() anywhere in its path, so it is unconditionally
// cleared by a reboot, matching set_watchdog_panic_disabled's "reboot
// always returns to the safe compiled state" property in spirit (that one
// persists deliberately; this one deliberately does not, because unlike a
// watchdog setting a stale injected reading left active across a power
// cycle would be actively dangerous rather than merely inconvenient).
//
// `tc_valid == false` means "inject a bad read" (mirrors thermo_snapshot_t's
// own valid==false contract: tc_c/cj_c are ignored and reported as NaN) --
// this is how S5's own recovery-from-bad-reads path gets exercised on the
// bench. Returns false (nothing changed) if safety_tc_installed != 0.
bool thermo_task_inject_reading(bool tc_valid, float tc_c, float cj_c, uint8_t fault_bits);

// Deactivates injection, reverting thermo_task_get_snapshot() to the real
// (currently: always invalid, since the physical part is not wired up)
// MAX31856 conversion loop. Always succeeds, even if injection was already
// inactive -- same "de-escalation always reachable, no gate" shape every
// other safe-direction transition in this codebase gets.
void thermo_task_inject_clear(void);

// True iff an injected reading is CURRENTLY being substituted for the real
// one -- i.e. thermo_task_inject_reading() was last called successfully,
// thermo_task_inject_clear() has not run since, AND safety_tc_installed is
// still 0 right now (the same live re-check thermo_task_get_snapshot()
// itself does). Consumed by link_task.c's status-frame builder (item 4:
// KILNLINK_STATUS_FLAG_TC_INJECTED / LINK_FLAG_TC_INJECTED) so an injected
// reading is never mistaken for a real one downstream.
bool thermo_task_injection_active(void);

// --- Live tc_type reapply, SAFETY_CMD_SET_CONFIG (link_task.c) ------------
// Requests that thermo_task_fn() call max31856_configure(config_store_get_
// tc_type()) again on its own next loop iteration, picking up a tc_type that
// link_task_handle_set_config() just committed to config_store while this
// task was already running with the OLD type verified. Without this, a live
// SET_CONFIG tc_type change would sit inert until the next reboot -- the
// existing max31856_reconfig_retry.h cadence only re-attempts configure()
// while max31856_tc_type_verified() is ALREADY false, which an old,
// still-verified type never is.
//
// Deliberately just a request flag, not a direct max31856_configure() call
// from link_task.c's own task context: the MAX31856 SPI bus is owned by
// thermo_task_fn()'s loop alone (see max31856.c's module comment), so any
// reconfigure has to happen there, not from whichever task decoded the wire
// frame.
//
// Safe by construction, not merely by policy: max31856_configure() clears
// max31856_tc_type_verified() unconditionally at entry (see its own header
// comment) before it does anything else, so from the instant thermo_task_fn()
// picks this request up, every snapshot published from that point (including
// while the reconfigure/CR1-readback is in flight) is already reported
// invalid through the existing "!verified -> invalid" downgrade -- the exact
// same fail-safe path a boot-time reconfig retry already uses. Readings only
// become valid again once the readback confirms the NEW type. This never
// touches relay/arm state, so it cannot itself disarm the Pico; a reapply
// that leaves tc_type unverified simply reports the sensor invalid, which
// S5 already treats as a trip condition like any other dead/unplugged part.
void thermo_task_request_tc_type_reapply(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_THERMO_TASK_H
