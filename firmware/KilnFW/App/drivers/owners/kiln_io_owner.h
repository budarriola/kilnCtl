// kiln_io_owner -- the single task that ever writes the SX1509 expander
// (relays, digital IO, and the raw register passthrough), so the checks
// that decide whether a write is allowed live in exactly one place instead
// of being copied at every call site.
//
// 2026-08-19, TODO.md section 10.14 Phase 1. Filed after a research pass
// found FIVE independent callers writing relay/IO state through
// kiln_io_set_relay()/kiln_io_set_relay_mask() with no coordination between
// them: uart_bridge.c's io_bridge_task (UART), dashboard_http.c's
// dashboard_set_relay() (HTTP and, via ui_page_temperature.c, the LCD),
// profile_executor.c's apply_relay()/force_relay_mask_off()/
// sweep_unowned_relays() (the automatic PID/time-proportioning control
// loop), and autotune_engine.c's apply_relay(). Two problems, not one:
//   1. kiln_io_set_relay_mask() is a read-modify-write against the
//      expander's data register. SX1509.c's own internal mutex
//      (SX1509Class::lock) serializes each CALL's I2C transaction, but does
//      NOT stop two independent calls from racing on a stale read -- a
//      lost-update on the code path that energizes mains contactors.
//   2. The "is this relay owned by a running profile" /
//      "would this turn a relay on while a safety fault is asserted" checks
//      were independently reimplemented in uart_bridge.c and
//      dashboard_http.c (relay_authority_manual_blocked_by_owner()/
//      relay_authority_on_blocked(), called from two places that can drift)
//      while profile_executor.c/autotune_engine.c apply their own
//      zone-level equivalent (relay_authority_zone_blocked()) before
//      calling in -- correct today, but nothing stopped a manual write from
//      landing between that check and the eventual I2C transfer.
//
// Every caller above becomes a producer into this module's queue instead.
// Two producer families, matching this codebase's existing
// firmware/SaftyFW/src/tasks/relay_owner.c pattern:
//   - MANUAL (kiln_io_owner_command_set_relay[_mask]()): applies the same
//     ownership/safety-fault gate uart_bridge.c and dashboard_http.c used
//     to apply independently, now in exactly one place. Used by the UART
//     bridge and dashboard_set_relay() (HTTP + LCD).
//   - AUTHORIZED (kiln_io_owner_command_set_relay_mask_authorized()): no
//     ownership check (the caller -- profile_executor.c/autotune_engine.c
//     -- already IS the owner of the relays it names, via
//     relay_authority_zone_blocked()'s zone-level gate, checked by the
//     caller before this is called) and no additional safety-fault check
//     (same reason: the caller already applied its own). This function
//     exists only so the actual I2C write is serialized against the MANUAL
//     writers above through the same queue -- it does not change who is
//     allowed to command what, only who is allowed to do the writing.
//
//     2026-08-28: uart_bridge.c's link-loss watchdog is a third caller, for
//     the same "no additional gate needed" reason but a different proof --
//     it isn't a relay owner, it computes `unowned_mask` (relays NOT under a
//     PROFILE/RULE/AUTOTUNE) and only ever pairs it with value=0, so there is
//     nothing left for an ownership or "turns something on" safety check to
//     catch. This one is NOT exempted from routing through the queue the way
//     kiln_io_all_relays_off() is (see the "explicitly NOT covered" note
//     below): unlike that unconditional all-relays/always-OFF call, this
//     write touches only a subset of the register, so a stale-read race with
//     owner_task can revert either side's bit -- exactly the read-modify-
//     write hazard this module exists to close. Confirmed safe to queue:
//     owner_task blocks only on its own command queue and the SX1509 I2C
//     mutex, never on the PC link or any bridge task, so it keeps running in
//     the exact "every bridge task is wedged" condition the watchdog exists
//     for.
//
// Every producer is a bounded, non-blocking POST (xQueueSend with 0 ticks)
// followed by a bounded WAIT on a per-call result -- callers here are the
// UART bridge task, the HTTP worker task, lvgl_port_task (a single I2C
// write's worth of latency, not a multi-second operation), and
// profile_executor's/autotune_engine's own control tasks, none of which are
// the owner task itself, so none of them can deadlock waiting on it.
//
// 2026-08-24: the per-call result/semaphore pair used to be stack-allocated
// in the calling producer's own frame (relay_owner.c's original shape).
// That was a lifetime bug: a producer that gave up after the bounded wait
// could return -- freeing/reusing that stack frame -- while the owner task
// was still going to write through the now-dangling pointers it had
// already been handed, a cross-task stack corruption reachable any time the
// owner task legitimately took longer than the wait (SX1509.c's own I2C
// lock timeout is 6000ms, 30x the 200ms wait). Fixed by moving both the
// result storage and the semaphore into a small, fixed, MODULE-owned pool
// (kiln_io_owner.c's `s_slots[]`) instead -- see owner_slot_pool.h for the
// two-sided release protocol that makes it safe for a producer to give up
// at any point without ever corrupting memory, and kiln_io_owner.c's
// KILN_IO_OWNER_WAIT_MS comment for why the wait itself stayed at 200ms.
//
// Explicitly NOT covered: kiln_io_lcd_dc()/kiln_io_lcd_reset(). Those are
// ILI9488.c's own hot path, called once per display command from
// lvgl_port_task, and SX1509.c's internal mutex already makes them safe to
// interleave with everything above at the I2C-transaction level -- routing
// them through this queue too would add a task hop to the single most
// latency-sensitive call in the display driver for no correctness benefit.
//
// Also explicitly NOT covered, and deliberately so: every DIRECT
// kiln_io_all_relays_off() call in the codebase that exists specifically as
// a last-resort fail-safe, independent of everything else --
// uart_bridge.c's link-loss watchdog (uart_bridge_start_link_watchdog(),
// "must still run when every bridge task is blocked"),
// profile_executor.c's own watchdog_task_entry() (guard 9 / the safety-link
// 30s-silence abort, "must still run if the main control task is stuck"),
// and main.c's kiln_enter_safe_state() (the shutdown/panic path). Routing
// any of these through this module's queue would make them depend on the
// owner task NOT being the thing that's wedged -- exactly backwards for
// code whose entire purpose is acting when something else already is.
// kiln_io_all_relays_off() is unconditional and only ever turns things OFF,
// so a race between one of these and the owner task mid-write is benign in
// the failure direction: worst case is a redundant I2C transaction, never
// an unsafe state.
#ifndef KILN_IO_OWNER_H
#define KILN_IO_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "SX1509.h"
#include "kiln_io.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_IO_OWNER_RELAY_OK = 0,
    KILN_IO_OWNER_RELAY_ERR_RANGE,    /* relay index out of range */
    KILN_IO_OWNER_RELAY_ERR_OWNED,    /* refused: owned by a running profile */
    KILN_IO_OWNER_RELAY_ERR_SAFETY,   /* refused: a safety fault source is asserted */
    KILN_IO_OWNER_RELAY_ERR_IO_FAIL,  /* the expander write itself failed */
    KILN_IO_OWNER_RELAY_ERR_TIMEOUT,  /* owner task did not answer in time -- see
                                        * kiln_io_owner_start()'s doc comment; treat
                                        * exactly like ERR_IO_FAIL, fail closed */
    /* 2026-08-21: refused because an ESP or Pico firmware update is in
     * progress (the OTA/heating mutual interlock -- relay_on_blocked()'s
     * ota_http_heat_blocked_by_update() half in kiln_io_owner.c). Until now
     * this case was reported as ERR_SAFETY above, which reads as "something
     * is FAULTED" to an operator when nothing is: no SAFETY_FAULT_SRC_* bit
     * (safety_link.h) is set, an update is simply running. See
     * relay_on_blocked()'s own comment in kiln_io_owner.c for the fix's full
     * history and for why the specific reason string was already being
     * logged server-side but never reaching dashboard_http.c/uart_bridge.c's
     * callers.
     *
     * APPENDED here rather than inserted after ERR_SAFETY where it would read
     * more naturally: this is a plain C enum with no explicit `= N` values,
     * so every member below an insertion point shifts its numeric value.
     * Checked before choosing append over insert -- neither
     * kiln_io_owner_relay_result_t nor kiln_io_owner_sx_result_t crosses the
     * UART wire (uart_bridge.c's SET_RELAY/SET_RELAY_MASK responses carry
     * their own separate wire status byte, translated from this enum at each
     * call site rather than sending this enum's raw numeric value -- see
     * uart_bridge.c's dispatch for those two commands) and neither is
     * persisted to NVS anywhere in this codebase, so an insertion could not
     * actually have corrupted a stored or transmitted value here. Appending
     * anyway costs nothing and rules the question out for good rather than
     * relying on that "checked, currently true" fact staying true forever. */
    KILN_IO_OWNER_RELAY_ERR_UPDATING,
    /* 2026-09-15 (docs/audits/manual_relay_readiness_gating_options_2026-09-15.md,
     * option B): refused because an unacknowledged crash report exists
     * (crash_report_has_unacknowledged()) -- danger mode still bypasses this,
     * same as every other gate in relay_on_blocked(). Distinct from
     * ERR_SAFETY for the same reason ERR_UPDATING is: no SAFETY_FAULT_SRC_*
     * bit is set, so a caller must not translate this into "safety fault"
     * text. Appended, not inserted, for the same "this enum has no explicit
     * numeric values and crosses neither the UART wire nor NVS" reasoning as
     * ERR_UPDATING's own comment above. */
    KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK,
    /* docs/SYSTEM_MODE_GATE.md, owner decision 2026-09-25 (Q1): refused
     * because a firing or autotune run is currently active -- system_mode_
     * gate.h's SYS_ACTION_RAW_RELAY_DEBUG_WRITE, checked in relay_on_blocked()
     * below. BLANKET refusal, not scoped to relays the run actually claims
     * (the owner's explicit choice over the plan doc's own claimed-relays-
     * only recommendation) -- ANY manual relay-ON is refused while either
     * engine is running, on all three transports (HTTP's danger-mode relay
     * route, the UART bridge's SET_RELAY/SET_RELAY_MASK, and the LCD's
     * manual override) since all three already funnel through this one
     * choke point. Distinct from ERR_OWNED (relay_authority_manual_blocked_
     * by_owner(), a per-relay ownership claim checked earlier in
     * handle_set_relay()/handle_set_relay_mask(), before relay_on_blocked()
     * is even reached) -- this fires even for a relay no run claims, which
     * ERR_OWNED never would. Appended, not inserted, for the same "plain
     * enum, no explicit values, crosses neither the UART wire nor NVS"
     * reasoning as ERR_UPDATING/ERR_CRASH_UNACK's own comments above.
     * 2026-09-28 (A4 review follow-up B): also returned while a backup
     * restore is in flight (system_mode_gate's restore_in_flight rule) --
     * every surface's text names all three causes. */
    KILN_IO_OWNER_RELAY_ERR_RUNNING,
} kiln_io_owner_relay_result_t;

typedef enum {
    KILN_IO_OWNER_SX_OK = 0,
    KILN_IO_OWNER_SX_REFUSED_RELAY, /* would touch a relay pin, refused --
                                      * docs/SAFETY_MODEL.md's "SX_WRITE_REG/
                                      * SX_SET_DIR can bypass the relay gate" gap */
    KILN_IO_OWNER_SX_IO_FAIL,
    KILN_IO_OWNER_SX_TIMEOUT,
} kiln_io_owner_sx_result_t;

/* Starts the owner task. Must be called exactly once, after kiln_io_init()
 * has succeeded on `io` and before anything below is called -- every
 * producer fails closed (returns a TIMEOUT-flavored result or
 * ESP_ERR_INVALID_STATE, never silently "assume it worked") if called before this or if
 * this itself failed. `safety` may be NULL (no SafetyLinkClass wired in
 * yet) -- same fail-closed behavior as the uart_bridge.c code this
 * replaces: every relay-ON command is refused until a live link exists. */
esp_err_t kiln_io_owner_start(kiln_io_t *io, SafetyLinkClass *safety);

/* ---- MANUAL producers -- ownership + safety-fault gated ---- */

/* relay is 1..KILN_IO_RELAY_COUNT (kiln_io.h's Relay1..4 numbering).
 * out_safety_sources may be NULL; only meaningful when the return is
 * KILN_IO_OWNER_RELAY_ERR_SAFETY. */
kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay(uint8_t relay, bool on,
                                                              uint32_t *out_safety_sources);

/* mask/value bit order matches kiln_io_set_relay_mask() (bit 0 = Relay1 ..
 * bit 3 = Relay4). Refused (ERR_OWNED) if ANY selected relay is owned by a
 * running profile, all-or-nothing, matching the manual single-relay path.
 * out_safety_sources may be NULL. */
kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay_mask(uint8_t mask, uint8_t value,
                                                                   uint32_t *out_safety_sources);

/* ---- AUTHORIZED producer -- profile_executor.c/autotune_engine.c, and
 * (2026-08-28) uart_bridge.c's link-loss watchdog for its unowned-relay
 * drop. See this header's top comment for why each skips the gate the
 * MANUAL functions apply. Returns the raw kiln_io_set_relay_mask() esp_err_t
 * (or ESP_ERR_TIMEOUT/ESP_ERR_INVALID_STATE if the owner task isn't up) so
 * existing callers' "if (err != ESP_OK) log and continue" shape is
 * unchanged. */
esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value);

/* ---- Digital IO (IO_1..IO_7) ---- */
esp_err_t kiln_io_owner_command_set_io(uint8_t index, bool level);
esp_err_t kiln_io_owner_command_set_io_dir(uint8_t index, bool input, bool pullup);

/* Unconditional, no gating (same as kiln_io_all_relays_off() itself --
 * this is the fail-safe path, not something to refuse). */
esp_err_t kiln_io_owner_command_all_relays_off(void);

/* One expander read -- see kiln_io_read()'s doc comment (also clears the
 * expander's interrupt source, releasing ~INT). */
esp_err_t kiln_io_owner_command_read(kiln_io_state_t *out_state);

/* ---- Raw SX1509 register passthrough -- uart_bridge.c's debug/diagnostic
 * subcommands only, no other caller today. Routed through the same owner
 * task as everything above so a diagnostic register poke can't interleave
 * with a relay write at the logical (not just I2C-transaction) level. ---- */

/* Refused (SX_REFUSED_RELAY) if the write would set a relay pin's bit HIGH
 * while a safety fault source is asserted -- see kiln_io_relay_pin_mask().
 * out_safety_sources may be NULL; only meaningful on SX_REFUSED_RELAY. */
kiln_io_owner_sx_result_t kiln_io_owner_command_sx_write_reg(uint8_t reg, uint8_t value,
                                                              uint32_t *out_safety_sources);
esp_err_t kiln_io_owner_command_sx_read_reg(uint8_t reg, uint8_t *out_buf, size_t len);

/* Unconditionally refused (SX_REFUSED_RELAY, no safety_sources -- relay
 * pins are always outputs on this board regardless of fault state) if
 * dir_mask would set any relay pin's bit to 1 (input). */
kiln_io_owner_sx_result_t kiln_io_owner_command_sx_set_dir(uint16_t dir_mask);

/* Unconditionally refused (SX_REFUSED_RELAY) if mask touches a relay pin --
 * see kiln_io_owner.c's sx_mask_touches_relay() doc comment (audit item,
 * TODO.md "Audit 2026-08-27 -- open items"). None of these three can move
 * RegData (the output latch) by themselves, so none can directly energize
 * a coil, but they can silently change how a relay pin behaves once it IS
 * driven (open-drain lets a commanded HIGH float; an enabled interrupt/
 * pull-up fights a pin that must stay a plain push-pull output), so they
 * are refused just like SX_SET_DIR rather than left open on the theory
 * that only RegData matters. */
kiln_io_owner_sx_result_t kiln_io_owner_command_sx_set_pullup(uint16_t mask);
kiln_io_owner_sx_result_t kiln_io_owner_command_sx_set_opendrain(uint16_t mask);

/* Not gated -- debounce is input-edge timing, inert on an output pin, see
 * kiln_io_owner.c's CMD_SX_SET_DEBOUNCE case comment. */
esp_err_t kiln_io_owner_command_sx_set_debounce(uint16_t mask, uint8_t config);

kiln_io_owner_sx_result_t kiln_io_owner_command_sx_set_int_mask(uint16_t mask, uint32_t sense);

/* Unconditionally refused (SX_REFUSED_RELAY) if pin is a relay pin (audit
 * item, TODO.md "Audit 2026-08-27 -- open items"). PWMing a mechanical
 * relay coil is not a normal operation -- see kiln_io_owner.c's
 * sx_led_driver_touches_relay() doc comment for the full reasoning and for
 * why this is refused entirely rather than gated the way an ordinary
 * relay-ON command is (relay_on_blocked()/relay_authority_manual_blocked_
 * by_owner()): there is no legitimate LED-driver use of a relay pin to let
 * through in the first place, so there is nothing for a conditional gate
 * to usefully allow. */
kiln_io_owner_sx_result_t kiln_io_owner_command_sx_led_driver(uint8_t pin, bool enable, uint8_t intensity);

/* Not gated -- a reset can only ever turn relays OFF (every pin becomes an
 * input), never on, so the safety/ownership gates that protect relay-ON
 * commands have nothing to protect against here, same reasoning as
 * kiln_io_owner_command_all_relays_off() above. On success it clears the
 * relay_shadow the SX_READ_REG/READ paths and the dashboard report from,
 * since the reset just silently dropped every coil -- see
 * kiln_io_owner.c's CMD_SX_RESET case comment. */
esp_err_t kiln_io_owner_command_sx_reset(bool hard);
esp_err_t kiln_io_owner_command_sx_scan(uint8_t *out_found, size_t max_found, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // KILN_IO_OWNER_H
