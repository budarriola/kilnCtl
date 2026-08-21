// zones_http -- Settings > Thermocouples & Zones page (TODO.md section 3),
// serving /settings/zones and its JSON API, plus the PID-tuning fields
// section 0.5 explicitly settled onto this same page rather than a
// separate one.
//
// Scope: config storage and validation only. Historically one zone per
// configured thermocouple channel (zone i <-> channel i); TODO.md 10.8
// (2026-08-17, first slice) extends this to a many-to-one mapping via
// thermo_mask below -- a zone's control temperature can now be combined
// across more than one assigned channel. zones_config_get_thermo_mask()'s
// doc comment covers the legacy-mapping default this module falls back to
// when a caller (or an unmodified zones_page.html submission) never sends
// the field. cal_offset_c is stored
// here and applied through zones_config_apply_cal() below, which satisfies
// TODO.md section 3's "applied in firmware" half -- but only for the
// consumers that call it: dashboard_http.c's display and
// profile_executor.c's control math. uart_bridge.c (the PC/MCP UART path)
// still reports MAX31856_read()'s raw value, so those clients see an
// uncorrected number; see zones_config_apply_cal()'s scope note for why
// that gap is deliberate rather than overlooked.
//
// max_ramp_c_per_hr is a user-entered ceiling, not a PID-estimated one --
// TODO.md section 3 poses both options and explicitly leaves the
// PID-estimated one undesigned ("more work, needs a design of its own
// before it's buildable"). The user-entered ceiling is what
// zones_config_get_max_ramp() below hands to profiles_http.c's
// feasibility check (TODO.md section 5).
#ifndef ZONES_HTTP_H
#define ZONES_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sanity bounds for the stored FOPDT plant model, shared by
 * zones_config_set_model() and any caller that must reject an out-of-range
 * model BEFORE writing anything (backup_http.c's import validation pass is
 * the reason these moved here, 2026-08-21: they used to be private #defines
 * in zones_http.c, so the import path could only check "finite and
 * non-negative" in its own first pass and had to let zones_config_set_model()
 * catch the real ceiling at commit time -- too late, since earlier entries in
 * the same import may already have been written to NVS by then. Now both
 * paths gate on the exact same two constants, so a whole-import validation
 * pass can refuse an out-of-range model before any write happens.
 *
 * These are typo/garbage filters, not physics: a kiln's static gain is order
 * 100-1000 degC per unit duty and its time constant order 1e3 s, so the
 * ceilings sit an order of magnitude clear of anything a real fit produces
 * while still rejecting a decimal-point slip. 86400 s (one day) is a hard "no
 * thermal process on this board is slower than this" bound. */
#define ZONE_MODEL_K_MAX 5000.0f
#define ZONE_MODEL_TIME_MAX_S 86400.0f

/* Loads zones_cfg from NVS (namespace "kiln_cfg", key "zones_cfg";
 * ESP_ERR_NVS_NOT_FOUND is not an error -- mirrors wifi_prov.c's nvs_load,
 * defaults to thermo_count/relay_count 0, i.e. nothing configured yet) and
 * registers /settings/zones + GET/POST /api/zones on the server
 * wifi_provision_http.c already started. No hardware pointers needed --
 * this is pure config CRUD, gated by MAX31856_CHANNEL_COUNT /
 * KILN_IO_RELAY_COUNT rather than which hardware actually answered. */
esp_err_t zones_http_start(void);

/* Read-only accessors for profiles_http.c's feasibility check (TODO.md
 * section 5) -- it must not touch NVS or this module's storage directly,
 * only ask these two questions. */

/* zone_index is 0-based, must be < the configured thermo_count. Returns
 * false (and leaves *out_c_per_hr untouched) for an out-of-range or
 * unconfigured zone -- the caller is expected to treat that as "cannot
 * check feasibility," not as a ceiling of 0. */
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr);

/* How many of the MAX31856_CHANNEL_COUNT channels currently have a zone
 * configured -- profiles_http.c uses this to reject a profile targeting a
 * zone that doesn't exist. */
uint8_t zones_config_get_thermo_count(void);

/* TODO.md 6A.5 load-staggering: board-wide cap on relays energized at once
 * (0 = unlimited, the default). Global, not per-zone -- enforced by
 * profile_executor.c against every active zone's commanded relay state in
 * a run, regardless of which zones it spans. */
uint8_t zones_config_get_max_simultaneous_relays(void);

/* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
 * firing" -- false (the default, matching a migrated pre-2026-08-13 blob
 * that predates this field) means abort; true is the explicit opt-in to
 * continue with the other healthy zones. Global, not per-zone -- see
 * zones_cfg_t::continue_on_zone_trip's comment. */
bool zones_config_get_continue_on_zone_trip(void);

/* TODO.md owner-report item 3 (2026-08-21): the RP2040 safety processor's
 * OWN, physically independent thermocouple type, as last saved on the
 * Thermocouples & Zones page. Consumed by safety_link.c to mirror this
 * setting to the Pico over SAFETY_CMD_SET_CONFIG and to re-apply it if the
 * link was down when it changed -- see safety_link.c's safety_sync_tc_type()
 * for the full mirroring design and why this is a SEPARATE setting from any
 * main-board zone's own tc_type.
 *
 * Returns false only for a NULL out pointer -- this is a global setting with
 * a real value from the moment NVS loads, so there is no "cannot answer,
 * zone not configured" case the way there is for the per-zone getters below.
 * Callers that care whether the returned value came from a genuinely loaded
 * config (vs. the zeroed default of a board that has never saved one) must
 * check zones_config_is_valid() themselves -- see this getter's own comment
 * in zones_http.c for why 0 is not a safe "unconfigured" reading here. */
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type);

/* 2026-08-21, LCD item 1: getter/setter pair for one channel's own
 * thermocouple type -- see zones_config_get_pid()'s/zones_config_set_pid()'s
 * doc comments for the shared "false means cannot answer"/generation-bump
 * conventions this follows exactly. zone_index is 0-based, must be < the
 * configured thermo_count (same bound zones_config_get_max_ramp() uses).
 * zones_config_set_tc_type() rejects tc_type > 7 (ZONE_TC_TYPE_MAX_REAL in
 * zones_http.c) -- a voltage-input mode is never legal from an
 * operator-facing type selector, LCD or web. See zones_http.c's own comment
 * on this pair for why the setter deliberately does NOT push the new value
 * to the live MAX31856 register (the web POST path doesn't either; both take
 * effect on the next boot only, today). */
bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type);
bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type);

/* Setter for zones_config_get_safety_tc_type()'s global RP2040-safety-
 * processor setting. Same bound/shape as zones_config_set_tc_type() above. */
bool zones_config_set_safety_tc_type(uint8_t tc_type);

/* TODO.md 8.2 "Tie it to the guards, not only the UI". true only after a
 * real, trustworthy zones config is live -- a successful load (current
 * version, or an older version successfully migrated) or a fresh validated
 * POST /api/zones save. false whenever the loader hit the version-refuses
 * (newer-than-firmware) path, a corrupt/wrong-size blob, an NVS partition
 * that failed to come up, or the namespace was simply never created
 * (first boot). Distinct from "thermo_count == 0", which is a legitimately
 * saved empty config, not a failed load.
 *
 * profile_executor.c and autotune_engine.c must refuse to start a run when
 * this is false, with an explicit reason -- not rely on relay_mask reading
 * as 0 by coincidence of the zeroed struct. dashboard_http.c reports this
 * on /api/status as "zones_config_valid" so the on-device dashboard and the
 * pc_tools GUI's Zones panel can say so instead of silently doing nothing. */
bool zones_config_is_valid(void);

/* Below: read-only accessors for profile_executor.c (TODO.md section 6).
 * Same rule as zones_config_get_max_ramp -- false means "cannot answer,"
 * not "answer is zero." */

/* bit N-1 = relay N belongs to this zone (matches zone_cfg_t::relay_mask,
 * the schematic's Relay1..4 numbering per kiln_io.h). */
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask);

/* TODO.md 10.8: bit N-1 = MAX31856 channel N belongs to this zone's control
 * temperature, N in 1..MAX31856_CHANNEL_COUNT -- same bit-numbering
 * convention as relay_mask above, deliberately: it is the natural pattern
 * this codebase already established for "which of a fixed hardware set
 * belongs to this zone," and following it means a caller that already
 * groks relay_mask reads this correctly on sight.
 *
 * Unlike relay_mask, a saved 0 here is NOT "nothing assigned" for a zone
 * that predates this field or whose submitter never heard of it -- see
 * zones_http.c's POST handler and migrate_zones_cfg_v1_to_current(). Both
 * substitute the legacy single-channel mapping (bit (zone_index) set, i.e.
 * "zone i reads channel i", matching this module's pre-10.8 behaviour)
 * whenever the field was never explicitly supplied, so an existing saved
 * config or an unmodified zones_page.html submission keeps controlling off
 * the same channel it always did. An explicit POST of 0 (a client that DOES
 * know this field and deliberately clears it) is honoured as a real "no
 * thermocouple assigned" and reaches this getter as 0 -- profile_executor.c
 * then sees the same "thermocouple invalid" state a single unassigned
 * channel has always produced (TODO.md 10.8: "zero valid readings ... is
 * the existing thermocouple invalid case, unchanged").
 *
 * Returns false (leaving *out_mask untouched) for the same "cannot answer"
 * reasons every getter here does -- an out-of-range or unconfigured
 * zone_index. */
bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask);

/* TODO.md 10.3: the operator-chosen name a zone_cfg_t already stores
 * (ZONE_NAME_MAX_LEN, currently 15 chars) but which, until now, had no
 * public accessor -- zones_http.c's own JSON responses read it straight off
 * the struct, and no other consumer had asked for it. ui_page_home.c is
 * that second caller, needing something better than "Zone N" for its
 * per-zone cards, hence this getter now existing (same "extract when the
 * second caller shows up" discipline as every other getter in this file).
 *
 * Copies at most out_cap-1 bytes plus a NUL terminator into *out. Returns
 * false (leaving *out untouched) for an out-of-range or unconfigured
 * zone_index, or a NULL/zero-capacity out buffer -- same "cannot answer"
 * convention as every getter above. A TRUE return with an empty string is
 * a real, different case: a configured zone that has never been given a
 * name. Callers must treat that the same as "no name" (e.g. fall back to a
 * generated "Zone N" label), not as a getter failure. */
bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap);

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd);

/* Writer for pid_autotune's results-acceptance flow (TODO.md 6A.4:
 * "results are proposed, never auto-applied... only then does it get
 * written through zones_http's config, which stays the one owner of zone
 * config -- autotune must not write NVS itself"). Only touches kp/ki/kd,
 * persists immediately, and returns false without writing anything for an
 * out-of-range zone_index or a non-finite/negative gain -- same validation
 * discipline as the form-submit path, just for a single field group instead
 * of the whole page. */
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd);

/* The FOPDT plant model autotune fitted for this zone (TODO.md 6A.4),
 * persisted so TODO.md 6A.2's feedforward term
 * u_ff = (T_sp - T_ambient)/K_dc + (dT_sp/dt)*tau/K_dc has something to
 * stand on after a reboot. Before this the fit lived only in
 * autotune_engine.c's RAM for the duration of one run, which is no place
 * for a number that costs the operator hours of real kiln heat to measure.
 *
 * Same "false means cannot answer, not answer is zero" convention as every
 * getter above -- and here the caller must apply it to the VALUES too: 0 in
 * any of the three outputs means no model has been identified for this
 * zone, and a caller that treats that as a real model divides by zero in
 * the expression above. "No model" is the expected state of a zone that has
 * never been autotuned, so the feedforward term is simply off for it (which
 * is what 6A.2 specifies: "default on once autotune has run, off before
 * that"), not an error to report. */
bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s);

/* Writer for the same results-acceptance flow zones_config_set_pid() serves,
 * and subject to the same 6A.4 rule: autotune proposes, the operator
 * accepts, and only then does zones_http -- the one owner of zone config --
 * write it. Only touches the three model fields, persists immediately, and
 * returns false without writing anything for an out-of-range zone_index or
 * a non-finite/negative/absurd parameter, so a rejected call can never
 * leave a zone holding K from one run and tau from another.
 *
 * Writing all zeros is legal and is how a caller clears a stale model
 * (e.g. after the kiln's load or element set changed enough that the old
 * fit is a lie); it is not treated as a validation failure. */
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s);

/* Direction/rate sanity monitor threshold (TODO.md section 6's "reasonable
 * rate ... I will determine later" item) -- degC/minute a zone's actual
 * reading must move, in the commanded direction, over a monitoring window
 * before profile_executor.c treats it as a possibly-detached thermocouple
 * rather than a slow kiln. 0 means "never configured on this zone," which
 * the caller should treat as "use PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN,"
 * not as "disable the check" -- a safety monitor that silently turns itself
 * off because a field was left blank is the wrong default here. */
bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min);

/* TODO.md 6A.1's three control modes. OFF: never commands heat (safe
 * default, and what a zone the board supports but the kiln doesn't use
 * should be set to). BANGBANG: relay on/off around setpoint with a fixed
 * hysteresis band, no PID math -- always available, the fallback if tuning
 * is bad. PID: the pid.c loop, rendered onto the relay by
 * heater_output_duty()'s time-proportioning window. */
typedef enum {
    ZONE_CONTROL_MODE_OFF = 0,
    ZONE_CONTROL_MODE_BANGBANG = 1,
    ZONE_CONTROL_MODE_PID = 2,
} zone_control_mode_t;

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode);

/* Guard 5's absolute limits (TODO.md 6A.3). max_temp_c == 0 means "not set,
 * treated as no ceiling" -- matches the safe-default-vs-configured-zero
 * convention every other zone field uses (max_ramp_c_per_hr, sanity_rate).
 * min_temp_c has no such special case -- the page's input defaults to -20C
 * (a plausible "colder than any kiln room" floor) so a saved zone always
 * carries a real value. */
bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c);

/* TODO.md 6A.9: per-zone heater_output_cfg_t timing, page-configurable.
 * 0 in any of the three outputs means "not configured" -- the caller
 * (profile_executor.c/autotune_engine.c) substitutes its own
 * PROFILE_EXECUTOR_DEFAULT_*_MS, same "false/0 means cannot answer, not
 * answer is zero" convention as every other getter here. */
bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms);

/* Guard 8's cross-zone plausibility threshold (TODO.md 6A.3/6A.5), in degC.
 * 0 means "not configured", and here that DISABLES the guard rather than
 * selecting a default -- unlike sanity_rate_c_per_min. A usable number
 * depends on how strongly this kiln's zones couple, which is what 6A.5's
 * cross-gain matrix measures; until an operator has one, no threshold is
 * better than a hand-picked one that either nuisance-trips a kiln that
 * genuinely stratifies or is too wide to catch anything. The guard also
 * needs >=2 zones reporting to compare against, so it stays inert on a
 * single-zone kiln whatever this is set to. */
bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c);

/* TODO.md 6A.3's remaining named thresholds (wrong-dir rate/window,
 * off-settle, runaway rate/margin, drift period, sensor debounce count,
 * frozen window) -- one getter, matching zones_config_get_heater_cfg()'s
 * "bundle the related group" precedent rather than eight single-field
 * getters. Same "false/0 means cannot answer, not answer is zero" convention
 * as every getter above, and 0 in an output does NOT disable the
 * corresponding guard -- thermal_guard.c substitutes its own firmware
 * default for a 0, same rule as sanity_rate_c_per_min (the opposite
 * convention to cross_zone_max_delta_c, which 0 genuinely disables). */
bool zones_config_get_guard_thresholds(uint8_t zone_index, float *out_wrong_dir_window_s,
                                       float *out_wrong_dir_rate_c_per_min, float *out_off_settle_s,
                                       float *out_runaway_rate_c_per_min, float *out_runaway_margin_c,
                                       float *out_drift_period_s, float *out_sensor_fault_debounce_ticks,
                                       float *out_frozen_window_s);

/* Monotonic counter, incremented every time the stored zone config
 * changes -- TODO.md 6A.7's "config reload while running". A consumer
 * (profile_executor.c) caches the value it last read its zone settings at
 * and re-reads them when this differs, instead of re-querying every getter
 * on every control tick or, worse, latching config once at run start and
 * silently ignoring an operator's edit for the rest of a multi-hour firing.
 *
 * Starts at 1, never 0: the natural way to write such a consumer is to
 * leave its cached generation zero-initialized, so 0 must be a value this
 * never returns if "never loaded anything yet" is to reliably compare
 * different from the current config. Unlike the getters above there is no
 * "cannot answer" case -- there is always a current config, even if it is
 * the all-zeros unconfigured one, and the caller only ever compares this
 * value against its own previous copy, never interprets its magnitude.
 *
 * Bumped only for changes that reached the in-RAM struct: a POST rejected
 * by validation never touched it, so it is not a config change. An NVS
 * write that fails after the in-RAM struct was updated IS one -- the live
 * config differs from what the consumer cached regardless of whether it
 * will survive a reboot. Wraparound is a non-issue in practice (2^32
 * operator edits) and harmless in principle, since != is the only test. */
uint32_t zones_config_generation(void);

/* Applies this zone's cal_offset_c to a raw reading -- the "applied in
 * firmware" half of TODO.md section 3's calibration item, at last wired to
 * an actual consumer. Returns raw_c unchanged if zone_index is out of range
 * or raw_c is NaN (NaN + anything is still NaN, but this makes the
 * "unconfigured zone / invalid reading -> pass through" behavior explicit
 * rather than relying on float semantics).
 *
 * SCOPE NOTE: dashboard_http.c's display, profile_executor.c/
 * autotune_engine.c's control math (TODO.md 6A.2/6A.3), readiness_http.c's
 * wizard check, and now uart_bridge.c's THERMO READ payload (2026-08-13,
 * closing the one documented holdout) all call this. A single
 * calibration-provider hook on MAX31856BusClass itself, mirroring the
 * existing drdy_provider pattern, remains the theoretically cleaner
 * lowest-common-point fix, but with every actual consumer now calling
 * through this function there is no live inconsistency left to motivate it
 * -- revisit only if a new consumer reads MAX31856_read() directly instead
 * of going through here. */
float zones_config_apply_cal(uint8_t zone_index, float raw_c);

#ifdef __cplusplus
}
#endif

#endif // ZONES_HTTP_H
