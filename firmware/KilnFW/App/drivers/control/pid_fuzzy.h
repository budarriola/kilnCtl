// pid_fuzzy -- optional fuzzy-adjustment layer over pid.c's classic gains.
// PID_EXPANSION_PLAN.md §2b / §4 Phase 3.
//
// Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O, no globals -- same
// discipline as pid.c, for the same reason: host-testable before it ever
// runs against a real kiln.
//
// What this is: a fixed, firmware-wide Mamdani fuzzy-rule table over two
// inputs (this zone's own error and its rate of change) that nudges the
// three PID gains up or down around whatever base gains Autotune measured.
// It is a *second selectable mode*, not a replacement for classic PID --
// selecting it does not change the base gains, it only lets them drift
// within a caller-bounded range while firing, so one set of tuning numbers
// keeps working across a temperature range wider than the point Autotune
// ran at.
//
// Why the rule table is a compile-time constant, not per-zone config
// (PID_EXPANSION_PLAN.md §2b/§4 Phase 2): a per-installation editable rule
// table reopens the trial-and-error tuning problem this whole mode exists
// to avoid. The one knob a caller gets is strength_pct -- "how far may this
// drift from what Autotune measured" -- not the rules themselves.
//
// Why there is no neighbor-zone / cross-zone-coupling input here
// (PID_EXPANSION_PLAN.md §2c): coupling is a *measured disturbance*, and
// the plan is explicit that a measured disturbance belongs on the
// feedforward term, not folded into the feedback gains. Routing a
// neighbor's temperature through Kp/Ki/Kd here would double-count an effect
// already visible in this zone's own error (that is what "coupled" means),
// and biasing Ki in response to a disturbance is the textbook route to
// integral windup on a plant with this much dead time. Do not add a
// neighbor parameter to this function -- add the coupling term to the
// feedforward path instead (§2c / Phase 3b).
#ifndef PID_FUZZY_H
#define PID_FUZZY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rule table (PID_EXPANSION_PLAN.md §3.3's "read-only 3x3 rule-strength
// summary" -- this comment block is the source of truth the UI renders,
// not the numeric constants in the .c file, in case those are ever
// re-derived).
//
// Two inputs, three triangular-membership buckets each:
//   error       (degC, setpoint - measurement): NEG / ZERO / POS
//   error_rate  (degC/s, d(error)/dt):          FALLING / STEADY / RISING
//
// For each of the 9 cells, direction each gain is nudged away from its base
// value (up / no change / down). NEG and POS are deliberately symmetric --
// "far from target" should behave the same whether the zone is running hot
// or cold, only the sign of the resulting duty change differs, and that
// sign already comes from error itself inside pid_update(), not from here.
//
//   error \ rate |  FALLING          |  STEADY           |  RISING
//   -------------+-------------------+--------------------+-------------------
//   POS (large)  | Kp -, Ki +, Kd -  | Kp +, Ki =, Kd =   | Kp +, Ki -, Kd +
//                | (already closing  | (steady approach,  | (far AND getting
//                |  in, ease off)    |  push harder)      |  worse: attack)
//   -------------+-------------------+--------------------+-------------------
//   ZERO         | Kp -, Ki -, Kd +  | Kp -, Ki +, Kd -   | Kp +, Ki -, Kd +
//                | (crossing target  | (settled: coast on | (just left
//                |  fast, damp)      |  I, ease P/D)       |  target, catch it)
//   -------------+-------------------+--------------------+-------------------
//   NEG (large)  | Kp +, Ki -, Kd +  | Kp +, Ki =, Kd =   | Kp -, Ki +, Kd -
//                | (overshoot still  | (steady overshoot, | (overshoot
//                |  growing: attack) | push harder)        |  recovering, ease)
//
// Rationale, in short: large-magnitude error favors more Kp/Kd and less Ki
// (a big, possibly-transient error should not be integrated aggressively --
// that is how windup starts); near setpoint favors more Ki and less Kp/Kd
// (fine settling, not chasing noise); a rate that says "getting worse"
// favors attacking harder (more Kp/Kd, less Ki so the response doesn't lag
// behind an accumulating integral); a rate that says "already recovering"
// favors easing off so the loop does not overshoot the correction itself.

// strength_pct: 0-100, how far the fuzzy layer may move the gains away from
// base_kp/base_ki/base_kd. strength_pct == 0 MUST reproduce the base gains
// exactly (bit-for-bit) -- that is the safety contract this mode rests on:
// at its most conservative setting it is indistinguishable from classic
// PID. 100 allows the full bounded nudge documented above.
//
// error_band_c / rate_band_c_per_s: the triangular-membership half-widths
// for the two axes above (ZONES_CFG_VERSION 18->19, PID_EXPANSION_PLAN.md
// sec 3.6g) -- per-zone config, resolved by the caller via
// zones_config_get_error_band_c()/zones_config_get_rate_band_c_per_s()
// BEFORE calling here (this function does not read zones config itself,
// same "no globals" discipline as the rest of this file). Must be > 0 and
// finite; a bad value is defended against internally (falls back to the
// documented firmware default, 20.0 degC / 0.5 degC/s) but callers should
// always pass an already-resolved value, not a raw possibly-zero-sentinel
// field. This is the width of the membership functions ONLY -- it does not
// change which rule fires for a given (error, rate) *within* whatever cell
// the rescaled axes land in; see pid_fuzzy.c's own header comment on
// ERROR_BAND_C_DEFAULT/RATE_BAND_C_PER_S_DEFAULT for why this is per-zone
// and not board-wide, and for why the rule TABLE itself (this header's
// comment above) stays a compile-time constant while these two numbers do
// not: the bands describe this zone's own measured plant envelope, the
// rules describe a control-law choice this project does not want re-opened
// per installation.
//
// error_c / error_rate_c_per_s: this zone's own signals only (setpoint -
// measurement, and that error's rate of change) -- see the header comment
// above for why a neighbor zone's state is never an input here.
// error_rate_c_per_s is normally fed from pid_state_t.d_filtered, which
// pid_update_terms() does NOT update while |error| > pid_range_c (it stays
// frozen at whatever it last held, 0.0f on a cold start) -- so the first
// in-range tick(s) after a long out-of-range climb may still see a stale
// rate here until the low-pass filter catches up. Harmless (gains don't
// matter while out of range), but worth knowing when reading rule firings
// near a functional-range transition.
//
// Output gains are always finite and non-negative, regardless of how
// extreme or non-finite (NaN/inf, e.g. from a faulted thermocouple feeding
// error_c) the inputs are -- a bad gain reaching pid_update() on a kiln is
// a hazard, not a style concern, so this function defends against it rather
// than trusting the caller to have already sanitized error_c/error_rate.
void pid_fuzzy_adjust(float error_c, float error_rate_c_per_s,
                      float error_band_c, float rate_band_c_per_s,
                      float base_kp, float base_ki, float base_kd,
                      uint8_t strength_pct,
                      float *out_kp, float *out_ki, float *out_kd);

// pid_fuzzy_derive_bands() -- docs/audits/fuzzy_dimensionless_bands_2026-09-13.md.
//
// ERROR_BAND_C_DEFAULT/RATE_BAND_C_PER_S_DEFAULT above are ABSOLUTE
// degC/degC-per-s constants the pid_fuzzy.c header comment already admits
// were sized by desk reasoning about "a mid-size kiln," never measured on
// any plant -- on THIS bench rig the rate default is ~6x the fastest ramp
// this kiln can actually produce (0.5 vs the measured ~0.083 degC/s), which
// is why 6 of the 9 rule cells were unreachable without an injected
// synthetic disturbance. A real kiln reaching ~1200 C where radiation
// dominates has a k_dc/tau_s an order of magnitude different again, so an
// absolute constant is wrong differently on every installation.
//
// This function instead derives both membership-band half-widths from
// THIS zone's own Autotune-identified FOPDT model (model_k_dc, degC at
// duty=1.0; model_tau_s, s -- zone_model_at()'s own two outputs, already
// used by the feedforward path):
//
//   rate_band_c_per_s = model_k_dc / model_tau_s
//     -- the FOPDT step response's own initial slope, dT/dt|t=0+ = k/tau
//     at a full (duty=1.0) step: the fastest rate THIS plant's own thermal
//     response can physically produce. Units check: degC / s. A genuine
//     disturbance (a stuck-open lid, a runaway element) can plausibly drive
//     the plant at close to this bound; an ordinary commanded ramp, run at
//     well under full duty, cannot -- so "large rate" keeps meaning "not a
//     normal firing," the same intent ERROR_BAND_C_DEFAULT/RATE_BAND_C_PER_S_
//     DEFAULT's own header comment states, just derived instead of guessed.
//
//   error_band_c = model_k_dc * ERROR_BAND_K_FRACTION
//     -- half of the plant's own full-duty steady-state temperature rise:
//     a "large" error is one on the order of what this zone's own actuator
//     can correct, not an arbitrary degC figure that means something
//     different on a 40 degC bench rig than on a 1200 degC kiln.
//
// Returns true and writes the derived bands when model_k_dc/model_tau_s are
// both finite and > 0.0f (zone_model_at()'s own "0 means never identified"
// convention, matching zone_load_model()'s validity check in profile_
// executor_feedforward.c). Returns false and writes the documented absolute
// ERROR_BAND_C_DEFAULT/RATE_BAND_C_PER_S_DEFAULT instead when the model is
// missing or invalid -- the explicit, only-for-a-never-autotuned-zone
// fallback this module still needs (there is no third option: a zone with
// no model has no plant-derived scale to work from). Callers MUST log a
// false return, not silently substitute the absolute default and move on --
// this is the whole reason a zone's fuzzy bands would still be the
// unmeasured desk-reasoning values, and that should be visible.
bool pid_fuzzy_derive_bands(float model_k_dc, float model_tau_s,
                             float *out_error_band_c, float *out_rate_band_c_per_s);

#ifdef __cplusplus
}
#endif

#endif // PID_FUZZY_H
