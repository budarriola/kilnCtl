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
// silently reintroduce a false near-linear approximation).
//
// NORMALISATION -- read this before touching cone_table.c's weight formula.
// The code does NOT divide by the rate at the target (that wording shipped
// here originally and was wrong -- it never matched the implementation).
// What it actually computes is a MIN-MAX RESCALE across the band:
//     weight = (rate_current - rate_bottom) / (rate_target - rate_bottom)
// i.e. 0.0 at the band bottom's own rate and 1.0 at the target's rate,
// linearly in *rate* space (which is still nonlinear in temperature, since
// rate itself is the Arrhenius exponential). This under-credits everywhere
// below the target compared to the "divide by rate at target" wording,
// because it also subtracts off the (nonzero) rate already present at the
// band bottom instead of treating that as part of the credit. That is the
// CONSERVATIVE direction for a dwell-credit feature: it produces a LOWER
// weight for a given current_c, which (once this module is wired into a
// dwell-shortening decision) means a longer dwell and an under-fire risk
// rather than an over-fire risk. Do NOT "fix" the code to match the old
// wording -- see the Ea note below for the size of that mistake. If this
// ever needs correcting, correct the comment (as this pass did), not the
// arithmetic, unless the conservative-direction tradeoff above is
// explicitly revisited and re-justified.
//
// Activation energy: Ea = 300000 J/mol (300 kJ/mol). This is an
// ENGINEERING APPROXIMATION, not certified Orton kinetics data -- Orton
// does not publish a single activation energy for cone bending (it is a
// glass-viscosity/sintering phenomenon, not one Arrhenius-obeying chemical
// reaction), and real ceramic heat-work models used in the industry (e.g.
// the Hyman/Nordberg-style "cone equivalent" formulas) fit in the same
// rough 250-450 kJ/mol range depending on body chemistry.
//
// This value is NOT consistent with this table's own time-temperature
// trade-off, and an earlier version of this comment claimed it was --
// that claim was never checked against the table's own rate columns.
// Inverting the table's own heating-rate spacings (27->108 and 108->270
// F/hr steps) gives per-cone apparent activation energies far above
// 300 kJ/mol across the working range -- roughly 500-1000+ kJ/mol
// depending on cone and step (measured examples: cone 014 ~257-299
// kJ/mol, cone 06 ~502-1102 kJ/mol, cone 04 ~951-1219 kJ/mol, cone 6
// ~531-675 kJ/mol, cone 10 ~808-937 kJ/mol). 300 kJ/mol was chosen only
// because it reproduces roughly the right SENSE of the table's behaviour
// at THIS module's own min-max normalisation (see above) -- over the
// ~15-20 C spacing between adjacent cones in the 016-06 range, evaluated
// near typical ^6 stoneware target temperatures (~1200-1230 C), it
// predicts each single adjacent cone step corresponds to roughly a 2x-4x
// change in instantaneous work rate, which is qualitatively plausible but
// was never fit against the table's actual rate data.
//
// The min-max normalisation above absorbs most of this discrepancy rather
// than propagating it 1:1 into the weight, but NOT to a single figure --
// the error scales with band width, and band width is wildly non-uniform
// across this table (see the non-uniform-spacing note above). Computed
// per-band (integral-mean weight across each band, Ea=300 kJ/mol vs a
// table-consistent reference) across all 37 bands in the table: against a
// 700 kJ/mol reference the over-credit ranges from ~1.0% (cone 2, an
// 8-degree-wide band) to ~32.0% (cone 019, a 51.5-degree-wide band),
// table-wide mean ~6.8%; against the 1000 kJ/mol end of the same
// reference range it ranges roughly 1.8%-62.7%, mean ~12.6%. The cone-6
// band specifically (the one most often quoted as "the" figure) reads
// ~3.7%/~6.6% at 700/1000 kJ/mol now that the 5.5 half-cone narrows it
// (previously, before the half-cones were added to this table, cone 6's
// wider band -- bracketed by cone 5 instead of cone 5.5 -- read ~7.27%/
// ~13.3% at 700/1000 kJ/mol; that older, wider-band number is what earlier
// revisions of this comment quoted as a single "~7%" bound). There is no
// single honest error bound today -- report the range, not one number,
// and note it will keep moving as bands are added or narrowed. If the
// normalisation is ever changed to the "divide by rate at target" form
// the old wording described (see above -- do not do this without also
// revisiting Ea), the two errors compound rather than cancel: the min-max
// rescale's conservatism is what is currently absorbing most of the Ea
// mismatch, so removing it without correcting Ea would roughly double
// every figure above. This is a deliberately simple single-Ea
// approximation across the whole table; it is not fit against real
// vitrification data and should not be treated as such.
//
// NOTE on the band-cliff fix (DEFECT 2, cone_table_band_bottom_c()) and Ea
// sensitivity: bands that used to be degenerate at ~0 width under the old
// (broken) "distance to lower cone" formula had ~0% Ea sensitivity too --
// a near-zero-width band's weight is dominated by boundary clamping, not
// by the Arrhenius shape, so the Ea error above barely showed up there.
// Fixing the cliff widened exactly those bands back out to their real
// local-spacing width, which is what now exposes their full Ea
// sensitivity in the range above -- the fix repaired the band-width bug
// and, as a side effect, raised how much this table's numbers actually
// depend on the still-unvalidated Ea choice in that same region.
#ifndef CONE_TABLE_H
#define CONE_TABLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of cones in the static table (cone 022 .. cone 14 inclusive, plus
// the two half-cones 05.5 and 5.5 that Orton's self-supporting chart also
// publishes -- labelled "05HALF" and "5HALF" in s_cones[], see cone_table.c).
#define CONE_TABLE_COUNT 38

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
// *out_band_bottom_c the temperature target_c minus half the LOCAL cone
// spacing at that point in the table. Because cone spacing is non-uniform,
// this half-step width varies with where target_c falls in the table --
// that is intentional and must not be collapsed into a fixed offset.
//
// "Local spacing" means the gap between the BRACKETING PAIR of table
// entries around target_c, not the distance from target_c down to the
// lower one:
//     lower = hottest tabulated entry strictly below target_c
//     upper = the entry immediately above `lower` (always exists -- see
//             below)
//     half  = (upper - lower) / 2
//     *out_band_bottom_c = target_c - half
//
// A PRIOR VERSION of this function computed half = (target_c - lower) / 2
// instead -- i.e. half the distance from target_c down to the lower cone,
// not half the local spacing. That formula collapses to a near-zero band
// for any target a hair above a tabulated cone: target 1222.21 C (0.01 C
// above cone 6's 1222.2 C) produced a 0.005 C band instead of the ~8.35 C
// the surrounding cone spacing (cone 6->cone 7 is 1222.2->1238.9) implies,
// while target 1222.19 C (0.01 C below the same cone) produced an 18.045 C
// band -- a 3600x difference in dwell credit from a 0.02 C difference in
// what the user typed. This was DEFECT 2 of the ramp-assist ^6 review; see
// PID_EXPANSION_PLAN.md sec7.3 for the numeric writeup and
// tools/PcTools/tests/test_cone_table.py for the regression test that pins
// the corrected widths (1222.19 -> 18.05, 1222.21 -> 8.35, 1223.00 -> 8.35).
//
// target_c exactly on a tabulated cone (other than the first or last) is
// NOT treated as the midpoint of the spacing on both sides of it -- the
// search above finds `lower` strictly below target_c, so an exact match
// becomes the UPPER bracket and the cone below it becomes `lower`; the
// band width is half the spacing BELOW that cone, not an average of the
// spacing above and below. This is a deliberate, simple choice (matching
// the one already validated in ramp_assist.py's local workaround) rather
// than an attempt to average both neighbours.
//
// The upper bracket always exists when this function does not return an
// error: target_c <= the highest tabulated cone (checked below) together
// with `lower` being strictly below target_c means `lower` cannot be the
// last table entry, so `lower + 1` is always a valid index.
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
