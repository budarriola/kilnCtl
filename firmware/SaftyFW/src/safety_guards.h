// safety_guards -- the safety processor's pure guard-evaluation module.
// TODO.md Phase 4.
//
// Pure function of (config, input, state) -> verdict, same host-testability
// reasoning as KilnFW's thermal_guard.h/pid.h: no FreeRTOS, no pico-sdk, no
// logging, no I/O, no time source of its own -- dt_s comes in as an argument
// every tick, inside the input struct (matching thermal_guard_input_t's own
// convention).
//
// This module implements 12 of the 13 guards in docs/SAFETY_MODEL.md
// section 4. S1/S5/S7/S11/S12 need nothing but a thermocouple snapshot and
// two debounced discretes. S2/S3/S4/S6/S9/S10/S13 additionally need context
// from the ESP (relay/setpoint/zone data) and/or current-sense presence --
// rather than pulling in context_snapshot_t / current_snapshot_t from
// link_task/current_task (neither of which exist as real producers yet,
// and this module must never #include the link header per ARCHITECTURE.md
// section 2's "the one rule that matters"), the exact scalar facts each
// guard needs are flattened directly into safety_guard_input_t below, each
// with its own "do you know this?" validity flag. That keeps this module
// exactly as pure and link-header-free as before -- it is still a
// synthetic-input pure function, just fed by more fields -- while giving
// safety_core a one-line mapping job once link_task/current_task are real
// (Phase 6/7), instead of a redesign.
//
//   S1  Absolute over-temperature              TRIP
//   S2  Sustained excess over setpoint         TRIP  (context, CHAMBER_AGREED only)
//   S3  Load active, no heat commanded         TRIP  (context)
//   S4  Heat commanded, load inactive          WARN  (context)
//   S5  Safety thermocouple invalid            WARN, then TRIP (graduated)
//   S6  Main controller unhealthy              TRIP  (mainFault discrete + link liveness)
//   S7  E-stop                                  TRIP
//   S8  Implausible rate of rise                TRIP, ships disabled (max_rate_c_per_min == 0)
//   S9  Trip ineffective / contactor welded    ESCALATE (post-trip current)
//   S10 Safety TC vs zone TC disagreement       WARN  (context, CHAMBER_AGREED only)
//   S11 Frozen safety reading                   TRIP
//   S12 Cold junction / enclosure over-temp     WARN, then TRIP (graduated)
//   S13 Borrowed channel not updating           WARN, then TRIP (context, BORROWED_ZONE/BOTH)
//
// S8 is now implemented (this pass), but SHIPS INERT: max_rate_c_per_min ==
// 0.0f (the zero-initialised, uncommissioned, and documented-default value --
// SAFETY_MODEL.md section 4, S8, config_store.c's config_store_default())
// means "not commissioned, never trip", the identical convention S1 uses for
// abs_max_temp_c and for the identical reason -- nobody has measured this
// kiln's maximum legitimate ramp rate, so this module must not guess one.
// See safety_guards.c's S8 block for the averaging-window design that keeps
// a single noisy sample from being mistaken for a runaway.
//
// NOT implemented here: the "runtime configuration integrity" background CRC
// check (SAFETY_MODEL.md section 4) -- that is a config_store concern
// (Phase 9, no config_store exists yet), not a guard evaluated per tick.
//
// Integration note (read before assuming this ships live): this file and its
// host tests are the pure guard only. safety_core.c's
// safety_core_load_guard_cfg() -- the function that copies commissioned
// config_store fields into s_guard_cfg, same one whose omission for S1 was a
// 2026-08-27 audit finding ("the pure function was correct the whole time;
// the value never arrived") -- has NOT been touched by this pass to populate
// max_rate_c_per_min/rate_window_s from config_store's existing 0x0204/0x0205
// fields. Until that one-line wiring lands, S8 is built and host-tested but
// NOT integrated, in this document's own vocabulary, and ships exactly as
// inert as it did before this pass (max_rate_c_per_min reads 0 either way).
//
// Latching, always (SAFETY_MODEL.md section 2's "latching is not
// auto-recovery"): once tripped, this module reports is_tripped == true on
// every subsequent call until the caller explicitly safety_guards_clear()s
// it. There is no condition-cleared auto-reset.
//
// Independence invariant (ARCHITECTURE.md section 10): nothing in this file
// reads, or even knows the shape of, context_snapshot_t. The input struct
// below has no link-derived field of any kind -- heat_commanded is a plain
// caller-supplied fact (see its comment on safety_guard_input_t), not a
// context read. A guard verdict from this module can never depend on
// whether the isolated link is up, because this module cannot tell.
#ifndef SAFETY_GUARDS_H
#define SAFETY_GUARDS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verbatim from ARCHITECTURE.md section 9. Do not renumber -- once this
 * ships, a trip code is a wire value that ends up in logs and screenshots,
 * and renumbering it later silently reinterprets every historical record.
 * 4 and 11 are reserved gaps (S4 and S10 are WARN-only, never produce a
 * trip code) and are kept here so the enum stays byte-for-byte the one the
 * doc defines, even though this phase only ever assigns five of these
 * values. */
typedef enum {
    SAFETY_TRIP_NONE = 0,
    SAFETY_TRIP_OVERTEMP        = 1,  /* S1  -- implemented */
    SAFETY_TRIP_OVER_SETPOINT   = 2,  /* S2  -- implemented */
    SAFETY_TRIP_LOAD_STUCK_ON   = 3,  /* S3  -- implemented */
    /* 4 reserved: S4 is WARN-only */
    SAFETY_TRIP_SENSOR_INVALID  = 5,  /* S5  -- implemented, after blind_grace_s */
    SAFETY_TRIP_MAIN_FAULT      = 6,  /* S6a -- implemented */
    SAFETY_TRIP_LINK_DEAD       = 7,  /* S6b -- implemented */
    SAFETY_TRIP_ESTOP           = 8,  /* S7  -- implemented */
    SAFETY_TRIP_RATE            = 9,  /* S8  -- implemented; ships disabled (max_rate_c_per_min == 0) */
    SAFETY_TRIP_INEFFECTIVE     = 10, /* S9  -- implemented */
    /* 11 reserved: S10 is WARN-only */
    SAFETY_TRIP_FROZEN_SENSOR   = 12, /* S11 -- implemented */
    SAFETY_TRIP_ENCLOSURE_TEMP  = 13, /* S12 -- implemented */
    SAFETY_TRIP_BORROWED_STALE  = 14, /* S13 -- implemented */
    SAFETY_TRIP_CONFIG_CORRUPT  = 15, /* not this module -- config_store, Phase 9 */
    SAFETY_TRIP_SELF_TEST       = 16, /* not this module -- watchdog_task's job */
} safety_trip_t;

/* SAFETY_MODEL.md section 3, "tc_placement_mode". Only S1 cares about it in
 * this phase (S2/S10 are not implemented yet). No default on purpose --
 * a caller that has not commissioned this field should be leaving
 * abs_max_temp_c at 0 too, which independently keeps S1 from trusting a
 * ceiling it was never told to use. */
typedef enum {
    SAFETY_TC_CHAMBER_AGREED = 0,
    SAFETY_TC_EXTERNAL_OVERHEAT = 1,
} safety_tc_placement_mode_t;

/* SAFETY_MODEL.md section 3, "tc_source" -- which sensor is the active
 * safety reading. Only S13 (borrowed-channel staleness) and S11's "which
 * source is active" note care about this; S1/S5/S12 above operate on
 * whatever in->tc_c/cj_c/tc_valid the caller hands them regardless of where
 * it came from, by design (SAFETY_MODEL.md section 3: those never change
 * behaviour based on tc_source). No default, same reasoning as
 * tc_placement_mode -- OWN_J7 is the conservative reading of "not
 * commissioned yet" because it is the only mode where S13 (which needs a
 * commissioning decision to even be meaningful) correctly stays off. */
typedef enum {
    SAFETY_TC_SOURCE_OWN_J7 = 0,
    SAFETY_TC_SOURCE_BORROWED_ZONE = 1,
    SAFETY_TC_SOURCE_BOTH = 2,
} safety_tc_source_t;

/* MAX31856 SR register bits, mirrored from KilnFW's uart_task_ids.h (same
 * part, same register, ported per ARCHITECTURE.md section 3's "port it, do
 * not rewrite it" for max31856.c -- the bit layout is part of what gets
 * ported, so it is duplicated here rather than pulled in from a KilnFW
 * header this module must never depend on). S5 only cares about
 * OPEN/OVUV/TCRANGE; TCHIGH/TCLOW are S1's job and CJHIGH/CJLOW/CJRANGE
 * alone are not this guard's concern (S12 is the numeric CJ guard). */
#define SAFETY_THERMO_FAULT_OPEN     0x01u /* thermocouple open circuit */
#define SAFETY_THERMO_FAULT_OVUV     0x02u /* over/under voltage on an input */
#define SAFETY_THERMO_FAULT_TCLOW    0x04u /* TC temperature below low threshold -- not S5's job */
#define SAFETY_THERMO_FAULT_TCHIGH   0x08u /* TC temperature above high threshold -- not S5's job */
#define SAFETY_THERMO_FAULT_CJLOW    0x10u /* cold junction below low threshold -- not S5's job */
#define SAFETY_THERMO_FAULT_CJHIGH   0x20u /* cold junction above high threshold -- not S5's job */
#define SAFETY_THERMO_FAULT_TCRANGE  0x40u /* TC temperature outside the type's range */
#define SAFETY_THERMO_FAULT_CJRANGE  0x80u /* cold junction outside -55..+125 degC -- not S5's job */

/* Thresholds. Every field follows thermal_guard_cfg_t's "0 means not
 * configured, substitute the firmware default" convention UNLESS its doc
 * comment says otherwise -- abs_max_temp_c is the one field here with no
 * safe default at all (SAFETY_MODEL.md section 4, S1: "abs_max_temp_c has
 * no default and must be commissioned"), so 0 there means "not
 * commissioned, never trip" rather than "use a hidden fallback ceiling". A
 * silently-substituted absolute temperature limit is exactly the kind of
 * thing SAFETY_MODEL.md section 2 calls a nuisance-trip risk in the wrong
 * direction -- and, worse, a *missed*-trip risk if the substituted number
 * were ever accidentally too high. */
typedef struct {
    /* tc_placement_valid: false means "nobody has commissioned
     * tc_placement_mode yet", and every guard that depends on the placement
     * decision must stay OFF while it is false. This flag exists because the
     * enum below CANNOT express "unset": SAFETY_TC_CHAMBER_AGREED is 0, which
     * is also what a zero-initialised config struct holds, and 0 is the value
     * that ARMS S2 and S10. SAFETY_MODEL.md section 3 is explicit that
     * "there is no safe default, so there is no default -- until it is set,
     * S2 and S10 stay off"; without this flag the code did the exact
     * opposite, arming both against an undeclared sensor on every
     * uncommissioned board (audit 2026-08-27). Renumbering the enum to add
     * an UNSET member was rejected: those values are on the wire
     * (CONFIG_STORE_TC_PLACEMENT_* in config_store.h) and in flash records,
     * so shifting them would silently reinterpret every already-commissioned
     * board's stored config. */
    bool tc_placement_valid;
    safety_tc_placement_mode_t tc_placement_mode;

    /* S1. abs_max_temp_c: 0 = not commissioned, guard never trips (see the
     * struct doc comment above). firing_margin_c: 0 -> 100.0C default. */
    float abs_max_temp_c;
    float firing_margin_c;

    /* S1's firing_max_c is context, not config -- it arrives from the ESP
     * over the isolated link at profile start (SAFETY_MODEL.md section 4,
     * S1) and link_task does not exist yet (that's Phase 7). It is placed
     * here, in the config struct, rather than in safety_guard_input_t,
     * because this pure module has no separate per-tick context input in
     * this phase and the task brief for Phase 4 is explicit that the field
     * should live somewhere ready for Phase 7 to wire up rather than being
     * invented fresh then. Until link_task sets firing_max_valid, S1
     * behaves exactly as SAFETY_MODEL.md says it must "when no firing is
     * running": ceiling = abs_max_temp_c, unconditionally. Re-examine
     * whether this belongs in a context-shaped input instead once Phase 7
     * actually produces one -- config that changes with what firing is
     * running today is an odd fit for a struct otherwise reserved for
     * bench-tunable numbers. */
    bool  firing_max_valid;
    float firing_max_c;

    /* S5. bad_read_count_threshold: 0 -> 10 consecutive. bad_read_time_s:
     * 0 -> 5.0s. blind_grace_s: 0 -> 60.0s. Both the count AND the time
     * bar must clear before WARN (SAFETY_MODEL.md section 4, S5's "a burst
     * of 10 reads in 50ms at some hypothetical fast poll rate shouldn't
     * warn; needs both"). */
    uint16_t bad_read_count_threshold;
    float    bad_read_time_s;
    float    blind_grace_s;

    /* S11. 0 -> 600.0s. */
    float frozen_window_s;

    /* S12. 0 -> 60.0C / 85.0C / 60.0s respectively. Unlike abs_max_temp_c,
     * these three have real, doc-specified defaults (SAFETY_MODEL.md
     * section 4, S12) and are never "not commissioned" -- a cold junction
     * near a real MAX31856 is never legitimately 0C, so 0 is a safe
     * "unset, use the default" sentinel here. */
    float cj_warn_c;
    float cj_max_c;
    float cj_time_s;

    /* S13. tc_source picks whether S13 can be active at all -- WARN/TRIP
     * (SAFETY_MODEL.md section 4, S13: "BORROWED_ZONE/BOTH only"). 0 ->
     * borrowed_stale_s=10.0s, borrowed_stale_trip_s=60.0s. */
    safety_tc_source_t tc_source;
    float borrowed_stale_s;
    float borrowed_stale_trip_s;

    /* S2. CHAMBER_AGREED only (tc_placement_mode above gates it). 0 ->
     * overshoot_margin_c=75.0C, overshoot_time_s=120.0s. */
    float overshoot_margin_c;
    float overshoot_time_s;

    /* S3/S4 share the current-presence threshold and the correlation
     * window (SAFETY_MODEL.md section 4). 0 -> i_present_a=2.0A,
     * correlation_window_s=150.0s. S3-only: stuck_on_time_s, 0 -> 20.0s. */
    float i_present_a;
    float correlation_window_s;
    float stuck_on_time_s;

    /* S6. 0 -> link_timeout_s=10.0s, link_dead_hard_s=120.0s,
     * main_fault_debounce_s left to discrete_task (already-debounced input,
     * same convention as estop_pressed) -- nothing to configure here. */
    float link_timeout_s;
    float link_dead_hard_s;

    /* S9. 0 -> trip_verify_s=10.0s. */
    float trip_verify_s;

    /* S10. CHAMBER_AGREED only. 0 -> tc_disagreement_c=200.0C,
     * tc_disagreement_time_s=300.0s. */
    float tc_disagreement_c;
    float tc_disagreement_time_s;

    /* S8. max_rate_c_per_min: 0.0f = not commissioned, guard never trips --
     * NOT a "substitute a default" 0, the same convention and the same
     * reason as abs_max_temp_c above (SAFETY_MODEL.md section 4, S8: "it
     * ships off ... nobody has ever measured this kiln's maximum legitimate
     * ramp rate"). config_store.c's config_store_default() already sets
     * this field to 0.0f, so an uncommissioned board is inert by
     * construction, not merely by this module's own gate. rate_window_s: 0
     * -> 60.0s default, an ordinary "0 means not configured" field like
     * every window elsewhere in this struct -- unlike the magnitude, a
     * window default is not a safety judgement call. */
    float max_rate_c_per_min;
    float rate_window_s;

    /* S14. Per-channel measured "normal" current (0x031A-0x031C,
     * i_normal_a[0..2]) and its own presence flag -- unlike every other
     * field in this struct, 0.0f is NOT "not configured, substitute a
     * default": a channel's normal current is a real measurement with no
     * safe firmware-invented fallback (COMMISSIONING_UX.md section 3.2,
     * "a channel whose i_normal_a has no fields_set bit is skipped
     * entirely"), so the caller must say explicitly, per channel, whether a
     * measurement exists. i_normal_valid[ch] == false means S14 is inactive
     * for that channel -- no accumulation, no warn, ever -- regardless of
     * what amps[ch] reads. overcurrent_pct: 0 -> 150 (%). overcurrent_time_s:
     * 0 -> 30.0s. */
    bool  i_normal_valid[3];
    float i_normal_a[3];
    uint16_t overcurrent_pct;
    float    overcurrent_time_s;

    /* ct_topology (config param 0x031F, CT_COMMISSIONING_PLAN.md step 3).
     * false (default, per_zone) -- one CT per zone, S14 evaluated per
     * channel exactly as above. true (summed) -- a single CT (channel 2,
     * "channel 3"/GPIO28) reads the sum of every zone's current, so S14's
     * per-channel comparison is meaningless for channels 0/1 (no sensor
     * behind them at all -- they must report inert/not-fitted, same
     * "current_sensing_disabled" treatment as a genuinely absent CT, never
     * a false pass) and channel 2's comparison must be against the SUM of
     * i_normal_a[] for whichever zones are commanded on right now, not a
     * single channel's own normal. Also arms S15 (new, WARN-only
     * under-current/open-heater detector), which only means anything when
     * one CT is shared across zones -- see safety_guards.c's S14/S15 block
     * and safety_guard_input_t::relay_commanded_now_for_zone below. Zero-
     * initialized (false/per_zone) so every existing test and caller keeps
     * today's per-channel behaviour unchanged. */
    bool ct_topology_summed;
} safety_guard_cfg_t;

/* One call's worth of input. tc_c/cj_c/fault_bits/spi_failed follow
 * ARCHITECTURE.md section 6's thermo_snapshot_t shape (tc_valid == false
 * implies tc_c and cj_c are NaN, never 0, never the last good reading --
 * SAFETY_MODEL.md section 4, S5). timestamp_ms is deliberately not part of
 * this struct: staleness is the producer/consumer's problem upstream of
 * this pure module (ARCHITECTURE.md section 6, "staleness is checked by the
 * consumer, not the producer"); by the time a snapshot reaches
 * safety_guards_tick() it is assumed to be this tick's fresh reading. */
typedef struct {
    bool     tc_valid;
    float    tc_c;         /* NaN when !tc_valid */
    float    cj_c;         /* NaN when !tc_valid */
    uint8_t  fault_bits;   /* SAFETY_THERMO_FAULT_* bits, SR register */
    bool     spi_failed;

    /* S5's declared-hardware-state escape hatch (config param 0x0211,
     * config_store.h's safety_tc_installed). Named with the INVERTED sense
     * of that config field, deliberately: every other bool in this struct
     * is false-by-default-safe (zero-initializing a test's or a stub
     * caller's input struct produces the normal, fully-armed guard
     * behaviour), and safety_tc_installed's own polarity (1 = normal, 0 =
     * escape hatch) would break that convention here -- a caller (or an
     * existing test literal written before this field existed) that leaves
     * this struct zero-initialized must still get S5's real TRIP behaviour,
     * not silently inherit the "sensor declared absent" downgrade. false
     * here means "installed" (the normal case); safety_core_build_input()
     * is the one place that does the inversion, from config_store's
     * safety_tc_installed. Read only by S5's grace-exceeded branch in
     * safety_guards.c -- see that block's own comment for what flipping
     * this changes (WARN stays, TRIP is withheld) and, just as
     * importantly, what it does NOT change: heating itself is refused
     * unconditionally elsewhere (safety_core_request_enable()) whenever the
     * declaring config field is 0, which is the only reason downgrading
     * this guard's own trip is safe at all. */
    bool safety_tc_not_installed_declared;

    /* S7. Already debounced (50ms) by discrete_task -- ARCHITECTURE.md's
     * module table gives debouncing to discrete_task, not to this pure
     * evaluator, the same division profile_executor.c / thermal_guard.c
     * use on the KilnFW side. */
    bool estop_pressed;

    /* S11's "and heat is happening" qualifier (SAFETY_MODEL.md section 4,
     * S11), named for exactly what it is rather than for where it might come
     * from: a plain fact the caller supplies, not link-derived data.
     * safety_core wires this from any_current_present -- SAFETY_MODEL.md's
     * own S11 formula is "current flowing OR heat commanded", and current
     * actually flowing is energy actually going in, which is what the
     * guard's prose cares about ("a genuinely static value ... while energy
     * is going in, does not happen in a real thermal system"). Deliberately
     * NOT wired from relay_commanded_recently/_continuously below even
     * though those also approximate "heat commanded": both are
     * context-derived, and this field must stay link-independent (this
     * struct's own header comment, and SAFETY_MODEL.md section 6's S11/S13
     * audit note: "S11 reads neither [link_up nor context_valid]"), so that
     * S11 keeps working -- keeps authority over K4 -- even with the link
     * down or the main controller absent. On an idle kiln with no current
     * flowing this is false, which is what keeps S11 correctly dormant (the
     * exact nuisance case SAFETY_MODEL.md section 4 warns about: "a cold,
     * idle kiln legitimately sits at a constant reading for hours") without
     * the guard ever having to guess or silently drop its own qualifier. */
    bool heat_commanded;

    /* --- Context from the ESP, over the isolated link (SAFETY_MODEL.md
     * section 5). context_valid is the single "do you know any of this?"
     * flag -- staleness/version-mismatch/never-received all collapse to
     * false here, per ARCHITECTURE.md section 6's "staleness is checked by
     * the consumer, not the producer" and section 9's DEGRADED_NO_CONTEXT
     * rule that context-dependent guards report inactive, never
     * pessimistic, when context is unusable. When false, S2/S3/S4/S10/S13
     * below are all skipped outright -- the caller does not need to zero
     * every other context field to make that safe. */
    bool  context_valid;

    /* S2/S10. The zone setpoint/measurement data S2 and S10 need, already
     * reduced by the caller (safety_core, once link_task exists) to the two
     * scalars each guard actually uses -- SAFETY_MODEL.md section 4 is
     * explicit both compare against "max(active zone setpoints)" (S2) and
     * "nearest valid zone measured_c" (S10), never the raw per-zone array,
     * so there is nothing this pure module would do with the array that
     * the caller cannot do once instead, every tick. zone_count == 0 means
     * "no active zones this tick" and both guards go inactive, same as
     * context_valid == false. */
    uint8_t zone_count;
    float   max_zone_setpoint_c;    /* S2 */
    float   nearest_zone_measured_c; /* S10 */

    /* S3/S4. Presence/absence facts only (SAFETY_MODEL.md section 3: "not
     * an over/under-current guard") -- any_current_present is already the
     * OR across all three channels against i_present_a, and
     * relay_commanded_recently/relay_commanded_continuously are already
     * relay_recent_mask-derived booleans (SAFETY_MODEL.md section 4, S3:
     * "was any relay commanded on at any point in the last N seconds",
     * computed on the ESP -- not the instantaneous mask). Kept as two
     * separate booleans rather than one mask, matching how S3 (recently,
     * i.e. OR over the window) and S4 (continuously, i.e. AND over the
     * window) genuinely ask different questions of the same window. */
    bool any_current_present;
    bool relay_commanded_recently;
    bool relay_commanded_continuously;

    /* S9 only (2026-08-27 audit, second pass). True iff the current-sensing
     * chain behind any_current_present is actually CALIBRATED -- the same
     * branch current_presence_policy.h's current_presence_is_flowing()
     * itself takes (k_ct_v_per_a > 0 for the relevant channel(s)), NOT
     * config_store's calibration_missing record (that flag tracks a
     * different, broader commissioning checklist -- tc_source/tc_placement_
     * mode/abs_max_temp_c/ct_channel_map/max_rate_c_per_min/mains_voltage_v/
     * tc_type via config_params_all_required_set() -- and does not include
     * k_ct_v_per_a at all, so it cannot answer this question).
     *
     * Why S9 alone needs its own trust flag when S3/S4/S6b/S11 read the same
     * any_current_present without one: current_presence_policy.h's own
     * header comment documents that an UNCOMMISSIONED k_ct_v_per_a makes
     * any_current_present run a deliberately sensitive, counts-domain
     * fallback (delta_counts > a small fixed margin) rather than a real
     * amps comparison -- "a presence heuristic, not a measurement." That
     * heuristic is fine for a WARN-class guard (S3/S4) or for merely arming
     * a check (S11) -- a false positive there costs nothing worse than an
     * eager watch. It is NOT fine as the sole evidence for S9's
     * trip_ineffective, which is the one latch in this whole module that is
     * never clearable by anything short of power removal at the breaker
     * (SAFETY_MODEL.md section 4, S9). An uncalibrated CT's DC offset floor
     * reads as "current present" on every tick, forever -- not a transient
     * a debounce can filter -- so without this flag an uncommissioned board
     * bricks itself on the first trip of any kind. See safety_guards.c's S9
     * block for how this is used: it does not silence the finding
     * (SAFETY_MODEL.md section 4 calls S9 "the single most valuable guard
     * after S1" specifically because it turns a silent failure into a loud
     * one), it only gates which RESPONSE CLASS the finding gets -- WARN
     * (s9_uncommissioned_warn, non-latching, exactly this file's own S4/S10
     * idiom) while uncommissioned, TRIP-ESCALATE once commissioned. */
    bool current_sensing_commissioned;

    /* S3/S4/S9/S14. True iff the operator has EXPLICITLY declared that this
     * board has no current transformers fitted (config_store.h's
     * ct_installed == 0 *with* CONFIG_STORE_SET_CT_INSTALLED set -- an
     * unanswered question is never "disabled").
     *
     * Named with the INVERTED sense, the same choice and for the same reason
     * as safety_tc_not_installed_declared above: false is the fully-armed
     * state, so a zero-initialised input struct -- every existing host test,
     * every future caller that forgets this field -- keeps the normal guard
     * behaviour, and only a deliberate positive assertion disarms anything.
     *
     * What it MEANS, precisely: `any_current_present` and `amps[]` carry no
     * information at all on this board. Not "no current" -- NO SENSOR. The
     * ADC channels still read a number (the AD8542's DC offset floor,
     * CURRENT_SENSE.md section 1), and current_presence_policy.c's
     * uncalibrated counts-domain fallback reads that floor as "current
     * present" on every tick forever. Left alone, that would make S3 (a
     * TRIP) fire on a healthy CT-less board within stuck_on_time_s. So this
     * flag is not a convenience: without it, "no CT fitted" is not merely
     * unprotected, it is actively unsafe-by-nuisance.
     *
     * What each guard does with it (safety_guards.c has the per-guard
     * detail):
     *
     *   S3  LOAD_STUCK_ON (TRIP)  -> INERT. Reports inactive, never
     *                               "passing": there is no evidence either
     *                               way about a welded SSR on this board.
     *   S4  load-inactive (WARN)  -> INERT. Otherwise it would assert
     *                               permanently on every firing, which is
     *                               worse than silence: a warning that is
     *                               always on trains an operator to ignore
     *                               warnings.
     *   S9  TRIP_INEFFECTIVE      -> INERT, and distinctly so. Already
     *                               gated by current_sensing_commissioned
     *                               (k_ct_v_per_a == 0 on a CT-less board),
     *                               but that gate reports
     *                               s9_uncommissioned_warn -- "finish
     *                               commissioning and this comes back",
     *                               which on a CT-less board is a lie. This
     *                               flag takes precedence and reports
     *                               ct_guards_disabled instead.
     *   S14 over-current (WARN)   -> INERT per channel.
     *
     * What it deliberately does NOT do: S6b. That guard's SOFT (link_timeout_s)
     * trip is keyed on any_current_present, so on a CT-less board it becomes
     * unreachable and S6b degrades to its unconditional link_dead_hard_s
     * backstop alone. This is a REAL loss of protection and it is left in
     * place rather than papered over, because the only local substitute for
     * "is heat on" that does not require the (by definition dead) link is
     * SaftyFW's own K4 energization state, and re-keying a TRIP-class guard
     * onto a different input is a change to what S6b MEANS -- SAFETY_MODEL.md
     * section 4's decision, not this pass's. Recorded in
     * docs/GUARD_TEST_MATRIX.md as the known, accepted degradation of
     * running without CTs. */
    bool current_sensing_disabled;

    /* S13. sample_counter_advancing is already the caller's comparison of
     * this tick's context frame's per-zone sample_counter against the last
     * one seen (SAFETY_MODEL.md section 4, S13) -- this module has no
     * notion of "last frame" to compare against on its own, by design (it
     * has no I/O and would have to keep frame history to do that itself,
     * which is exactly the kind of state this module tries not to own). */
    bool sample_counter_advancing;

    /* S6a. mainFault (GPIO10, active low), already debounced 200ms by
     * discrete_task -- same division as estop_pressed above. */
    bool main_fault_asserted;

    /* S6a startup grace (2026-09-07, fc6d30f8 flash incident): a plain,
     * already-computed fact -- "the Pico's own to_ms_since_boot() clock is
     * still inside its post-reset startup window" -- same discipline as
     * reboot_grace_active just below (this module has no clock of its own;
     * safety_core.c's safety_core_build_input() is the only place allowed to
     * compare now_ms against S6A_STARTUP_GRACE_MS).
     *
     * Why this exists and why it is NOT the same field as reboot_grace_active:
     * ANNOUNCE_REBOOT only arrives when the ESP deliberately tells the Pico a
     * reboot is coming (an OTA self-restart) -- a debug-probe-driven reset of
     * BOTH processors together (e.g. a bench reflash) sends no such frame, so
     * reboot_grace_active is false the entire time. But GPIO6 ("Fault") is an
     * ESP output the ESP's own firmware has to actively drive to the healthy
     * (low) level; before that firmware runs, GPIO6's state is whatever the
     * ESP's boot ROM/second-stage loader leaves it at, which HARDWARE.md
     * section 4 does not characterize for the "ESP mid-boot" case (only
     * "ESP absent" is documented, and that reads healthy). A Pico reset that
     * coincides with an ESP reset -- exactly the fc6d30f8 incident -- can
     * therefore see mainFault read asserted for the whole time the ESP takes
     * to reach its own GPIO6 init, comfortably longer than the 200ms
     * consecutive-sample debounce discrete_task.c applies (that debounce is
     * sized for opto/switch bounce, not a multi-hundred-ms MCU boot).
     *
     * Scope is exactly as narrow as reboot_grace_active's: this ONLY gates
     * the S6a trip() call below. It does not touch discrete_task's debounce,
     * does not weaken S6a once the window closes (the very next tick after
     * S6A_STARTUP_GRACE_MS trips on a still-asserted mainFault exactly as if
     * this field had never existed), and grants no heating permission --
     * relay_owner has no path to this field. Because it is keyed to the
     * Pico's OWN to_ms_since_boot() clock rather than a stored timestamp
     * that could re-arm, it also cannot be re-triggered mid-run: it is true
     * only once, for the first S6A_STARTUP_GRACE_MS after this boot, and
     * false for the remainder of this boot's lifetime. */
    bool s6a_startup_grace_active;

    /* S6b. link_up is the producer's "a valid frame arrived within
     * link_timeout_s" fact -- this module still needs its own elapsed-time
     * accumulator for the two-tier timeout (10s conditional / 120s
     * unconditional backstop), so it is a level (true/false per tick), not
     * a duration, deliberately mirroring how estop_pressed is a level too. */
    bool link_up;

    /* S6b, ANNOUNCE_REBOOT grace window (KilnFW/TODO.md's "SAFETY_CMD_
     * ANNOUNCE_REBOOT sent before the ESP reboots" line). A plain,
     * already-computed fact -- "the ESP told us within the last
     * reboot_grace_window_s that a reboot was coming, and we're still inside
     * that window" -- never a timestamp or a duration this module would have
     * to do its own clock math on, matching every other input in this
     * struct's own discipline (this module has no clock, no link, no flash
     * access at all; see this file's own isolation notes and safety_core.c's
     * safety_core_build_input(), the only place that is allowed to compute
     * this by comparing reboot_announce_get()'s timestamp against "now").
     *
     * Scope is narrow and deliberate: this ONLY suppresses S6b's own trip
     * condition below. It does not change link_up itself (the elapsed-time
     * accumulator below still advances normally while link_up is false, so
     * the guard's memory of how long the link has actually been silent stays
     * accurate), does not touch any other guard, and grants no heating
     * permission of any kind -- relay_owner's energize/ARM logic has no path
     * to this field at all. If this flips back to false (window expired)
     * while link_up is still false, the very next tick trips exactly as it
     * would have if this field had been false the whole time -- see the S6b
     * block's own comment for how that "no accumulated advantage" property
     * is achieved (by never resetting the elapsed accumulator, only gating
     * the trip() calls). */
    bool reboot_grace_active;

    /* S9. Set by the caller once relay_owner has actually de-energized K4
     * (SAFETY_MODEL.md section 4, S9: "tripped (K4 de-energized) for
     * trip_verify_s"). Distinct from state->is_tripped, which this module
     * already tracks itself -- relay_deenergized answers "did the hardware
     * actually respond", which only the caller (relay_owner) can know. */
    bool relay_deenergized;

    /* S14. Per-channel current magnitude, its own validity flag, and
     * whether that channel's mapped relay (ct_channel_map[ch]) is commanded
     * on right now. Flattened scalars with their own validity, matching this
     * struct's existing convention (any_current_present etc.) and keeping
     * this module link-header-free -- safety_core does the ct_channel_map
     * lookup and hands over three plain per-channel facts, not the map
     * itself. amps_valid[ch] == false means "no reading this tick", which
     * (like i_normal_valid above) leaves the channel inactive rather than
     * defaulting the reading to 0.0f and reading 0 < i_normal_a as fine. */
    float amps[3];
    bool  amps_valid[3];
    bool  relay_commanded_now_for_ct[3];

    /* S14 (summed topology only)/S15. Per-ZONE commanded-now facts,
     * independent of ct_channel_map -- deliberately a second array rather
     * than reusing relay_commanded_now_for_ct[3] above, because in summed
     * mode every channel maps to the SAME shared CT (channel 2), so the
     * ct_channel_map-indexed array above cannot answer "which zones are
     * on" for zones 0/1 the way it can in per_zone mode. safety_core reads
     * this straight from ctx.relay_now_mask by zone id, the same source
     * relay_commanded_now_for_ct's per-channel lookup already uses. Unused
     * (left false) when cfg->ct_topology_summed is false -- per_zone mode's
     * S14 keeps using relay_commanded_now_for_ct exactly as before. */
    bool  relay_commanded_now_for_zone[3];

    float dt_s;
} safety_guard_input_t;

typedef struct {
    bool          is_tripped;
    safety_trip_t reason;
    char          detail[96];

    /* S1: consecutive valid over-ceiling readings. */
    uint8_t s1_over_ceiling_streak;

    /* S5: consecutive-bad-read count and cumulative bad time (both must
     * clear before WARN; the same running elapsed timer continues past
     * WARN toward blind_grace_s for the TRIP promotion). */
    uint16_t s5_bad_streak;
    float    s5_bad_elapsed_s;
    bool     s5_warn; /* WARN state active -- caller clears TEMP_VALID while this is true */
    bool     s5_not_installed; /* true while blind_grace_s has been exceeded
                                 * AND in->safety_tc_not_installed_declared was
                                 * true on the tick that happened -- the
                                 * caller's cue to report "safety TC declared
                                 * not installed, heat blocked" rather than a
                                 * clean status, distinct from s5_warn (which
                                 * also covers the ordinary, expected-to-
                                 * recover intermittent-connector case).
                                 * Cleared the instant a read is good again,
                                 * same as s5_warn. */

    /* S11: value-identity window, gated on heat_commanded. */
    bool  s11_window_active;
    float s11_last_c;
    float s11_elapsed_s;

    /* S12: sustained-over-cj_max_c timer; s12_warn tracks the (non-latching,
     * non-timed) cj_warn_c crossing. */
    bool  s12_warn;
    float s12_over_max_elapsed_s;

    /* S8: baseline-sample-and-hold rate-of-rise window. s8_window_start_c is
     * the reading at the start of the current rate_window_s window;
     * s8_window_elapsed_s accumulates toward rate_window_s, at which point
     * the average rate over the window is evaluated and the window slides
     * (never grows without bound). s8_over_rate_streak counts consecutive
     * WINDOW EVALUATIONS (not ticks) whose average rate exceeded
     * max_rate_c_per_min -- see safety_guards.c's S8 block for why a single
     * over-threshold window is not enough to trip on its own. */
    bool    s8_window_active;
    float   s8_window_start_c;
    float   s8_window_elapsed_s;
    uint8_t s8_over_rate_streak;

    /* S2: sustained-over-setpoint timer. */
    float s2_over_elapsed_s;

    /* S3: sustained "current present, nothing recently commanded" timer. */
    float s3_stuck_elapsed_s;

    /* S4: WARN only, no timer -- level-tracked for the caller's telemetry. */
    bool s4_warn;

    /* S6a/S6b: independent elapsed timers -- (a) is a debounce-then-trip
     * with no further state needed beyond the input already being
     * debounced upstream, so it needs none here; (b) needs its own
     * link-down elapsed accumulator, reset on every tick link_up is true. */
    float s6b_link_down_elapsed_s;

    /* S9: elapsed time since relay_deenergized first went true (tracked
     * independently of is_tripped, since a trip and a *verified* trip are
     * different moments -- SAFETY_MODEL.md section 4, S9's own
     * trip_verify_s window starts at de-energization, not at the original
     * guard trip). trip_ineffective latches separately from is_tripped so
     * the caller can distinguish "tripped" from "tripped AND still
     * conducting", which SAFETY_MODEL.md section 4 says needs its own,
     * louder, escalation. */
    bool  s9_verify_active;
    float s9_verify_elapsed_s;
    /* Consecutive-tick counter for any_current_present once
     * s9_verify_elapsed_s has cleared trip_verify_s -- see
     * S9_CURRENT_PRESENT_STREAK_TO_TRIP's comment in the .c file. Reset to 0
     * whenever any_current_present or context_valid is false on a checked
     * tick, and whenever relay_deenergized itself goes false (the
     * verification window restarting from scratch). */
    uint8_t s9_current_present_streak;
    /* 2026-08-27 audit, second pass: level-tracked, non-latching WARN --
     * same idiom as s4_warn/s10_warn -- true while the verify window has
     * cleared trip_verify_s with current present but
     * in->current_sensing_commissioned is false, i.e. exactly the case this
     * module cannot trust enough to progress s9_current_present_streak
     * toward the unclearable trip_ineffective latch. Cleared the instant
     * the condition stops holding (current present is gone, OR current
     * sensing becomes commissioned and the streak takes over instead). */
    bool  s9_uncommissioned_warn;
    /* True on any tick evaluated with in->current_sensing_disabled set.
     * Purely a REPORTING field -- nothing in this module reads it back. It
     * exists so "S3/S4/S9/S14 did not fire" is distinguishable, at the
     * caller and on the wire, from "S3/S4/S9/S14 are not watching", which is
     * the whole difference between a guard passing and a guard being absent.
     * Non-latching, like every other *_warn level in this struct. */
    bool  ct_guards_disabled;
    bool  trip_ineffective;

    /* S10: WARN only, sustained-disagreement timer + level. */
    bool  s10_warn;
    float s10_disagree_elapsed_s;

    /* S13: graduated exactly like S5 -- elapsed time since
     * sample_counter_advancing last went true. */
    float s13_stale_elapsed_s;
    bool  s13_warn;

    /* S14: per-channel sustained-over-normal timer + level, WARN only.
     * Reset in the same !in->context_valid block as s3/s4 above -- S14
     * needs ct_channel_map (via relay_commanded_now_for_ct) and the
     * commanded-relay fact, both of which are context. */
    float s14_over_elapsed_s[3];
    bool  s14_warn[3];

    /* S15 (new, CT_COMMISSIONING_PLAN.md step 3): summed-topology-only,
     * per-zone sustained-under-current ("open heater") timer + level, WARN
     * only, same non-latching idiom as S4/S10/S14. Inert (stays at zero/
     * false) whenever cfg->ct_topology_summed is false -- per_zone mode has
     * no shared-CT deficit to attribute to a single zone. Reset in the same
     * !in->context_valid block as S14 above, for the same reason (needs
     * relay_commanded_now_for_zone, which is context).
     *
     * Per-element array, but the deficit it is compared against is ONE
     * shared-CT scalar (safety_guards.c): a single open heater can clear
     * more than one commanded zone's own threshold at once, so s15_warn[z]
     * true for several z does not mean several zones are faulty -- it means
     * the fault is consistent with any one of them. Report/read it as "one
     * of the commanded zones", never "each flagged zone", faulty. */
    float s15_under_elapsed_s[3];
    bool  s15_warn[3];
} safety_guard_state_t;

void safety_guards_reset(safety_guard_state_t *state);

/* Explicit operator acknowledgement -- the only way out of a latched trip
 * (SAFETY_MODEL.md section 2, "latching is not auto-recovery"). Resets
 * every window too, so the next tick starts clean, exactly like
 * thermal_guard_clear(). */
void safety_guards_clear(safety_guard_state_t *state);

/* Evaluates one tick. Returns true if this call is the one that newly
 * tripped it (state->is_tripped was false, now true) -- the caller uses
 * this to latch the relay / log / emit a trip-event frame exactly once, not
 * every tick afterward. Once tripped, further calls are no-ops that just
 * re-report is_tripped == true until safety_guards_clear(). */
bool safety_guards_tick(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                         const safety_guard_input_t *in);

// Attempts to clear a latched trip: resets guard state (safety_guards_clear),
// then immediately re-evaluates one tick against `in` (the caller's current
// input). If that re-evaluation retrips within this single tick, the clear
// is refused -- `state` is left freshly re-tripped (is_tripped=true, reason
// set, exactly as if safety_guards_tick() had just been called fresh) and
// this returns false. Otherwise the clear holds and this returns true.
//
// Honest limitation, read before calling this expecting more than it gives:
// only guards whose trip condition fires from a single tick's raw input with
// no accumulation window (S6a mainFault, S7 estop, S6b's hard backstop tier)
// are guaranteed to be caught here if their triggering condition is still
// present -- PLUS, as of the 2026-08-27 audit, S2/S3/S5/S11/S12/S13, whose
// conditions guard_condition_still_immediate() (safety_guards.c) recomputes
// directly from `in` (and, for S11, from `state` before it gets zeroed
// below) rather than trusting an accumulator that safety_guards_clear() is
// about to reset. S9/TRIP_INEFFECTIVE is refused unconditionally, before
// either of those checks runs -- see this function's own S9 comment; it has
// no "still present" condition to recompute because it is simply never
// clearable. S1 remains a genuine scope limit: it is a 3-tick debounce with
// no single-tick "value itself is disqualifying" test short of the trip
// condition itself, so if it is still over ceiling, it will re-trip again
// once its debounce re-accumulates on its own normal timescale, not
// necessarily on this exact call. That is not a safety hole (the guard
// still does its job), it is a scope limit of THIS function.
bool safety_guards_try_clear(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                              const safety_guard_input_t *in);

// Outcome of one CLEAR_TRIP (0x0A) request as resolved by
// safety_core_task's queue-drain loop (src/tasks/safety_core.c) -- lives
// here, not in safety_core.h, so safety_guards_decide_clear_trip_outcome()
// just below can be pure and host-tested the same way
// link_frame_decide_clear_trip() (src/tasks/link_frame.c) already is for
// link_task's OWN pair of CLEAR_TRIP refusal checks. safety_core.h re-uses
// this exact enum (it already includes this header) rather than defining a
// second, convertible one.
typedef enum {
    SAFETY_CLEAR_TRIP_OUTCOME_NONE = 0,                // nothing processed yet this boot
    SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED,                // cleared: TRIPPED -> ARMED
    SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED,   // safety_guards_try_clear() retripped
    SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED, // is_tripped was already false
                                                        // by the time the request was dequeued
} safety_clear_trip_outcome_t;

// Pure 3-way classification of a queued CLEAR_TRIP request's result, given
// the two facts safety_core_task already has in hand right after it dequeues
// one: whether a trip was still latched at the moment of dequeue, and (only
// if so) what safety_guards_try_clear() returned. Factored out purely so
// this classification is host-testable like the rest of this file --
// safety_core.c's own queue-drain loop is not (FreeRTOS-shaped), but the
// three-way decision it makes from those two booleans is exactly as pure as
// link_frame_decide_clear_trip()'s two-way one, and just as worth pinning
// down with a test as that one was (see CommonFW's/this project's "every new
// check must be proven capable of failing" standard).
safety_clear_trip_outcome_t safety_guards_decide_clear_trip_outcome(bool was_tripped,
                                                                      bool try_clear_result);

// Best-effort "what number decided this trip" for CommonFW/docs/LINK_PROTOCOL.md
// sec 6 Frame D's `deciding_threshold` field ("Capturing the deciding values
// at the instant of the trip is the whole point"). Pure function of (reason,
// cfg) -- deliberately NOT a function of live state, since the caller reads
// this once, right after safety_guards_tick() returns true, while `reason`
// still names what just tripped.
//
// Honest, partial coverage, not a claim of completeness: covers the six
// guards (S1/S2/S3/S11/S12/S13) that have one meaningful magnitude threshold
// each; returns NaN for the rest (S5's dual count+time bar, S6a/S7's boolean
// conditions, S6b's already-qualitative hard backstop, S9's escalation-not-a-
// fresh-threshold nature) rather than guessing. S1's abs_max_temp_c is
// reported as configured, not the possibly-tighter runtime
// min(abs_max_temp_c, firing_max_c + firing_margin_c) safety_guards_tick()
// computes internally and does not expose -- see the .c file's doc comment
// on the SAFETY_TRIP_OVERTEMP case for the exact caveat.
float safety_guards_deciding_threshold_c(safety_trip_t reason, const safety_guard_cfg_t *cfg);

// Opus review of 51c084f/c49bb0e, finding 1: s15_warn[]/s14_warn[] were read
// nowhere outside this module -- no DIAG warn-mask bit, no telemetry -- even
// though safety_core_get_diag_status()'s out_warn_active had already
// documented (falsely, by the time S14/S15 existed) that S5/S12 were "the
// only two guards in this build that have a WARN concept at all". This is
// the real per-guard mask LINK_PROTOCOL.md's Frame B has always specified
// ("one bit per guard currently warning"), replacing link_task_send_diag()'s
// former single-bit (bit0) degraded approximation. Pure and host-tested here
// (test_safety_guards.c) rather than in link_task.c/link_frame.c, since this
// module already owns every field it reads and safety_core.c is structurally
// forbidden from #include-ing anything link-shaped (docs/ARCHITECTURE.md
// section 2) -- safety_core_get_diag_status() calls this directly.
//
// Bit numbering: for every guard that already has a safety_trip_t code, the
// bit is (that code - 1), i.e. the exact same numbering link_frame_trip_mask_
// for_reason() uses for trip_mask -- S4/S10's codes are reserved gaps (4 and
// 11) specifically so a WARN-only guard has a slot here without colliding
// with any TRIP guard's bit. S14 and S15 (both added after safety_trip_t was
// written, CT_COMMISSIONING_PLAN.md) were never given a reserved trip code at
// all -- codes 15/16 went to CONFIG_CORRUPT/SELF_TEST, unrelated future
// guards -- so they take bits 14/15 directly: both bits have been 0 on every
// frame ever sent (warn_mask was always the single-bit approximation before
// this), so this is a genuinely unused-until-now pair of bits, not a reuse of
// anything live. S14/S15 are 3-channel/3-zone arrays in safety_guard_state_t;
// each gets exactly one mask bit (any channel/zone warning), matching
// trip_mask's existing per-guard-not-per-channel granularity.
//
//   bit 3  (S4)  s4_warn
//   bit 4  (S5)  s5_warn
//   bit 9  (S9)  s9_uncommissioned_warn
//   bit 10 (S10) s10_warn
//   bit 12 (S12) s12_warn
//   bit 13 (S13) s13_warn
//   bit 14 (S14) any(s14_warn[0..2])
//   bit 15 (S15) any(s15_warn[0..2])
//
// All other bits are always 0 in this build (no other guard has a WARN
// concept yet). See firmware/CommonFW/docs/LINK_PROTOCOL.md's Frame B table
// for the wire-level documentation of this same numbering.
uint16_t safety_guards_warn_mask(const safety_guard_state_t *state);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_GUARDS_H
