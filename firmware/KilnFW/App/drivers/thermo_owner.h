// thermo_owner -- the single task that ever touches the MAX31856 thermocouple
// channels' SPI-facing API (config, thresholds, one-shot/read, fault
// mask/clear, raw register passthrough), so every subsystem that wants a
// thermocouple operation done goes through one queue instead of calling
// MAX31856_*() straight from whichever task it happens to run on.
//
// 2026-08-19, TODO.md section 10.14 Phase 2 -- same shape/risk profile as
// kiln_io_owner.c (Phase 1), same file-count cost, deliberately mirroring its
// structure closely rather than inventing a new one. IMPORTANT DIFFERENCE
// FROM PHASE 1, stated plainly rather than implied: Phase 1 existed because
// SX1509's own mutex only serialized ONE I2C transaction at a time, not a
// whole logical relay/IO operation, which left a real lost-update race
// between independent callers. MAX31856.c's ch->lock is not that -- see
// MAX31856.c's max31856_lock()/max31856_unlock() (around line 77) and its own
// comment: every public MAX31856_* entry point takes ch->lock for its ENTIRE
// multi-transfer sequence (read-modify-write of CR0/CR1, a burst threshold
// write, a read followed by the derived logging, etc.) and releases it only
// once that whole sequence is done. Two callers racing on the same channel
// today already get correctly serialized, whole-operation, by that lock --
// there is no equivalent of kiln_io_set_relay_mask()'s read-modify-write
// racing across two independent SX1509 register writes here. So this file is
// NOT fixing a live bug. It exists so that every hardware subsystem on this
// board has exactly one owning task (kiln_io_owner for the expander,
// thermo_owner for the thermocouples, matching this codebase's existing
// firmware/SaftyFW/src/tasks/relay_owner.c precedent), for architectural
// consistency with Phase 1 and to give Phase 6's later system-mode command
// gate (see the plan at TODO.md 10.14 -- "is this class of command allowed
// right now", not "can two writers race") a single choke point per subsystem
// to sit in front of, once that phase is designed and built. Do not read a
// race-fix motivation into this file that Phase 1's header genuinely had and
// this one does not.
//
// Producer shape: every thermo_owner_command_*() below is a bounded,
// non-blocking POST (xQueueSend with 0 ticks) followed by a bounded WAIT on a
// per-call result (a stack-allocated binary semaphore) -- post_and_wait(),
// identical in shape to kiln_io_owner.c's helper of the same name. Callers
// today are uart_bridge.c's thermo_bridge_task (UART) and safety_link.c's
// poll task (building the periodic SAFETY_CMD_PUSH_CONTEXT broadcast); neither
// is the owner task itself, so neither can deadlock waiting on it.
//
// Explicitly NOT covered, and deliberately so:
//   - MAX31856_get_config(): reads ch->cr0_shadow/ch->cr1_shadow under
//     ch->lock and returns -- no SPI transfer at all (see MAX31856.c's
//     implementation, around line 793). Routing a call with no hardware
//     access through a task hop would add latency for zero correctness
//     benefit, the same reasoning kiln_io_owner.h gives for leaving
//     kiln_io_lcd_dc()/kiln_io_lcd_reset() direct. safety_link.c keeps
//     calling it directly.
//   - MAX31856_read_all(): read-only (no register write, no shared state
//     mutated besides each channel's own shadow bookkeeping, already
//     serialized per-channel by ch->lock), and called from several places in
//     this codebase today (profile_executor.c's control loop, autotune_engine.c,
//     dashboard_http.c, board_temps.c, ota_http.c) that are out of scope for
//     this pass -- profile_executor.c and dashboard_http.c are explicitly
//     off-limits here, and the rest were not part of the diagnosed problem
//     (a single owning task for *writes and configuration*, not every read).
//     safety_link.c's call is the one exception migrated here
//     (thermo_owner_command_read_all() below), because it sits alongside
//     MAX31856_get_config() in the same function this pass already had to
//     touch to build the safety-context broadcast, not because its read_all()
//     call was itself racing anything.
#ifndef THERMO_OWNER_H
#define THERMO_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "MAX31856.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the owner task. Must be called exactly once, after
 * MAX31856_start_all() has run on `bus` (whether or not every channel came
 * up -- see below) and before anything in this header is called. Every
 * producer fails closed (ESP_ERR_TIMEOUT/ESP_ERR_INVALID_STATE, never
 * silently "assume it worked") if called before this or if this itself
 * failed.
 *
 * BENCH NOTE (2026-08-19): the thermocouple daughterboard is not physically
 * connected in this environment, so MAX31856_start_all() is expected to
 * leave every channel un-initialized (`bus->initialized` false) here -- this
 * function still succeeds in that case: the owner task starts and its queue
 * comes up, but every per-channel command it drains will find
 * MAX31856_bus_channel() returning NULL and answer ESP_ERR_NOT_FOUND, same
 * as a channel that failed its own bring-up on real hardware. Nothing in
 * this file assumes any channel is actually present. `bus` may not be NULL
 * (there is nothing to own without one), but its channels may all be down. */
esp_err_t thermo_owner_start(MAX31856BusClass *bus);

/* --- Configuration ------------------------------------------------------ */

/* channel is 0..MAX31856_CHANNEL_COUNT-1. Every producer below returns
 * ESP_ERR_NOT_FOUND if the channel is out of range or never came up (the
 * owner task's MAX31856_bus_channel() lookup failing), matching
 * MAX31856_bus_channel()'s own "NULL means report this channel as failed"
 * contract -- callers that used to get NULL back from a direct
 * MAX31856_bus_channel() call and branch on that now get the same signal via
 * the return code instead. */
esp_err_t thermo_owner_command_config_channel(uint8_t channel, uint8_t tc_type, uint8_t avg_mode,
                                              bool filter_50hz, bool auto_convert);

esp_err_t thermo_owner_command_set_thresholds(uint8_t channel, float tc_high_c, float tc_low_c,
                                              int8_t cj_high_c, int8_t cj_low_c);

esp_err_t thermo_owner_command_set_cj_offset(uint8_t channel, float offset_c);

/* --- Measurement ---------------------------------------------------------*/

esp_err_t thermo_owner_command_trigger_one_shot(uint8_t channel);

/* Fills *out unconditionally, same contract as MAX31856_read(): on any
 * failure (bad channel, timeout, SPI error) *out is zeroed with NaN
 * temperatures and spi_failed set, never left stale/uninitialized. */
esp_err_t thermo_owner_command_read(uint8_t channel, MAX31856Reading *out);

/* Mirrors MAX31856_read_all()'s shape exactly (out array + max_readings +
 * out_count) -- see this header's top comment for why this is the one
 * read_all() caller migrated in this pass (safety_link.c). */
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings,
                                        size_t *out_count);

esp_err_t thermo_owner_command_read_faults(uint8_t channel, uint8_t *out_sr, uint8_t *out_mask);

esp_err_t thermo_owner_command_clear_faults(uint8_t channel);

/* --- Raw register passthrough -- uart_bridge.c's debug subcommands only,
 * no other caller today. Routed through the same owner task as everything
 * above so a diagnostic register poke can't interleave with a config write
 * at the logical (not just SPI-transaction) level. len is 1..
 * MAX31856_MAX_BURST_LEN. --- */
esp_err_t thermo_owner_command_read_reg(uint8_t channel, uint8_t reg, uint8_t *out_buf, size_t len);
esp_err_t thermo_owner_command_write_reg(uint8_t channel, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif // THERMO_OWNER_H
