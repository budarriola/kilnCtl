# Temperature limit / headroom sweep — 2026-09-08

Owner concern: the bench's 80 C ceiling (Pico `abs_max_temp_c`) must not have
leaked into the code as a baked-in assumption anywhere else, and no other
bench-tuned margin/band/rate/duty headroom should silently misbehave at real
cone temperatures (up to ~1300 C).

## Method
Grepped KilnFW/SaftyFW/CommonFW for: temperature constants and defines,
validation bounds on temp fields, clamp/saturate logic, fixed-point/int16
scaling of temperature, UI input bounds, and named bands/margins/rates/
windows called out by the owner (progress_band_c, ramp-lock band, on/off
hysteresis, S8 rate guard, ff_hold, duty/integral floors, history ring
sizing, guard debounce windows).

## Findings by class

**(a) Legitimate physical limits — correct, unchanged**
- `MAX31856_TC_TEMP_C_PER_LSB` / `MAX31856_CJ_TEMP_C_PER_LSB` codec scaling
  (`max31856_codec.c`) — hardware LSB definitions, sign-extended 24-bit raw
  value, correct across the chip's full range.
- `TC_MAX_C_BY_TYPE[]` (SaftyFW `config_params.c`) — real thermocouple-type
  linearisation ranges, used to reject an `abs_max_temp_c` that contradicts
  the fitted TC type. Correct and must stay.

**(b) Configurable, bench value only — confirmed genuinely configurable**
- Pico `abs_max_temp_c` (0x0104, SaftyFW `config_params.c`/`config_store.c`)
  — persisted config field, no hardcoded shadow constant found duplicating
  80 C anywhere else in KilnFW/SaftyFW/CommonFW.
- `ZONE_MAX_TEMP_C_MAX` (`zones_config_accessors.h:171`) = 2500.0f — an
  input-sanity ceiling on the *validation* bound, not a bench limit; already
  documented in-repo as deliberately set above realistic cone temperatures
  (1300 C) with a `_Static_assert` in `profiles_http.c` tying
  `PROFILE_TARGET_C_MAX` to it. Correct.
- `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C` (25.0 C), `PROFILE_EXECUTOR_HYSTERESIS_C`
  (2.0 C), `progress_band_c`/`drift_hysteresis_c` (thermal_guard) — all are
  struct fields with `effective_f(cfg->x, DEFAULT)` fallback, i.e. already
  per-zone configurable; the `#define` is only the shipped default, not a
  hardcoded ceiling. `EXEC_RAMP_LOCK_BAND_C(zi)` / `EXEC_BANGBANG_HYSTERESIS_C(zi)`
  resolve through `exec_threshold()`, confirming per-zone override exists.
- `CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN` (33.3 C/min, SaftyFW S8) — named
  and commented as a *default*, stored in the same configurable record as
  `abs_max_temp_c`. Reasoning on absolute-vs-scaled: a rate guard in C/min is
  the right unit regardless of setpoint (it bounds furnace runaway rate, not
  proximity to setpoint), so this does not need to scale with temperature —
  but it DOES need to be re-tuned for a kiln with much larger thermal mass:
  33.3 C/min was fit to the bench unit's fast small chamber. Classify as (b),
  flagged for owner re-tuning before a real firing, not a code defect.
- `history_pack_temp()` / `AUTOTUNE_TRACE_TEMP_INVALID` int16 0.1-C-LSB
  encoding — checked for overflow at 1300 C: 1300*10=13000, well inside
  int16 range (±32767), and both encoders explicitly clamp to
  ±32767 before casting. No hazard.

**(c) Bench assumption that would misbehave at cone temperature — none found**
No hardcoded numeric ceiling, buffer size, or validation bound was found
still pinned to 80/62/60 C outside the Pico's own configurable
`abs_max_temp_c` record. The repo's own comments (`autotune_engine_internal.h`
around `max_temp_c=1300`) show this class was already hunted and fixed in an
earlier pass — this sweep found no new instance.

**(d) Numeric-range hazard at 1300 C — none found**
- Elapsed-time/duration fields relevant to a real (many-hour) firing
  (`total_elapsed_s`, `segment_elapsed_s`, `duration_s`, `fs_max_overshoot_elapsed_s`)
  are all `uint32_t` seconds — good for ~136 years, not a bench-length
  assumption.
- `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` and the history ring pack/unpack use
  fixed-width int16 with explicit saturation, not truncation.
- UI number inputs (`zones_page.html`, `setup_wizard_page.html`,
  `profiles_page.html`) have no `max` attribute on `max_temp_c`/target
  fields (unbounded client-side), so no stale bench cap there either; the
  one bounded input found (`atSetpoint` autotune target, `max="1400"`) is
  already set above cone range, not below it.

## Headroom items from the coordinator's follow-up

- **ff_hold infeasible above ~62 C** (already known, `project_ff_hold_infeasible_above_62c.md`)
  — this IS the real instance of tuning-headroom running out under real
  conditions. No sibling of the same shape (a coupled-solve matrix valid
  only over a narrow low-temperature range) was found elsewhere; the
  coupling matrix is the only cross-zone solve in the tree.
- **S5 debounce (10-bad-reads-in-5-s) and the 60 s PWM window**: both are
  fixed durations, not temperature-scaled, and there's no reason they should
  scale with setpoint — they bound sensor-noise and actuation-cadence
  behaviour, which doesn't change with kiln temperature. Correctly absolute.
- **Guard 1 progress window / arrival band**: already configurable per-zone
  (see (b) above); absolute-C is the right unit for "close enough to target"
  regardless of setpoint, since MAX31856 noise floor is also absolute, not
  proportional — tightening it automatically at high setpoint would produce
  false progress-guard trips from ordinary sensor noise. Recommend leaving
  absolute, re-tuning per-zone only if bench-derived defaults prove too
  loose/tight on the real kiln's dynamics.
- **Duty/integral floor** (`pid.h`'s `Ki*integral >= -ff_hold`): this is a
  feedforward-shaped floor, not a bench constant — it already generalizes
  correctly (`ff_hold=ff_u` reproduces prior behaviour). No fixed numeric
  headroom found hardcoded here.

## Fixes applied
None — no genuine defect (class c) or numeric hazard (class d) was found in
this pass; today's constants are either physical (a) or already
configurable with documented defaults (b). No negative test was added,
since no production logic changed. (Prior passes evidently already did the
work this sweep was checking for — see `autotune_engine_internal.h`'s
`max_temp_c=1300` commentary and `ZONE_MAX_TEMP_C_MAX`'s `_Static_assert`.)

## Owner actions needed
1. `abs_max_temp_c` must be raised on the **Pico first, then the ESP**
   (standing rule: the Pico ceiling is a second set of eyes and must never
   be tighter than the ESP's) before any real firing above 80 C.
2. Re-tune `CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN` (33.3 C/min) for the
   real kiln's thermal mass — bench-fit value, likely too tight for a larger
   chamber and will nuisance-trip S8 mid-firing if left as-is.
3. Re-validate `ff_hold`'s coupled-solve feasibility region above 62 C once
   real-kiln coupling data exists; it is currently fit only to the bench
   unit's zones.
