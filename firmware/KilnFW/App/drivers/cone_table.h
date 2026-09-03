// cone_table -- static Orton pyrometric cone reference and heat-work
// helpers. Foundation for a future dwell-credit feature (a slow ramp spends
// time near a target before the dwell nominally starts; that time already
// did ceramic "heat work" and should shorten the dwell that follows). This
// module does not implement dwell credit itself and is not wired into
// profile_executor.c -- it is deliberately self-contained, pure data plus
// pure math, so it can be unit-tested on the host before anything consumes
// it.
//
// Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O, no globals, no
// dynamic allocation -- same discipline as pid.h/thermo_combine.h (see
// pid.h's header comment for the rationale).
//
// -- Cone table source and heating rate --------------------------------
// Temperatures are Orton SELF-SUPPORTING cone equivalents at a heating rate
// of 108 F/hr (60 C/hr), the standard "medium speed" reference rate
// published in Orton's pyrometric cone charts (Edward Orton Jr. Ceramic
// Foundation, "Orton Cone Temperature Equivalents" chart; self-supporting
// column). A different heating rate shifts every value in this table --
// cones are rate-sensitive by design (that is the entire point of a cone:
// it integrates time-at-temperature, not just peak temperature) -- Orton
// also publishes 27 F/hr, 54 F/hr and 270 F/hr columns that give visibly
// different bending temperatures for the same cone. 108 F/hr was chosen
// because it is the rate Orton's own literature treats as the general
// reference and is a reasonable approximation of a mid/late kiln firing
// segment's ramp rate. If a caller ever needs a different reference rate,
// that is a new table, not a scaling of this one.
//
// Cone spacing is NOT uniform (bottom-of-range cones are packed roughly
// 15-70 C apart; top-of-range cones spread out to 20-40 C) -- this is
// preserved exactly from the source data and must never be smoothed,
// rounded aggressively, or interpolated when adding/adjusting entries.
//
// -- Heat-work weight model ----------------------------------------------
// cone_table_heat_work_weight() models the *rate* at which a soak below a
// target contributes ceramic heat work, normalised to 1.0 at the target
// temperature itself and falling toward 0 at the bottom of the half-cone-
// step band. It uses an Arrhenius form, rate ~ exp(-Ea / (R * T)) with T in
// KELVIN (never Celsius -- the whole point of the Arrhenius form is that it
// is not linear in temperature, and using Celsius in the exponent would
// silently reintroduce a false near-linear approximation), normalised by
// dividing by the rate at the target so the result is exactly 1.0 there.
//
// Activation energy: Ea = 300000 J/mol (300 kJ/mol). This is an
// ENGINEERING APPROXIMATION, not certified Orton kinetics data -- Orton
// does not publish a single activation energy for cone bending (it is a
// glass-viscosity/sintering phenomenon, not one Arrhenius-obeying chemical
// reaction), and real ceramic heat-work models used in the industry (e.g.
// the Hyman/Nordberg-style "cone equivalent" formulas) fit in the same
// rough 250-450 kJ/mol range depending on body chemistry. 300 kJ/mol was
// chosen because it reproduces roughly the right SENSE of the cone table's
// own behaviour at this module's normalisation: over the ~15-20 C spacing
// between adjacent cones in the 016-06 range, and evaluated near typical
// ^6 stoneware target temperatures (~1200-1230 C = ~1473-1503 K), it
// predicts each single adjacent cone step corresponds to roughly a 2x-4x
// change in instantaneous work rate -- consistent with the qualitative
// fact that cones this close together represent meaningfully different
// "how done is it" points, not a near-flat rate. This is a deliberately
// simple single-Ea approximation across the whole table; it is not fit
// against real vitrification data and should not be treated as such.
#ifndef CONE_TABLE_H
#define CONE_TABLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of cones in the static table (cone 022 .. cone 14 inclusive).
#define CONE_TABLE_COUNT 36

// Result codes shared by every lookup/derived-value function in this
// module. Deliberately explicit rather than a sentinel float (NAN, -1,
// etc.) so a caller cannot silently treat an error as a plausible value --
// see cone_table.c's individual function comments for which failure modes
// map to which code.
typedef enum {
    CONE_TABLE_OK = 0,
    CONE_TABLE_ERR_OUT_OF_RANGE_LOW,  // below the lowest cone (022)
    CONE_TABLE_ERR_OUT_OF_RANGE_HIGH, // above the highest cone (14)
    CONE_TABLE_ERR_INVALID_INPUT,     // NaN/Inf input, or a malformed cone index
} cone_table_status_t;

// One entry: an Orton cone number and its self-supporting, 108 F/hr
// (60 C/hr) equivalent temperature in Celsius. `index` is this table's own
// ordinal (0 == cone 022, CONE_TABLE_COUNT-1 == cone 14), monotonically
// increasing with temperature -- NOT the cone's own numbering, which resets
// sign convention between the "0xx" cones (022 down to 01, counting DOWN
// toward hotter) and the plain-numbered cones (1 up to 14, counting UP
// toward hotter). Callers that need "which cone is this" should use
// cone_table_get(index)->cone_label or cone_table_cone_for_temp_c(), never
// try to reconstruct the label from the index arithmetically.
typedef struct {
    const char *cone_label; // e.g. "022", "06", "6" -- matches Orton usage
    float temp_c;           // self-supporting equivalent temperature, deg C
} cone_table_entry_t;

// Returns the number of entries in the static table (always
// CONE_TABLE_COUNT). Provided so a caller can iterate without depending on
// the macro directly.
int cone_table_count(void);

// Returns a pointer to table entry `index` (0-based, 0 == cone 022,
// count-1 == cone 14), or NULL if index is out of [0, count) range. The
// returned pointer refers to static storage and is valid for the life of
// the program.
const cone_table_entry_t *cone_table_get(int index);

// Looks up the self-supporting equivalent temperature (deg C) for a named
// cone label (e.g. "6", "06", "022" -- exact string match against
// cone_label, case-sensitive). On success returns CONE_TABLE_OK and writes
// *out_temp_c. On no match returns CONE_TABLE_ERR_INVALID_INPUT and leaves
// *out_temp_c unmodified. `cone_label` NULL is also
// CONE_TABLE_ERR_INVALID_INPUT.
cone_table_status_t cone_table_temp_c_for_cone(const char *cone_label, float *out_temp_c);

// Looks up which cone (if any) has the closest equivalent temperature at
// or below `temp_c`, i.e. the hottest cone that `temp_c` has reached or
// exceeded. On success returns CONE_TABLE_OK and writes *out_index (usable
// with cone_table_get()). If temp_c is below the lowest cone's temperature,
// returns CONE_TABLE_ERR_OUT_OF_RANGE_LOW. If temp_c is NaN/Inf, returns
// CONE_TABLE_ERR_INVALID_INPUT. Note temp_c above the highest cone is NOT
// an error here (it means "at least cone 14 has been reached") -- it
// returns CONE_TABLE_OK with *out_index == CONE_TABLE_COUNT-1. Callers that
// need to distinguish "above cone 14" explicitly should compare temp_c
// against cone_table_get(CONE_TABLE_COUNT-1)->temp_c themselves.
cone_table_status_t cone_table_cone_for_temp_c(float temp_c, int *out_index);

// Half-cone-step band bottom: given a target temperature `target_c`
// (which may fall exactly on a tabulated cone or between two), returns in
// *out_band_bottom_c the temperature halfway from target_c DOWN toward the
// equivalent temperature of the next lower cone. Because cone spacing is
// non-uniform, this half-step width varies with where target_c falls in
// the table -- that is intentional and must not be collapsed into a fixed
// offset.
//
// target_c between two tabulated cones is handled by linearly interpolating
// the "next lower cone" temperature at target_c's position (i.e. the lower
// bracketing cone is used directly; target_c does not have to be an exact
// table entry).
//
// Returns CONE_TABLE_ERR_OUT_OF_RANGE_LOW if target_c is at or below the
// lowest tabulated cone (there is no lower cone to measure a band against).
// Returns CONE_TABLE_ERR_OUT_OF_RANGE_HIGH if target_c is above the highest
// tabulated cone. Returns CONE_TABLE_ERR_INVALID_INPUT for NaN/Inf input.
cone_table_status_t cone_table_band_bottom_c(float target_c, float *out_band_bottom_c);

// Heat-work weight: given the current temperature `current_c` and a target
// temperature `target_c`, returns in *out_weight a relative work rate in
// [0, 1] -- 1.0 at current_c == target_c, falling toward 0.0 at (and below)
// the half-cone-step band bottom (cone_table_band_bottom_c(target_c)), using
// the Arrhenius form documented at the top of this file. current_c above
// target_c clamps to 1.0 (this module has no opinion on overshoot credit).
//
// Returns CONE_TABLE_ERR_INVALID_INPUT for NaN/Inf current_c or target_c,
// or if either temperature is at/below absolute zero (-273.15 C) -- the
// Arrhenius form divides by absolute temperature in Kelvin, so this guards
// the division as well as physical sense. Otherwise propagates whatever
// cone_table_band_bottom_c(target_c) returns (out-of-range target_c is an
// error here too, since there is no band to weight against).
cone_table_status_t cone_table_heat_work_weight(float current_c, float target_c, float *out_weight);

#ifdef __cplusplus
}
#endif

#endif // CONE_TABLE_H
