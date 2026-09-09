# S8 rate-guard re-tune — read-only analysis and recommendation

**Date:** 2026-09-09. Analysis only: no flash, no board writes, no firing/autotune,
no safety-processor config change. All numbers below come from recorded log
data (`logs/coupling/*.jsonl`, gitignored/local, 65 files) and firmware source
already in the tree. Board state was read but not written.

## 1. Where 33.3 C/min came from

Not arbitrary — traced to two distinct events that must not be conflated:

1. **`c43323a2`** (2026-09-05), "S8 sanity-rate guard: code default is now 2x
   the fastest shipped ramp" — this changed the **compiled record default**
   (`CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN`) from `0.0f` (ships disabled)
   to a derived value. Per `firmware/SaftyFW/docs/CONFIG_REFERENCE.md`'s
   later correction, the derivation is: S8 only trips on a **positive**
   (climbing) delta, so KilnFW's built-in profile catalog's fastest *rising*
   segment governs, not its fastest segment overall (which is a crash-cool).
   The fastest rising segments are two tied 999.0 C/hr steps in profile
   `FSCGB1` ("Shimbo Crystal Holding Pattern 2"): 999.0 / 60 × 2 = **33.3
   C/min**. (An earlier, since-corrected version of this doc used the
   crash-cool figure and got 333.3 C/min — that number never shipped as the
   compiled default; 33.3 is the corrected, current one, per
   `CONFIG_REFERENCE.md`'s own text.)
2. **`889f80d9`** (2026-09-06), "Record S8 rate-guard commissioning and
   measured CT noise floor" — this is the event that actually put 33.3 C/min
   on the bench board's live config: "applied to the bench Pico at the
   owner-decided 33.3 C/min threshold via `safety_set_rate_guard`". The
   owner's choice happened to equal the compiled default rather than
   overriding it with a bench measurement.

**Verdict: derived, not a guess — but derived from the wrong quantity.** It
is 2x the fastest *authored profile* ramp, a "no shipped profile can ever
trip this" ceiling. It was never derived from a measured plant capability
(a real full-duty ramp on this bench, or any kiln). `docs/audits/
temperature_limit_sweep_2026-09-08.md` (`§`"CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN")
already reached the same conclusion and flagged it for re-tune, as does
`ROADMAP.md` line 129. This document supplies the measurement that was
missing.

## 2. Firmware's actual S8 implementation

`firmware/SaftyFW/src/safety_guards.c` (current HEAD `4907817`, function
around line 702, "S8: implausible rate of rise"):

- **Baseline-sample-and-hold, not a sliding window.** A window opens at the
  current reading, accumulates `dt_s` until elapsed ≥ `rate_window_s`
  (default 60 s), then computes `rate_c_per_min = (tc_c_now - tc_c_at_window_start) / (elapsed_s/60)`
  and starts a fresh window from the new endpoint. So the "60 s window" is
  really evaluated once every ~60 s, using only the window's two endpoint
  samples — not an average of many samples, not a continuous derivative.
- **Debounced 2x before tripping** (`S8_OVER_RATE_STREAK_TO_TRIP = 2`): the
  rate must exceed `max_rate_c_per_min` in **two consecutive** ~60 s
  evaluations (~120 s of sustained over-rate) before S8 trips. A single
  fast window does not trip it.
- **Global, single value — not per-zone.** The Pico only reads its own one
  safety thermocouple (`safety_tc_c`, `tc_source` selects which physical TC
  feeds it); there is exactly one `max_rate_c_per_min`/`rate_window_s` pair
  in `config_store`, and S8 evaluates it once per tick against that single
  reading. It has no concept of "zone."
- **Ships fields_set-gated OFF**: `safety_core_load_guard_cfg()` forces the
  armed value to 0.0f unless `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN` was
  explicitly written through commissioning — confirmed still true on HEAD.

This matters for the comparison in §3 below: the firmware's own rate
estimate is a **coarse, twice-per-2-minutes endpoint delta**, not a
per-sample derivative. A same-day, same-log finding (`sim_wide_temperature_
2026-09-08.md`, "guard behaviour" section) independently used this same
windowed method for its own S8 discussion, so the methodology in this
document is consistent with prior work.

## 3. Measured rate distribution from `logs/coupling/*.jsonl` (65 captures)

Each capture logs `exec.zones[].actual_c` at ~5.05-5.3 s cadence (median
5.235 s; occasional multi-hundred-second gaps between segments, filtered out
of the windowed calculation by construction). Two estimators were computed
per capture per zone:

- **Raw consecutive-sample dT/dt** (successive log ticks, ~5.2 s apart) — the
  finest resolution available, but exactly the shape CLAUDE.md warns about: a
  short interval on quantized MAX31856 (0.25-0.5 C LSB-ish) data can produce
  spurious spikes. This is reported for completeness, not as the operative
  number.
- **Firmware-style 60 s-window rate** — baseline-sample-and-hold reproducing
  `safety_guards.c`'s own algorithm exactly (see §2): pick a baseline
  sample, accumulate elapsed time, evaluate once elapsed ≥ 60 s, take the
  two-endpoint delta, reset. This is the number the guard actually acts on.

| Estimator | n | max | p99.9 | p99 | p95 | p50 |
|---|---|---|---|---|---|---|
| Raw per-sample (~5.2 s) | 56,460 | 9.92 C/min | 7.86 | 5.16 | 3.20 | 0.00 |
| Firmware 60 s-window | 6,546 | **7.69 C/min** | 7.14 | 3.86 | 2.93 | 0.00 |

The two agree within ~25% at the extreme (9.92 vs 7.69), i.e. the raw
5.2 s-interval estimator is **not** wildly spuriously inflated here relative
to the firmware's own coarser method — both point to the same physical
event. All top values in both tables come from the same handful of captures:
`easeoff_ab_20260904_2p0/3p0_run*.jsonl` and `fuzzy_ab_20260904_s50_run1.jsonl`,
all zone 0/1, all in the first ~60-65 s of a run — the initial full-duty
transient climbing away from ambient (~32-35 C baseline to ~40-42 C), exactly
where `dT/dt ~ P/C` is largest before losses build up (matches the
`sim_wide_temperature_2026-09-08.md` §4/§3a finding independently).

**Cross-check against the simulator (`docs/audits/sim_wide_temperature_
2026-09-08.md` §4):** at `power_scale=1` (the faithful, non-extrapolated
bench model), simulated `max_rate` was **13.1-13.5 C/min** — same order as
this log-derived 7.7-9.9 C/min, higher because the simulator's illustrative
runs cover more of the cold-start full-duty transient than any single
65-capture recording happened to catch. Both are far below the current 33.3
C/min threshold. That same audit also found that a **hypothetical** 5x-more-
powerful element set (`power_scale=5`, explicitly labeled an unvalidated
extrapolation, not a measurement) would produce ~58 C/min and nuisance-trip
S8 — this is not evidence about the current bench, only a caution about
scaling element power without re-deriving S8.

**No firmware/measurement discrepancy found.** The firmware's coarser
60 s-endpoint method reports slightly *lower* peak rates than the raw
per-sample method here (7.69 vs 9.92), not higher — so there is no hidden
case where the firmware would trip on a rate this analysis missed. This is
the opposite of a case where "the discrepancy is itself the finding": here
the two estimators agree closely enough that either is a safe basis for a
recommendation.

## 4. Plant context — this is a bench number, not a kiln number

Per prior identification work (`project_coupling_matrix_resolved`, cited in
memory), the bench rig's diagonal gain is `K_diag ~= 38 C/duty`, and the
documented consequence is that the bench **cannot reach 40 C above ambient
even at full duty**. Every capture analyzed above was recorded on this same
weak rig. The observed 7.7-13.5 C/min ceiling is a direct consequence of that
gain — not a property of "a kiln" in general.

Real kilns are tuned hot, and the roadmap/audit record already states that
both k (gain) and tau (time constant) fall by roughly the same factor
(~20x) at high temperature. A real kiln's fastest achievable rate is thus
governed by very different numbers than this bench's — a real kiln typically
has much larger heating elements (larger raw power) but also vastly larger
thermal mass, and the two effects partially cancel; nothing in this repo's
data lets that net rate be estimated for a real kiln today. **This
recommendation is a bench-only value.** It must be re-derived by repeating
this same procedure (log real full-duty ramps from cold on the attached kiln,
compute the firmware's own 60 s-window rate, apply a margin) once a real
kiln is attached — carrying 33.3, or any bench-fit number, forward
unexamined onto a real kiln is exactly the mistake `temperature_limit_
sweep_2026-09-08.md` already flagged.

## 5. Recommendation

**Bench (today): no change is strictly required, but 33.3 C/min carries far
more margin than its own justification claims, and that margin is
coincidental, not engineered.** It happens to sit ~3.4x-4.3x above the
measured 60 s-window peak (7.69 C/min) purely because a weak bench rig
produces small numbers, not because anyone checked. If the owner wants a
bench value that is actually defensible on the same "2x measured peak" logic
already used elsewhere in this codebase (e.g. `c43323a2`'s own reasoning,
just applied to the right quantity):

- **Recommended bench value: 18-20 C/min** (>2x the measured 60 s-window
  peak of 7.69 C/min, comfortably above the simulator's 13.1-13.5 C/min
  faithful-bench figure too, so it does not ride the edge of either
  measurement).
- This is **tighter** than the current 33.3 C/min. The "never tighter than
  the ESP's equivalent limit" constraint does not bind here: KilnFW has no
  independent max-rate-ceiling guard to compare against. `thermal_guard.c`'s
  `sanity_rate_c_per_min` (guards 1/2) is a **minimum**-rise floor (catches
  heat not arriving), not a maximum-rise ceiling, and `RUNAWAY_RATE_C_PER_MIN`
  (guard 3, 1.0 C/min) only applies with heat commanded **off** (welded-relay
  check) — neither is S8's counterpart. The `0x0204 max_rate_c_per_min`
  field itself is the *same* shared safety-link parameter KilnFW's
  `safety_cfg_store.c` pushes to the Pico during commissioning (ships as
  `0.0f`/disabled on the ESP side too) — there is one number, not two, so
  tightening it on the Pico does not create an ESP/Pico asymmetry.
- **What would have tripped at 18-20 C/min:** zero of the 65 recorded
  captures. Even at a much more aggressive 8 C/min (just above the single
  observed 7.69 C/min max), **zero captures still trip**, because S8's
  2-consecutive-windows debounce (~120 s sustained) is never satisfied by
  these transients — the fast windows are single, isolated spikes at the
  start of a run, not a sustained climb. Table (windows-over-threshold vs.
  captures-that-actually-trip, honoring the debounce):

  | Threshold (C/min) | Windows over threshold | Captures that would TRIP |
  |---|---|---|
  | 7 | 10 | 0 |
  | 8-33.3 | 0 | 0 |

  (all 65 captures pass at every threshold tested from 7 to 33.3 C/min once
  the real debounce logic is honored — recommending 18-20 gives margin
  without spending any of that headroom against real recorded operation).

**Real kiln (future): do not carry forward any bench number.** Before a real
kiln's first firing, repeat this analysis against that kiln's own logged
full-duty ramp data and re-derive a threshold the same way (2x the measured
60 s-window peak). Given the k/tau ~20x-at-temperature relationship already
on record, do not assume the real-kiln number will be smaller just because
tau grows — a real kiln's raw element power is also far larger than this
bench's, and nothing here bounds the net effect.

## 6. Interaction with `abs_max_temp_c` (currently 80 C, flagged separately for raising)

Independent parameters — S8 bounds *rate*, S1 bounds *absolute level* — so
raising `abs_max_temp_c` does not itself require changing `max_rate_c_per_min`
for S8 to keep working as designed. However, raising the ceiling is exactly
the kind of change that should not happen without re-examining every other
bench-fit safety number at the same time: a real firing running to a much
higher ceiling will also be running with a real kiln attached (see §4), and
that is precisely the point at which 33.3 (or any bench value) stops being
defensible. Practically: **re-tune S8 as part of the same real-kiln
commissioning pass that raises `abs_max_temp_c`**, not as a separate,
possibly-forgotten follow-up.

## 7. Data and commits cited

- `firmware/SaftyFW/src/safety_guards.c` @ HEAD `4907817` (S8 implementation)
- `c43323a2` — compiled default introduced (33.3 C/min, corrected derivation
  per `CONFIG_REFERENCE.md`)
- `889f80d9` — 33.3 C/min applied to the bench board via commissioning
- `2b3f1206` — most recent commit on `main` as of this analysis (unrelated;
  cited only to confirm HEAD at analysis time)
- `docs/audits/temperature_limit_sweep_2026-09-08.md` — prior flag that 33.3
  needs re-tuning for a real kiln
- `docs/audits/sim_wide_temperature_2026-09-08.md` §4 — simulator cross-check
  (13.1-13.5 C/min at faithful `power_scale=1`; 58 C/min at unvalidated
  `power_scale=5`)
- `logs/coupling/*.jsonl` (65 files, gitignored per `.gitignore:100`) — this
  document's primary measured data; not committed (by design), so results
  are reproducible only from a tree that still has these local capture files

**No code, config, or board state was changed by this analysis. This is a
recommendation for the owner to apply via `safety_set_rate_guard`, not a
change already made.**
