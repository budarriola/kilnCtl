# Design: confidence-graduated authority for `adaptive_tune` (2026-09-13)

Design only. No code in this repo was changed by this document.

Reads on: `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md` (`655da406`),
`firmware/KilnFW/App/drivers/control/adaptive_tune.c`/`.h`, `adaptive_tune_model.c`,
`adaptive_tune_ki.c`, `adaptive_tune_internal.h`; commits `97288659` (K_dc ratchet fix,
anchor to `autotune_baseline_k_dc`) and `36f88d62` (adversarial follow-up: a whole-page
`POST /api/zones` silently zeroed that new anchor field); `docs/FUZZY_CONTROLLER_PLAN.md`
§8 (effort/payoff table for the fuzzy layer's own, structurally similar, confidence
proposal).

## 0. Where things stand today (facts this design is built on)

- `655da406` §3(c) and §5: every `adaptive_tune` guard is a fixed constant —
  `ADAPTIVE_TUNE_MIN_OBSERVATIONS=4`, `ADAPTIVE_TUNE_BLEND_ALPHA=0.15`,
  `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE=0.20` (`adaptive_tune_internal.h:114,120,121`) — applied
  identically to the 1st and the 100th accepted refinement. No confidence variable exists.
- `97288659` fixed the one ratchet that *did* exist: the ±5x plausibility check used to read
  the live, self-moving `model_k_dc` as its own reference. It now reads
  `zone_cfg_t::autotune_baseline_k_dc`, the K_dc the last full autotune Accept wrote, which
  `adaptive_tune` itself never moves (`adaptive_tune_model.c`, see the commit's diff). The
  blend target and its own ±20%/run cap are deliberately left reading the live `k_dc` — that
  is what lets repeated accepted runs converge toward a better-fit value at all.
- `36f88d62` found that the new anchor field was not carried through
  `zones_http_post_parse.c`'s field-by-field zone rebuild, so an ordinary whole-page
  `POST /api/zones` zeroed it — 0 is the field's own "no baseline recorded yet" sentinel, so
  the very next accepted refinement re-bootstrapped the anchor from the *already-adapted*
  live value, silently recreating the ratchet `97288659` exists to remove. Fixed by adding the
  missing carry-through line (`zones_http_post_parse.c:478`,
  `z->autotune_baseline_k_dc = current_z->autotune_baseline_k_dc;`), following the same pattern
  already used for `tuning_*`, `model_fit_temp_c`/`model_fit_ambient_c`, and
  `adaptive_tune_enabled` (each with its own reset-one-side comment,
  `zones_http_post_parse.c:415-478`).
- Nothing above adds a confidence variable. Both commits are about anchoring an existing fixed
  bound correctly, which is a prerequisite for this design, not a substitute for it.
- `docs/FUZZY_CONTROLLER_PLAN.md` §8 already ran the effort/payoff argument for a *sibling*
  confidence mechanism (option (i), per-cell fuzzy confidence) and rejected building it now:
  "8 of 9 confidence values cannot be evidenced on this fixture." §7 below asks the identical
  question for `adaptive_tune`'s confidence and reaches a compatible answer for a compatible
  reason — this is not a coincidence, it is the same bench limitation applying to any
  measured-confidence design in this codebase.

## 1. What confidence IS

Reject the two non-starters explicitly, since the brief calls them out and it would be easy to
reach for either by default:

- **Elapsed time** — measures nothing about the plant or the fit; a board sitting idle for a
  month has learned nothing.
- **Raw firing count** — `ADAPTIVE_TUNE_MIN_OBSERVATIONS` already gates on *count* today and
  §3(c) of `655da406` is precisely the finding that a count-only gate is not graduated
  authority, only an on/off switch. Reusing count alone as "confidence" would just rename the
  existing defect, not fix it.

**Proposed confidence signal: agreement between the model's OWN prediction and the next
run's OWN outcome — a forward-checked residual, not a backward-fit residual.**

Concretely, per zone, maintain a rolling record of, for each accepted refinement at run N:
- the refined model (`K_dc`, τ, dead-time) as it stood *after* run N's blend, and
- the settled-dwell observations `adaptive_tune_zone_tick()` records during run N+1 (the same
  (duty, rise-over-ambient) pairs already harvested for the ring, `adaptive_tune.c:319-355`).

At run N+1's own run-end, before folding N+1's observations into the fit, first score how well
run N's model *predicted* N+1's actual settled points:

```
predicted_rise = K_dc_after_run_N * duty_observed_in_run_N+1
residual_frac  = |predicted_rise - actual_rise_observed_in_run_N+1| / actual_rise_observed_in_run_N+1
```

averaged over the qualifying settled-dwell observations in run N+1 (same settle/duty-stability
gates the ring already applies, `adaptive_tune.c:319-355`). A converging sequence of small,
shrinking `residual_frac` values across successive runs is evidence of a stable, well-identified
plant; a residual that stays large or grows is evidence the model (or the plant) is moving out
from under the fit.

Combine two independent pieces of evidence, both forward-checked:
1. **Predictive residual** (above) — did the model predict the NEXT firing, not just fit the
   PAST ones.
2. **Fit stability** — the sequence of successive accepted `K_fit` values (the raw per-run OLS
   fit before blending, already computed at `adaptive_tune.c:151-166`) narrowing rather than
   wandering, e.g. the coefficient of variation of the last `K` accepted `K_fit` values falling
   below a threshold. A converging sequence is evidence; a wandering one is not (per the brief).

Confidence is a monotone function (e.g. a weighted count, not a raw average, so a single lucky
run cannot spike it — see §5) of "how many CONSECUTIVE runs have both signals stayed inside their
good-agreement band," reset to its floor the instant either signal fails (see §3).

Observation count and operating-condition spread (the third candidate in the brief) should be
an **eligibility gate on the two signals above**, not a confidence input on its own: a model
cannot be said to "agree with reality across conditions" if every accepted run has come from the
same narrow duty band. Concretely, require the ring's existing `ADAPTIVE_TUNE_MIN_DUTY_SPREAD`
gate (`adaptive_tune_internal.h:116`) to have been satisfied over the runs being scored before
counting them toward confidence at all — reusing a gate the module already computes rather than
inventing a second one.

### Is this circular?

The brief's circularity precedent (cited from an earlier review, not found verbatim in the files
read for this pass) is: a controller must not be scored as "good" by its OWN tracking error,
because a controller that never leaves one regime looks good for the wrong reason — the fitted
plant re-identification driving adaptation must be independent of the controller's own
performance metric, otherwise the loop validates itself.

**That specific objection does not apply here, and the reason has to be stated precisely rather
than asserted:** the confidence estimator proposed above does not touch tracking error (how
close the zone got to setpoint) anywhere. It compares two independent PLANT observations —
run N's fitted `K_dc` versus run N+1's freshly measured (duty, rise) pairs — the same class of
fresh re-identification the earlier review already approved for the base mechanism itself
(`adaptive_tune_zone_tick()`'s settled-dwell harvesting is *why* `655da406` found the module
does satisfy (a)+(b)). Confidence-scoring "did run N's model predict run N+1's fresh
measurement" is structurally the same operation as the base learning loop, run one step further
out — it asks "would the last fit have generalized," not "did the controller feel good about
itself."

**Where a narrower, real circularity risk DOES exist, and must be designed out:** if a zone's
firing pattern never varies (same profile, same duty band, every time — plausible on a
single-purpose kiln), the predictive-residual signal could stay flatteringly small purely
because the model is never asked to extrapolate, not because it is actually a good model outside
that band. This is why the duty-spread eligibility gate above is load-bearing, not decorative:
without it, a zone that always dwells at the same duty could ratchet to full authority having
never been tested anywhere else. **Position taken: the plant-re-identification objection does
not apply to this confidence design as specified, provided the duty-spread gate is implemented
as a genuine precondition (runs outside the historically-seen duty band do not count toward
raising confidence, and — see §3 — a run whose duty falls outside the previously-confident band
should be scored for agreement, not skipped, so a real regime change is visible instead of
invisible).**

## 2. Which guard(s) confidence should modulate

**Proposed: confidence modulates `ADAPTIVE_TUNE_BLEND_ALPHA` and `ADAPTIVE_TUNE_MIN_OBSERVATIONS`
only. The ±5x envelope against `autotune_baseline_k_dc` (`ADAPTIVE_TUNE_MAX_JUMP_RATIO`) and the
absolute ceiling `ADAPTIVE_TUNE_K_DC_ABS_MAX` stay fixed, permanently, regardless of confidence.**

Argument for leaving the envelope fixed:

- `655da406` §5 and `97288659`'s own commit message name this exact envelope as "this codebase's
  own existing answer to the hottest temperature this system is designed to ever legitimately
  report" — it is anchored to a physical fact (kiln cone ratings), not to anything
  `adaptive_tune` measures about itself. A confidence-driven *widening* of that envelope would
  make the hard safety bound a function of the very mechanism it exists to bound — which is
  exactly the shape `655da406` §5's closing paragraph warns against ("a confidence-driven
  authority increase on top of a self-moving reference point would only compound that hazard").
  The precedent already in this repo (`project_bound_relative_to_persisted_state`,
  `project_reset_one_side_bug_class`) is specifically about safety bounds whose reference frame
  is allowed to move; making the reference frame move *faster* under high confidence is a
  strictly worse version of the same defect, not a mitigation of it.
- The envelope is also the module's LAST line of defense if the confidence estimator itself is
  ever wrong (a bug in the residual computation, a corrupted ring, a `POST` field wiped the way
  `36f88d62` found for the anchor). A wrong confidence score should degrade to "learns
  conservatively," never to "the safety bound moves too."

Argument for the two guards proposed as confidence-modulated:

- **`ADAPTIVE_TUNE_BLEND_ALPHA`** (currently a fixed 0.15 convex-combination weight,
  `adaptive_tune_model.c` blend step): a HIGHER confidence in the fit justifies weighting the
  fresh `K_fit` more heavily in the blend — this is precisely "authority grows with measured
  confidence" and is bounded by construction (`alpha` is a weight in a convex combination; it
  cannot itself push `K_dc` outside `[K_fit, K_dc]`, which is already inside the fixed envelope
  above). Raising `alpha` when confidence is high lets the model converge faster toward a
  well-evidenced fit; lowering it toward the floor when confidence is low is the natural
  cautious default.
- **`ADAPTIVE_TUNE_MIN_OBSERVATIONS`**: rather than "confidence lowers the bar to accept a fit,"
  frame it the other direction, which is safer and matches "authority grows": a zone with a
  strong track record earns the right to act on a *smaller* fresh ring (faster reaction to a
  real change) than a zone with no track record, which should be held to the current, more
  conservative minimum, not a lowered one. So the base value (4) is the FLOOR for a
  zero-confidence zone; the ceiling this guard can relax to is a matter for calibration, not this
  design (§5's ceiling policy applies).

This is deliberately the weaker of the two claims the brief distinguishes: modulating a blend
weight and an evidence-count threshold that already produce inputs the fixed envelope re-checks
every run, versus modulating the envelope itself. Both proposed knobs can only ever move the
"how eagerly do we act" side of the mechanism; the "how far are we allowed to go" side never
moves. `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE` (the ±20%/run cap) is left as a THIRD candidate,
explicitly not proposed here: it interacts with `alpha` (both bound the same blend step from
different sides — `alpha` sets the target, the per-run cap clamps the result), and modulating
both at once from the same confidence signal risks a compounding effect that is harder to reason
about than modulating one. If experience with `alpha`-only modulation later shows it converges
too slowly even at maximum confidence, revisit the per-run cap as a second knob then, with its
own bound (never above `ADAPTIVE_TUNE_MAX_JUMP_RATIO` decomposed per-run, i.e. still inside the
fixed envelope over any finite number of runs) — not as part of this initial design.

## 3. Confidence must be able to fall

**What raises it:** a materially small residual (§1) on the run immediately following an
accepted refinement, sustained over consecutive qualifying runs, with the duty-spread
eligibility gate satisfied.

**What drops it, and by how much:**

| Event | Effect on confidence |
|---|---|
| A qualifying run's predictive residual exceeds its good-agreement band | Reset to floor immediately (single-run drop, not gradual — an outcome that contradicted the model is evidence the model is wrong NOW, not evidence to average away) |
| Any guard trip on that zone during the firing being scored | Reset to floor. A trip during a firing whose data would otherwise feed confidence means the firing's own thermal history is suspect for anything past the trip |
| `adaptive_tune_revert()` invoked on that zone | Reset to floor — reverting is the operator saying "this refinement was wrong," which is stronger evidence against confidence than a single missed prediction |
| A NEW full autotune Accept on that zone (`autotune_baseline_k_dc` re-anchored, per `97288659`'s `autotune_engine_guard.c` change) | Reset to floor. A fresh autotune is definitionally a fresh plant re-identification superseding everything `adaptive_tune` had accumulated confidence about; carrying old confidence forward past a new baseline would misrepresent how well-evidenced the NEW baseline is (zero runs against it so far) |
| A run refused as faulted/stopped-early or over the excluded-sample fraction (`ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION`, `adaptive_tune.c:559-575`) | No change either direction — already excluded from training data by the existing gate, so it should be excluded from confidence scoring too, neither raising nor falling it |
| A physically changed kiln (element replacement, rebuild, different load) | **Cannot be detected by firmware.** No sensor in this system observes "the physical kiln changed" independent of the thermal response itself — a genuine hardware change and a large model disagreement look identical from inside `adaptive_tune`. The falling-residual path above is the ONLY mechanism that can catch this, and it does so correctly: a changed kiln immediately produces bad forward predictions, which is exactly the "residual exceeds band" case above. There is no separate detector to design; state this limitation rather than pretending a dedicated one is missing |

**Persistence — where confidence lives and what resets it, in the same register as the
`36f88d62` hazard:**

Confidence must persist across reboots for the SAME reason `autotune_baseline_k_dc` had to move
into the persisted zone config blob rather than staying RAM-only in `655da406`'s Gap 1 sketch:
a per-boot-only value would make "authority" reset to the floor on every ordinary power cycle,
which is not a graduated authority scheme at all, it is a disguised on/off switch keyed to
uptime — precisely the "elapsed time" non-starter rejected in §1, arrived at by the back door.

So confidence state (proposed fields: a small integer "consecutive good runs" counter per zone,
plus the duty-spread eligibility bookkeeping) belongs in the SAME persisted `zone_cfg_t` blob as
`autotune_baseline_k_dc`, not in `adaptive_tune`'s own private NVS namespace where the
observation rings and Ki baseline currently live (`ADAPTIVE_TUNE_NVS_NAMESPACE`,
`adaptive_tune_internal.h:145`) — because it needs the EXACT SAME whole-page-save carry-through
discipline `36f88d62` had to retrofit onto `autotune_baseline_k_dc`, and putting it in the same
struct as that field means one already-fixed code path (`zones_http_post_parse.c`'s per-field
carry-through, now four fields deep: `tuning_*`, `model_fit_*`, `adaptive_tune_enabled`,
`autotune_baseline_k_dc`) is the one place a new confidence field also needs a carry-through
line — not a new struct with its own new parse path to get right from scratch.

**Explicit check for the same class of hazard, per the brief's instruction:** any NEW
zone-config field this design adds (the confidence counter, any per-zone eligibility state)
MUST get its own explicit `z->new_field = current_z->new_field;` line in
`zones_http_post_parse.c`'s per-zone rebuild, following the pattern at line 478, and a test
extending the existing "whole-page save preserves the key-less fields" coverage
(`test_zones_http.c`, extended by `36f88d62`) for that new field specifically — a general
statement that the pattern exists is not sufficient, `36f88d62` is proof that a field can be
added to the struct correctly and STILL be missed in this one call site, because the struct
definition and the parse function are edited in two different places under two different review
passes. This design explicitly recommends implementing the new field(s) and their
carry-through line in the SAME commit, with the adversarial "did I miss a carry-through" review
`36f88d62` performed applied to this feature the first time, not as a follow-up fix after a
second live-config defect is found the same way.

## 4. Cold start

A fresh install (or a zone just after a new autotune Accept, per §3's reset-on-reautotune rule)
starts with confidence at its floor. At the floor:
- `ADAPTIVE_TUNE_BLEND_ALPHA` is at its current fixed value, 0.15 — i.e. **the floor setting IS
  today's existing, already-shipped, host-tested behavior**, not a new, untested, more
  conservative mode. This is a useful minimum, not a useless one: it is the exact configuration
  `655da406` already found satisfies (a)+(b) and is safe by the existing bounds analysis in its
  §5. Nothing about adding confidence-graduated authority ABOVE this floor can make the floor
  itself worse than what ships today.
- `ADAPTIVE_TUNE_MIN_OBSERVATIONS` is at its current fixed value, 4 (the floor, per §2).
- The ±5x envelope and absolute ceiling are unaffected by confidence at any level (§2), so cold
  start and full confidence are identical on that axis by design.

This makes cold start safe by construction: the entire confidence mechanism is additive on top
of a floor state that is bit-for-bit today's shipped, already-reviewed behavior. A bug in the
NEW confidence-scoring code (miscomputed residual, corrupted counter) can only ever fail to
RAISE authority above where it already safely sits today; it cannot on its own lower a guard
below today's shipped values, because the guards start pinned at today's values and confidence
is defined as strictly non-decreasing-then-clamped from there (§5).

## 5. Bounds and monotonicity

- **Rise:** rate-limited by construction — confidence rises by at most one "consecutive good
  run" increment per accepted run (§1/§3), never by a batch jump, so even a burst of favorable
  residuals cannot vault a zone from floor to ceiling in one run.
- **Fall:** immediate and unrated-limited (§3) — any one disqualifying event drops confidence to
  the floor in the SAME run-end call that detects it, never gradually. This asymmetry is
  deliberate: the cost of being slow to regain authority after a bad sign is a few extra
  conservative runs; the cost of being slow to LOSE authority after a bad sign is running with
  more authority than the evidence currently supports.
- **Ceiling:** propose a ceiling on `alpha` well short of 1.0 (e.g. 0.5 — a number to calibrate,
  not one this design fixes) so that even a maximally-confident zone still blends rather than
  fully replacing `K_dc` with a single run's raw fit in one step; `ADAPTIVE_TUNE_MIN_OBSERVATIONS`
  similarly floors at some value above 1 (e.g. 2) so a single lucky observation can never alone
  trigger a refinement regardless of confidence.
- **Should full authority ever be reached automatically, or require an operator action?**
  **Automatically, but only up to the ceiling proposed above — never to "no cap at all."**
  Requiring an operator action to reach the ceiling would reintroduce a manual step this whole
  mechanism exists to avoid (the owner's requirement (c) is explicitly about authority growing
  as confidence rises, not about an operator manually promoting a zone), and the ceiling itself
  is already a hard limit independent of any operator action — "full authority" under this
  design is still bounded by `alpha_max < 1.0` and `MIN_OBSERVATIONS_floor >= 2`, both fixed
  constants chosen at design time, so there is no unbounded state an operator action would be
  needed to gate.

## 6. Suppression

Confidence accumulation (not just the write itself, which `adaptive_tune_run_end()` already
gates to run-end-only) must be suppressed — meaning the run in question is excluded from
scoring in either direction — during:

- **Faults / aborted firings:** already excluded from training data by
  `ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION` (`adaptive_tune.c:559-575`); extend the same exclusion to
  confidence scoring (§3's table, "no change either direction").
- **Guard trips:** treated as a confidence-lowering event per §3 (not merely excluded — a trip
  during a scored firing is itself adverse evidence, not neutral).
- **Autotune in progress or just completed:** a full autotune Accept resets confidence to the
  floor (§3); while an autotune is actively running on a zone, `adaptive_tune` should not be
  scoring or writing for that zone at all (no different from today's non-interlocked-but-
  disjoint-timing argument in `655da406` §6 — autotune and adaptive_tune already write through
  the same setters at disjoint times, and confidence bookkeeping should follow the same
  disjoint-timing discipline rather than adding a new lock).
- **Recovery mode:** `boot_guard_is_recovery_mode()` — CLAUDE.md's Firmware gotchas section is
  explicit that RECOVERY MODE already skips starting `profile_executor`/`autotune_engine`
  outright; since `adaptive_tune_zone_tick()`/`_run_end()` are only ever called FROM
  `profile_executor.c`, a board in recovery mode never calls into `adaptive_tune` at all today,
  so no separate confidence-specific recovery gate is needed — the existing gate upstream
  already covers it structurally. State this rather than adding a redundant check whose only
  effect would be dead code.
- **`abs_max_temp_c` on the Pico (SaftyFW) stays completely independent, per the brief's explicit
  instruction — it is never tightened, and this design does not touch it or reference it as an
  input to anything.** It is a separate processor's separate, hard-coded absolute ceiling; no
  confidence value computed on the ESP-S3 side should ever be wired to it in either direction.

## 7. Validation, realistically

This is inescapably a multi-firing behaviour: confidence is defined (§1) as a property of a
SEQUENCE of runs, so a single-firing test proves nothing about it beyond "the residual
computation didn't crash."

**`sim_iter_tune.c` does not fit directly**, exactly as the brief anticipates: its own header
comment states its Monte-Carlo structure explicitly — "runs the... decision core... closed-loop
against the maintained sim_plant.c kiln model, from several DIFFERENT starting gain sets" — each
trial is independent, with no state carried from one trial to the next
(`firmware/KilnFW/App/test/sim_iter_tune.c:1-9`). Confidence-graduated authority is specifically
about state that MUST persist and accumulate ACROSS runs (the consecutive-good-run counter, the
anchored baseline); a harness whose whole design point is independent trials cannot exercise the
one thing being tested.

**What is actually needed: a new, small, CHAINED single-zone harness** — not sim_iter_tune, not
a fork of it, a new file — that:
1. Links the real `adaptive_tune*.c` (not a mirror or a re-implementation — see
   `project_binding_a_python_mirror_to_c`, a mirror proves nothing about the production code) plus
   `sim_plant.c` for the plant response, the same "linked as-is, not reimplemented" discipline
   `sim_iter_tune.c` already follows for `pid.c`/`heater_output.c`.
2. Runs a SEQUENCE of N simulated firings against ONE zone, carrying `adaptive_tune`'s own
   persisted-equivalent state (K_dc, the baseline anchor, the new confidence counter) forward
   from firing to firing exactly as NVS would — this is the one new piece of harness plumbing
   sim_iter_tune's independent-trial design does not need and this design does.
3. Exercises, as separate scripted scenarios, not just the sunny-day path: (a) a consistent
   plant producing a genuinely converging fit (confidence should rise and plateau at the
   ceiling); (b) a step change in the simulated plant gain partway through the sequence
   (confidence should visibly fall the run immediately after the step, per §3); (c) an injected
   guard trip mid-sequence (confidence should reset); (d) a simulated autotune re-run
   mid-sequence (confidence should reset per §3's table); (e) an adversarial duty pattern that
   never varies (the §1 circularity-risk case — confidence should NOT reach the ceiling without
   the duty-spread gate being satisfied, proving the gate is load-bearing, not decorative).
4. Reuses `sim_plant.c`'s own stated ceiling honestly: per its existing caveat (restated in
   `sim_iter_tune.c`'s header, echoed for wide-sweep too), the model is only valid up to
   ambient+K_dc (~42-52 C) — so scenario (b)'s "step change in plant gain" must be injected as a
   parameter change in `sim_plant.c`'s own model, not attempted by asking the model to reach a
   temperature it cannot represent.

**Multi-zone results are explicitly out of scope for this validation, per the brief and
`docs/FUZZY_CONTROLLER_PLAN.md`'s own findings**: the coupling model has been refuted
(`project_z0_coupling_is_shape_not_scale`, `project_sim_firmware_coupling_model_mismatch`, and
two replacement attempts failed per `project_load_estimator_cannot_observe_load`) — any
multi-zone simulated result inherits that refuted coupling and would misrepresent confidence's
real-world behaviour on a coupled board. Single-zone chained sequences are the only trustworthy
validation surface available; the joint/coupled-matrix path (`adaptive_tune_refine_coupled_
locked()`, `adaptive_tune_model.c:335-464`) should be EXPLICITLY EXCLUDED from any confidence
scheme built from this design until a trustworthy multi-zone model exists to validate it against
— confidence-graduated authority on the coupled path would be unvalidatable by construction
today, not merely unvalidated.

## 8. Effort versus payoff — honestly

Relevant bench facts, both already established elsewhere in this repo and repeated in the task
brief: every controller effect measured on the bench so far sits at or below the project's 0.5 C
MAE materiality line (`feedback_ignore_sub_half_degree_effects`), and the bench physically cannot
exceed roughly 40 C above ambient (`project_bench_is_a_4w_test_fixture`,
`sim_iter_tune.c`'s own ~42-52 C model ceiling) while a real kiln reaches ~1200 C. `adaptive_tune`
itself, per `655da406` §2, has zero observations on any zone today — `enabled=False` everywhere,
never turned on.

This stacks three independent reasons the payoff cannot be demonstrated on this fixture right
now, not one:
1. There is no adaptation happening at all yet to grow authority over (the opt-in flag is off on
   every zone) — confidence-graduated authority has nothing to graduate until the base mechanism
   is actually turned on and produces its first accepted refinements.
2. Even once turned on, the bench's ~40 C span and ~4 W thermal mass cannot produce the kind of
   varied, materially significant plant behaviour (element ageing, load changes, multi-hundred-
   degree operating range) that would let a confidence estimator demonstrate it is tracking
   something REAL rather than bench noise — the same "cannot be evidenced on this fixture"
   conclusion `docs/FUZZY_CONTROLLER_PLAN.md` §8 already reached for the sibling fuzzy-confidence
   proposal, for the identical underlying reason (a 40 C bench cannot stand in for a 1200 C kiln's
   operating envelope).
3. Every effect this project has been able to measure on this hardware tops out at or below the
   0.5 C materiality line the owner has separately instructed this project to stop chasing —
   confidence-graduated authority is a mechanism for extracting MORE performance from an already
   well-fit model over time; if the ceiling on measurable benefit is 0.5 C on THIS hardware, the
   mechanism cannot even in principle produce a result worth the calibration effort (picking
   `alpha_max`, the good-agreement band width, the consecutive-run threshold) until it runs on a
   kiln where the base signal is large enough to matter.

**Verdict: design it, do not build it yet.** This is not a claim that confidence-graduated
authority is a bad idea — §1-§6 above is a complete, specific, non-circular design that resolves
every open question `655da406` §4 raised, and it should be picked up directly once two
preconditions are met: (i) `adaptive_tune` is actually turned on on at least one zone and has
produced real accepted refinements to have a track record over, and (ii) either a real kiln
firing (not this bench) or a validated (not refuted) simulated plant model is available to
calibrate and check the residual/stability thresholds against — calibrating `alpha_max`, the
good-agreement residual band, and the consecutive-good-run threshold against bench-only data
that tops out at 40 C and 0.5 C MAE would be fitting noise, not confidence. Building the
mechanism now, ahead of both preconditions, would produce host-tested code with untunable
constants — exactly the "uncertain payoff, cannot be evidenced on this fixture" verdict
`docs/FUZZY_CONTROLLER_PLAN.md` §8 already reached for the structurally identical fuzzy-layer
proposal, for the same underlying reason.

---

## Review, 2026-09-13 — adversarial: the non-circularity claim (§1) is REFUTED AS SPECIFIED; the bounded-walk invariant SURVIVES a varying alpha

Adversarial review of this document (`8c988093`) against the sources it cites. Design review
only; no code was changed by this pass. Hashes checked with `git cat-file -t`: `8c988093`,
`655da406`, `97288659`, `36f88d62` all resolve to commits.

**What was verified against source versus taken on this document's word.** Verified by reading:
`adaptive_tune_internal.h:114-145` (the constants and their values), `adaptive_tune_model.c:38-190`
(the whole `adaptive_tune_refine_zone_locked()` guard chain, the baseline anchor, the blend and
the clamps), `adaptive_tune.c:365-380` (the observation ring's write path), `adaptive_tune.c:421-460`
(`reset_run_status_locked()` and the lifetime-latch carve-out), `adaptive_tune.c:536-600`
(`adaptive_tune_run_end()`'s skip chain), `adaptive_tune_internal.h:279-320` (the per-zone state
struct), and `zones_http_post_parse.c:400-500` and `:690-760` (the carry-through block and the
model-field parse block). Taken on this document's word, not re-derived: the `sim_iter_tune.c`
header quotation, the `docs/FUZZY_CONTROLLER_PLAN.md` §8 comparison, and the bench-physics facts
in §8.

### 1. Circularity (§1) — the argument is right in principle and wrong as written

Two separate things are tangled in §1, and they deserve opposite verdicts.

**The principle is sound, and more strongly than §1 argues it.** §1 defends the forward check by
saying it compares plant observations rather than tracking error. That is true but understates
the case. At a settled dwell, `profile_executor.c` zeroes the target rate, so duty is pure hold
term, and integral action drives duty to whatever the *true* plant requires to hold that rise:
`u ≈ rise / K_true`. The observed `(duty, rise)` pair is therefore set by the plant and the
setpoint, **not** by the model. So the brief's specific worry — "can a model error bias duty into
a region where the model happens to fit well" — is largely answered: within a settled dwell, a
wrong model does not *relocate* the operating point, it only makes the transient to reach it
worse. That is a genuine, checkable reason the loop is not closed, and it is stronger than the
one §1 gives.

**But the implementation §1 specifies is in-sample contaminated, and that is a load-bearing
defect.** §1 says the residual is scored over "the settled-dwell observations
`adaptive_tune_zone_tick()` records during run N+1 (the same (duty, rise-over-ambient) pairs
already harvested for the ring, `adaptive_tune.c:319-355`)". **The ring is not per-run.**
`adaptive_tune.c:365-380` is the only writer, and nothing anywhere in `adaptive_tune.c` ever
clears `ring_count`/`ring_head` at a run boundary — the only `ring_count` references in that file
are the append path and the status getter. It is a rolling 12-deep window (`ADAPTIVE_TUNE_RING_CAPACITY`)
spanning however many runs it takes to fill, evicting oldest. Consequences:

- At run N+1's end, the ring holds a *mixture* of run N+1's observations and run N's — the very
  observations run N's `K_fit` was trained on. Scoring run N's model against that mixture is a
  partly **in-sample** residual, which is exactly the self-validating shape §1 claims to avoid.
  The contamination fraction is not small: if a firing contributes 3-4 dwell observations, a
  majority of a 12-deep ring at run N+1's end is pre-N+1 data.
- The reuse §1 relies on ("the same pairs already harvested") is therefore **not available as
  stated**. A forward check needs per-run partitioning of the ring (a run-boundary marker, or a
  separate per-run scoring buffer) that does not exist today. This is new bookkeeping the design
  does not budget for, and it is a precondition for the residual to mean what §1 says it means.

**The duty-spread gate does not close the general case — it closes almost nothing.** Two findings,
both traced:

- Under §1's own predictor, `predicted_rise = K_N * duty`, the residual is
  `|K_N·u − K_true·u| / (K_true·u) = |K_N − K_true| / K_true` — **duty cancels**. For a linear
  model the fractional residual is *independent of the duty at which it is measured*, so duty
  spread contributes literally zero information to this signal. Spread only matters if the true
  plant is nonlinear in duty, and `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` is 0.05 — a 5-percentage-point
  duty band (`adaptive_tune_internal.h:116`, checked at `adaptive_tune_model.c:53`). On a kiln
  that is a sliver of the operating envelope. A single-purpose kiln firing the same profile every
  time clears a 5% spread trivially while never leaving its regime — the precise case §1 says the
  gate closes.
- The gate is also computed over the *current ring only* (`adaptive_tune_model.c:45-56`,
  `umin`/`umax` over `z->ring_count` entries), i.e. a rolling 12-sample window. §1 describes it as
  applying "over the runs being scored" and speaks of "the historically-seen duty band" — a
  cross-run coverage notion that has no representation in the code. The design reuses a named
  constant while silently redefining its semantics. That is the same class of error this repo
  logs as `project_unchanged_code_is_not_unchanged_behavior`.

**The real residual leak §1 misses is selection, not location.** A badly wrong model degrades the
approach to dwell; observations are only harvested once the settle gates pass
(`ADAPTIVE_TUNE_SETTLE_MIN_S`, the slope floor, and the duty-stability gates), and a whole run can
be dropped by `ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION` at `adaptive_tune.c:559-575`. So the *set* of
observations that reaches the scorer is filtered by how well the current model is behaving:
regimes where the model is worst are preferentially discarded, and §3's table scores those runs as
"no change either direction". That is survivorship bias feeding a confidence estimator, and it
biases confidence **upward** — the dangerous direction. It is a weaker loop than tracking-error
self-scoring, but it is a loop, and the design does not name it.

**Verdict on §1: the non-circularity argument is REFUTED as specified.** Not because
forward-checking is wrong — it is the right idea and the settled-dwell reasoning above is a better
defence than the one written — but because (a) the data source named is cross-run and would make
the residual partly in-sample, (b) the gate leaned on to close the narrow case is duty-independent
for this predictor and only 5 points wide, and (c) the actual leak is selection bias, unaddressed.
None of the three is fatal to the concept; all three are fatal to building §1 as it currently
reads.

### 2. The floor-state safety claim (§4) — true only with a read-side clamp the design never states

§4's claim, restated: "a bug in the NEW confidence-scoring code ... can only ever fail to RAISE
authority ... it cannot on its own lower a guard below today's shipped values." This is asserted,
not constructed. It holds **only** if the mapping from the persisted confidence field to `alpha`
is clamped on read into `[0.15, alpha_max]`. The design never says so, and its persistence
proposal makes the unclamped case reachable:

- A corrupted, partially-migrated, or out-of-range field under a natural linear mapping
  (`alpha = 0.15 + conf·(alpha_max − 0.15)/CONF_MAX`) yields `alpha < 0.15` for negative `conf`,
  and `alpha < 0` for sufficiently negative `conf`. **A negative alpha is not merely "more
  conservative" — it breaks the convexity premise of the ratchet invariant** (see §3 below):
  `k_blended = k_dc + α(k_fit − k_dc)` with `α < 0` moves `k_dc` *away* from `k_fit`, so
  `k_blended` is no longer in `[min(k_dc,k_fit), max(k_dc,k_fit)]`. The `±20%` clamp at
  `adaptive_tune_model.c:158-160` is relative to the live `k_dc`, so it bounds the step but not
  the walk: `k_dc` can then march 20% per run in a direction no `k_fit` ever pointed, out of the
  `[baseline/5, 5·baseline]` envelope, stopping only at `ADAPTIVE_TUNE_K_DC_ABS_MAX`. So the
  floor claim is not just wrong at the margins — its failure mode reaches through and destroys the
  `97288659` lifetime bound. **Mandatory addition to this design: clamp on read, and treat any
  out-of-range or non-finite confidence value as the floor, not as a value to map.**
- §3 chooses to store confidence in `zone_cfg_t` "for the carry-through discipline". That is
  reasonable, but it also puts confidence inside the struct that `POST /api/zones` rebuilds. The
  design must state explicitly that confidence has **no** `z%u_` POST key — otherwise confidence
  becomes operator-writable, and a config write silently becomes an authority grant with no
  evidence behind it.
- **The carry-through policy stated in §3 is mechanically right but wrong for one case.**
  §3 prescribes `z->confidence = current_z->confidence;` on the `36f88d62` pattern. But
  `zones_http_post_parse.c:490-493` shows this file already distinguishes two policies:
  unconditional carry-through (`autotune_baseline_k_dc`, `model_fit_*`, `adaptive_tune_enabled`)
  versus **carry-then-invalidate-on-actual-change** (`tuning_valid`, zeroed when
  `|Δpid_kp|>0.0001` etc.). Confidence belongs in the *second* category, not the first: a
  whole-page save that changes the PID gains has replaced the thing confidence was accumulated
  about. As written, §3 would preserve a maxed-out confidence across an operator gain edit.

### 3. The bounded-walk invariant under a varying alpha — IT SURVIVES

Redone from `adaptive_tune_model.c:157-176`, not taken on the document's word.

Let `B = autotune_baseline_k_dc`, `R = ADAPTIVE_TUNE_MAX_JUMP_RATIO = 5`, `I = [B/R, B·R]`.
Invariant: `k_dc ∈ I` before a run ⟹ `k_dc ∈ I` after.
1. `k_fit ∈ I` unconditionally — `adaptive_tune_model.c:125` refuses otherwise, and `B` is never
   written by this function (only by `autotune_engine_guard.c`'s accept path and the one-shot
   bootstrap at `:111-118`).
2. `k_blended = k_dc + α(k_fit − k_dc)`. For **any** `α ∈ (0,1]` this is a convex combination, so
   `k_blended ∈ [min(k_dc,k_fit), max(k_dc,k_fit)] ⊆ I`.
3. The clamp at `:158-160` maps `k_blended` to within `k_dc ± 0.2·k_dc`, and since `k_blended` is
   already on the `k_fit` side of `k_dc`, the clamp only ever moves it **toward** `k_dc` — an
   interior point of `I`.

Nothing in steps 1-3 uses the numeric value 0.15. **The invariant is a property of `α ∈ (0,1]`,
not of `α = 0.15`.** It survives `alpha_max = 0.5`, and would survive `alpha_max = 1.0`. The
`97288659` proof does **not** need re-deriving, and the §5 ceiling `alpha_max < 1.0` is justified
by convergence and noise arguments — not by the envelope, which does not need it. §2's own
statement ("bounded by construction ... cannot itself push K_dc outside `[K_fit, K_dc]`") is
correct. This is the one load-bearing claim in the document I checked and could not break, and
the design should say plainly that the bound is α-agnostic rather than leaving it implicit.

**Two unadvertised side effects of modulating alpha, both traced, neither mentioned:**

- **Ki-layer starvation via the D5 arbitration.** `adaptive_tune_run_end()`'s D5 rule
  (`adaptive_tune.c:~590-615`) lets `adaptive_tune_refine_ki_locked()` run *only* on runs where
  the model refine did not fire. Raising `alpha` makes `|k_blended − k_dc|` larger, so more runs
  clear `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC` (`adaptive_tune_model.c:183-189`) and the model
  refine fires more often — **which starves the Ki diagnosis of turns, precisely on the zones that
  earned the most confidence.** Modulating alpha is therefore not a pure "act more eagerly" knob;
  it silently reallocates authority between two layers.
- **The two proposed knobs compound in the same direction, which is what §2 says it avoided.**
  §2 declines to also modulate `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE` because "modulating both at
  once from the same confidence signal risks a compounding effect". But `MIN_OBSERVATIONS` and
  `alpha` compound just as directly: per-run `K_dc` jitter scales roughly as `α·σ/√n`, so
  `0.15 → 0.5` with `n: 4 → 2` is about a **4.7x increase in run-to-run gain jitter** — more trust
  applied to a noisier estimate. At `n = 2` an OLS gain fit passes through its two points exactly
  and rejects no noise at all. The §2 compounding argument is applied inconsistently; if it
  disqualifies the per-run cap it also disqualifies relaxing `MIN_OBSERVATIONS` alongside alpha.
  Recommendation: modulate `alpha` only in a first cut, and leave `MIN_OBSERVATIONS` at 4.

### 4. The fall path — stale-state enumeration, three real gaps

§3's table covers four reset triggers. Enumerating everything that goes stale when confidence
resets, or that changes the model without resetting confidence:

| State | Covered by §3? | Finding |
|---|---|---|
| The consecutive-good-run counter | Yes | — |
| `K_dc_after_run_N` (the prediction being scored) | Not listed, but OK | Derivable from live `model_k_dc`, since this function is its only routine writer — *except* via the operator path below |
| The observation ring (`ring`, `ring_count`, `ring_head`) | **No** | RAM-only, cross-run, never cleared. Two consequences: (a) after a reset triggered by a genuine plant change, the ring still holds pre-change observations and the next fit is trained on a mixture; (b) **after a reboot the ring is empty while persisted confidence is still high** — so a relaxed `MIN_OBSERVATIONS` would let the first 2 observations after a power cycle drive a max-alpha refinement. This is a textbook `project_reset_one_side_bug_class` pair: confidence persisted, its evidence base not |
| Operator `POST /api/zones` writing `z%u_k` | **No** | `zones_http_post_parse.c:701-710` — `model_k_dc` **is** operator-writable, bounded only by `ZONE_MODEL_K_MAX`. An operator can replace the model by hand and confidence carries straight over |
| Operator `POST /api/zones` **omitting** `z%u_k` | **No** | Same block, and this file's own comment calls it "this file's OWN documented sharp edge": omitting the key **deletes** the model (`z` is zero-initialised; 0 is the "no model" encoding). Refinement then refuses at `adaptive_tune_model.c:69-73`, but confidence sits latched at maximum until a model reappears — at which point full authority applies to a model with zero runs behind it |
| Operator gain edit on the same page | **No** | See §2 above — must follow `tuning_valid`'s invalidate-on-change, not unconditional carry |
| `model_fit_temp_c` / `model_fit_ambient_c` | **No** | The operating point each fit was taken at (and now `zone_model_at()`'s schedule seam, `5d3bc854`). If confidence is meant to be a statement about a model at an operating point, these are part of what goes stale |
| `has_applied` / `prior_k_dc` / `applied_k_dc` / `last_delta_pct` | Partly | `adaptive_tune.c:440-447` (K2) documents these as **lifetime latches**, deliberately *not* reset on skip paths. Whether confidence is a lifetime latch or a per-episode counter must be stated; `adaptive_tune_revert()` clears `has_applied` (`:1065`) but the design does not say whether the revert reset is symmetric |
| `ki_baseline` / `ki_baseline_valid` | **No** | Persisted in this module's own NVS blob, documented "latched once, never overwritten". A confidence reset on a new autotune Accept coexists with a Ki baseline that may or may not be re-latched — the design should say which |

The `36f88d62`-class carry-through requirement itself is **correctly stated** as a mechanism (the
line goes in `zones_http_post_parse.c`'s per-zone rebuild; the test extends `test_zones_http.c`;
both in the same commit). The policy attached to it is wrong for the gain-change case, per §2.

### 5. Validation (§7)

**The `sim_iter_tune.c` judgement is correct** — an independent-trial Monte Carlo harness cannot
exercise state whose whole point is accumulating across trials. A new chained single-zone harness
is the right call, and the "link the real `.c`, do not mirror it" discipline
(`project_binding_a_python_mirror_to_c`) is correctly invoked.

The five scenarios are necessary but not sufficient. Add:

- **(f) Reboot mid-sequence.** Drop the RAM-only ring while carrying persisted confidence forward,
  and assert the first post-reboot run does not act at full authority on 2 observations. This is
  the §4 gap above and it is the cheapest one to catch in a harness.
- **(g) Operator whole-page `POST /api/zones` mid-sequence**, in both shapes: one that changes the
  gains, and one that omits `z%u_k` and deletes the model. Assert confidence resets. `36f88d62`
  is proof this call site is missed by inspection.
- **(h) A quantized, noisy plant.** `project_idealized_test_input_bug_class` — a perfectly linear
  noiseless `sim_plant.c` will produce shrinking residuals and a ratchet to the ceiling by
  construction, proving nothing. The load-bearing test is that confidence does **not** reach the
  ceiling on a plant whose apparent gain wanders only by sensor noise.
- **(i) Recovery after a fall.** Every §3 row tests a drop; none tests that confidence then
  climbs back correctly and by the right increments.
- **(j) A negative test of the mapping itself.** `feedback_negative_test_every_check`: break the
  confidence→alpha mapping in the *production* function (not a test-local copy —
  `project_negative_test_on_a_mirror_is_vacuous`) and prove a scenario fails. Restore by hand,
  never `git checkout --`.

Scenario (e) as written cannot do its stated job while `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` is the
gate — per §1 above, a 5% within-ring spread is clearable by an invariant firing pattern, so the
harness would show the gate passing and confidence ratcheting, which is the correct result for a
gate that does not do what §1 claims. (e) should be rewritten to assert that outcome and force the
design to specify a real coverage gate.

### 6. Build-now-or-later — I agree with the verdict, but two of its three grounds are the weak ones

Agreed: **do not build it yet.** But the reasoning should be re-ordered, because two of the three
grounds would not survive a determined push-back.

- Ground 1 ("zero observations on any zone today") is the weakest. It is self-imposed and
  removable in one config write — `adaptive_tune_enabled` is a per-zone flag. "We have not turned
  it on" is a schedule fact, not a design argument.
- Ground 3 (the 0.5 C materiality floor) is close to circular in the other direction: it argues
  that a mechanism for improving long-run model fidelity is not worth building because *current*
  bench effects are small. That conflates the size of the effect with the size of the risk. This
  mechanism's payoff is not 0.5 C of MAE on a bench; it is not drifting on a 1200 C kiln over
  fifty firings. `feedback_ignore_sub_half_degree_effects` is about not *chasing* sub-0.5 C
  effects, not about declining to bound a drift mechanism.
- Ground 2 (the thresholds cannot be calibrated on a 40 C, 4 W fixture) is the real one, and it is
  sufficient on its own. The good-agreement residual band, the consecutive-run threshold, and
  `alpha_max` are all numbers that must be fitted to a plant, and fitting them to this bench would
  be fitting noise.

**And a fourth ground, which this review adds and which is stronger than 1 and 3: the design is
not implementable as written.** §1's data source does not exist in the form it names (the ring is
cross-run, `adaptive_tune.c:365-380`), so the residual it specifies would be in-sample. That must
be resolved — per-run partitioning, a coverage gate that is not
`ADAPTIVE_TUNE_MIN_DUTY_SPREAD`, and an explicit read-side clamp — before this document is a
buildable design rather than a direction. **Do not pick this up off the shelf as-is when the two
preconditions in §8 are met; §1, §3's reset table and §4's floor claim need the revisions above
first.**

### Summary of required changes before this design is buildable

1. §1: partition the ring per run (or add a dedicated scoring buffer) so the forward residual is
   genuinely out-of-sample. Not optional — it is the difference between the design's central claim
   being true and false.
2. §1: replace `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` as the coverage gate. It is duty-independent for
   this predictor, 5 points wide, and computed over a rolling 12-sample window. State the
   selection-bias leak (non-settling runs excluded ⇒ confidence biased upward) explicitly.
3. §4: mandate a read-side clamp of confidence into `[floor, ceiling]`, treating non-finite and
   out-of-range as floor. Without it a corrupted field can produce `α ≤ 0` and break the
   `97288659` envelope invariant outright.
4. §3: confidence must have no `z%u_` POST key, and must follow `tuning_valid`'s
   invalidate-on-actual-gain-change policy, not `autotune_baseline_k_dc`'s unconditional carry.
5. §3: add reset triggers for operator `model_k_dc` write, operator `z%u_k` omission (which
   deletes the model), and reboot-with-empty-ring; state confidence's relationship to
   `has_applied`'s lifetime-latch semantics and to `ki_baseline`.
6. §2: either modulate `alpha` alone, or apply the compounding argument consistently and explain
   why `alpha × MIN_OBSERVATIONS` (≈4.7x jitter) is acceptable when `alpha × MAX_FRACTIONAL_MOVE`
   is not. Note the D5 Ki-starvation side effect either way.
7. §5: state that the envelope invariant is α-agnostic over `(0,1]` — the `alpha_max < 1.0`
   ceiling is a convergence/noise choice, not a safety requirement.
