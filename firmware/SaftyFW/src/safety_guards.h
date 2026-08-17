// safety_guards -- the safety processor's pure guard-evaluation module.
// TODO.md Phase 4.
//
// Pure function of (config, input, state) -> verdict, same host-testability
// reasoning as KilnFW's thermal_guard.h/pid.h: no FreeRTOS, no pico-sdk, no
// logging, no I/O, no time source of its own -- dt_s comes in as an argument
// every tick, inside the input struct (matching thermal_guard_input_t's own
// convention).
//
// This phase implements exactly 5 of the 13 guards in docs/SAFETY_MODEL.md
// section 4 -- the ones that need neither the isolated link's context frame
// nor current-sense calibration, and so can run correctly with nothing more
// than a thermocouple snapshot and two debounced discretes:
//
//   S1  Absolute over-temperature            TRIP
//   S5  Safety thermocouple invalid           WARN, then TRIP (graduated)
//   S7  E-stop                                TRIP
//   S11 Frozen safety reading                 TRIP
//   S12 Cold junction / enclosure over-temp    WARN, then TRIP (graduated)
//
// NOT implemented here: S2, S3, S4, S6, S8, S9, S10, S13 -- they need the
// context_snapshot_t from link_task, current_snapshot_t from current_task,
// or both. docs/SAFETY_MODEL.md section 4 has the full guard suite.
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
    SAFETY_TRIP_OVERTEMP        = 1,  /* S1  -- implemented here */
    SAFETY_TRIP_OVER_SETPOINT   = 2,  /* S2  -- not this phase */
    SAFETY_TRIP_LOAD_STUCK_ON   = 3,  /* S3  -- not this phase */
    /* 4 reserved: S4 is WARN-only */
    SAFETY_TRIP_SENSOR_INVALID  = 5,  /* S5  -- implemented here, after blind_grace_s */
    SAFETY_TRIP_MAIN_FAULT      = 6,  /* S6a -- not this phase */
    SAFETY_TRIP_LINK_DEAD       = 7,  /* S6b -- not this phase */
    SAFETY_TRIP_ESTOP           = 8,  /* S7  -- implemented here */
    SAFETY_TRIP_RATE            = 9,  /* S8  -- not this phase */
    SAFETY_TRIP_INEFFECTIVE     = 10, /* S9  -- not this phase */
    /* 11 reserved: S10 is WARN-only */
    SAFETY_TRIP_FROZEN_SENSOR   = 12, /* S11 -- implemented here */
    SAFETY_TRIP_ENCLOSURE_TEMP  = 13, /* S12 -- implemented here */
    SAFETY_TRIP_BORROWED_STALE  = 14, /* S13 -- not this phase */
    SAFETY_TRIP_CONFIG_CORRUPT  = 15, /* not this phase */
    SAFETY_TRIP_SELF_TEST       = 16, /* not this phase */
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

    /* S7. Already debounced (50ms) by discrete_task -- ARCHITECTURE.md's
     * module table gives debouncing to discrete_task, not to this pure
     * evaluator, the same division profile_executor.c / thermal_guard.c
     * use on the KilnFW side. */
    bool estop_pressed;

    /* S11's "and heat is happening" qualifier (SAFETY_MODEL.md section 4,
     * S11), named for exactly what it is rather than for where it might one
     * day come from: a plain fact the caller supplies, not link-derived
     * data. In this phase nothing wires it to anything real (no current
     * sense, no link context), so a caller with no better information
     * should pass false. That is not a workaround -- false is the honest,
     * conservative answer to "do you know heat is happening?" when the
     * answer is "no", and passing it makes S11 correctly stay dormant on an
     * idle kiln (the exact nuisance case SAFETY_MODEL.md section 4 warns
     * about: "a cold, idle kiln legitimately sits at a constant reading for
     * hours") without the guard ever having to guess or silently drop its
     * own qualifier. Once current sensing or the link context exists,
     * wiring a real "duty commanded" or "current present" signal in here
     * is a one-line change at the call site, not a change to this module. */
    bool heat_commanded;

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

    /* S11: value-identity window, gated on heat_commanded. */
    bool  s11_window_active;
    float s11_last_c;
    float s11_elapsed_s;

    /* S12: sustained-over-cj_max_c timer; s12_warn tracks the (non-latching,
     * non-timed) cj_warn_c crossing. */
    bool  s12_warn;
    float s12_over_max_elapsed_s;
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

#ifdef __cplusplus
}
#endif

#endif // SAFETY_GUARDS_H
