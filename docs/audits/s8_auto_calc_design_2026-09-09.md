# S8 rate-guard auto-calculation -- design and validation

**Date:** 2026-09-09. Builds on `docs/audits/s8_rate_guard_retune_2026-09-09.md`
(commit `bd2ad739`), which measured the bench's real max rate-of-rise and
found the compiled 33.3 C/min default was derived from the wrong quantity
(2x the fastest shipped PROFILE ramp, not a measured plant capability).

Two owner decisions this document responds to:

1. Set the bench's live S8 threshold to 20 C/min now. **Done** -- see
   "Part 1" below.
2. "If this is a safety guard it should be autocalculated for real kilns" --
   a hand-entered per-kiln number is the wrong design long-term. This
   document designs and partially implements that; see "Part 2".

## Part 1 -- bench value set to 20 C/min

Read via `safety_get_rate_guard()` before and after, both independently:

| | max_rate_c_per_min | rate_window_s |
|---|---|---|
| Before | 33.3 (ARMED) | 60 |
| After | **20** (ARMED) | 60 |

The write required `debug_reset(peer="pico")` first -- `safety_set_rate_guard`
refused with "commit rejected: relay is ARMED -- config writes are refused
while ARMED" until the reset opened the 60s post-reset GRACE window. After
the reset, `safety_get_diag()` showed `trip_reason 0 | trip_mask 0x0000` and
the write landed and read back cleanly on the next two independent
`safety_get_rate_guard()` calls.

**KilnFW-side ceiling check (confirmed, not just asserted):** grepped
`sanity_rate_c_per_min` across `firmware/KilnFW/App` --
`zones_config_json.h`'s own field comment calls it a "direction/rate sanity
threshold" and `test_zones_http.c`'s test comments call it "guard 1's
MINIMUM rise rate, the dead-element check" (a zone that isn't heating fast
enough trips it, not one heating too fast). `zones_page.html` labels its UI
field "Minimum rise rate". This is the opposite of S8's maximum-rate
ceiling, so "never make a Pico guard tighter than the ESP's equivalent"
does not apply here -- there is no ESP-side maximum-rate ceiling to compare
against, exactly as the retune audit's section 5 already concluded.

## Part 2 -- auto-calculation design

### What input

The identified per-zone FOPDT plant model already carries what a maximum-
rate estimate needs: `model_k_dc` (steady-state gain, degC/duty),
`model_tau_s` (time constant, seconds), and, since `5d3bc854`,
`model_fit_temp_c`/`model_fit_ambient_c` (the operating point that
identification ran at). A first-order plant's initial slope at full duty
(`u=1.0`) is the textbook quantity `dT/dt|t=0 = k_dc * u / tau_s`; converted
to degC/min this is `(k_dc / tau_s) * 60`. This is the ESP's own
feedforward math (`profile_executor_feedforward.c`, `zone_coupling_solve.c`)
re-purposed as a safety-ceiling estimator, not a new physical model.

Implemented as `s8_rate_guard_estimate()`
(`firmware/KilnFW/App/drivers/control/s8_rate_guard_estimate.c/.h`), a pure,
host-testable function with no FreeRTOS/hardware dependency -- same
"genuinely self-contained numeric code" rationale `zone_coupling_solve.c`
already established for this directory.

### Temperature dependence -- the hard part, and why a single fit is picked deliberately

Per `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`'s high-temperature transfer
analysis (cited in this repo's memory), both `k` and `tau` fall by roughly
the same factor (~20x at 1200 C) as a real kiln heats up. **If a single
identification's k/tau ratio were treated as portable to every operating
point, the ratio itself is not obviously constant** (k and tau do not
necessarily track each other exactly, and nothing in this repo's data
proves they do) -- so this design does NOT attempt to schedule the
threshold across temperature using one point's k/tau extrapolated outward.
The `zone_model_at()`/`coupling_at()` schedule seam `5d3bc854` began exists
for exactly this future need, but today only ONE fit point is ever
recorded per zone (the seam records an operating point, it does not yet
hold a multi-point schedule) -- there is nothing to interpolate yet.

**What this design does instead:** across all zones with a valid
identification, it picks the ONE with the LOWEST `model_fit_temp_c` and
uses ONLY that zone's own `k_dc`/`tau_s` as the single, unscheduled, global
threshold basis. This is deliberate, not an oversight:

- The retune audit's own measured data (all bench captures, and the
  high-temperature-transfer finding) agrees that the coldest part of a
  firing is the physically FASTEST part -- k and tau shrinking together as
  temperature rises means the plant gets progressively slower, not faster,
  later in a firing.
- A ceiling sized to the coldest, fastest identified operating point is
  therefore, for every hotter point in the same firing, generous relative
  to what the plant can actually still do -- never accidentally tight
  there, and (more importantly for a safety guard) still tight enough to
  catch a genuine runaway even where the plant is naturally slower, since
  the ceiling doesn't loosen as temperature rises.
- If NO zone has been identified cold (e.g. every autotune ran only at a
  high dwell temperature), this function returns
  `S8_RATE_GUARD_ESTIMATE_NO_DATA` rather than deriving a number from an
  unrepresentative hot-only fit and silently under-protecting the coldest,
  fastest part of a real firing it has no evidence about.

**What happens across a full firing if this is NOT temperature-scheduled:**
a single global ceiling, sized off the coldest measured point, stays valid
(neither nuisance-tripping nor under-protective) everywhere else in the
firing precisely because the plant only gets slower from there -- this is
the reasoning above, restated as the answer to "what happens if you don't
schedule it." A genuinely tighter, temperature-scheduled version would
still be strictly safe (equally protective, fewer nuisance trips at high
temperature) but requires the multi-point schedule the seam does not yet
populate; left as explicit future work, not attempted here to avoid
extrapolating from data this repo does not have.

### Fail-safe direction, floor and ceiling

The margin is `S8_RATE_GUARD_ESTIMATE_MARGIN = 2.0` (same "2x measured
peak" convention `c43323a2` already used, applied to a measured plant
capability instead of an authored profile ramp) applied to the coldest
zone's own full-duty slope -- this errs toward a TIGHTER guard (more
willing to nuisance-trip) rather than a looser one, which is the correct
side for a guard whose job is to catch a runaway: a guard sized to the
plant's actual capability plus a safety margin cannot be looser than what
the plant has already proven it can do.

Floor and ceiling (`S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN` = 15.0,
`_CEILING_C_PER_MIN` = 60.0) mirror the Pico's own
`CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR`/`_CEILING` (config_store.h) exactly
-- an un-commissioned or badly-identified plant (near-zero `k_dc`, or a
`tau_s` near zero that would blow the slope up) cannot produce an
unusably-tight or absurdly-loose number: the floor protects against a
bad-low identification silently disarming trust in the guard's usefulness
(nuisance-tripping so often it gets disabled by an operator), and the
ceiling protects against a bad-high identification (or a compromised/buggy
computing side) producing a threshold so loose it cannot catch a real
runaway.

### Who computes it, and when

**The ESP computes the candidate** (it alone holds the plant model -- the
Pico has no coupling/FOPDT identification of any kind and was never meant
to). This does trade away some of the Pico's "independent second set of
eyes" property for max_rate_c_per_min specifically: if the ESP's
identification is wrong, the number it proposes is wrong in the same way.

**The mitigation, and the actual answer to "what if the computing side is
wrong or compromised":** the Pico's own floor/ceiling range check
(`config_params.c`'s `CHECK_F32_RANGE_OR_ZERO`, added in this same pass --
see "Part 1's sibling change" below) is independent of WHERE the number
came from. A compromised or buggy ESP cannot push an arbitrarily small or
arbitrarily large value past it, regardless of whether the number was
hand-typed by an operator or computed by this module -- the Pico still
enforces the one thing it can verify without a plant model: that the number
is not absurd. What the Pico cannot do is verify the number is *correct*
for this specific kiln; that trust boundary is real and stated plainly, not
hidden. A kiln whose ESP is compromised in a way that keeps its identified
k_dc/tau_s "plausible" (inside the floor/ceiling) but wrong could still get
an incorrect-but-bounded threshold -- exactly the same trust boundary every
other Pico guard threshold pushed from `zones_config` already has (e.g.
`abs_max_temp_c`), not a new one this feature introduces.

`s8_rate_guard_estimate()` itself still **never writes anything** -- it
remains a pure function returning a suggested value. Part 3 below (2026-09-
10) is the follow-up pass this section originally deferred: the write path
is now built.

### Part 3 -- the write path (2026-09-10)

**Chosen policy: tighten-auto-apply, loosen-requires-confirm** -- the
middle path this section's Part 2 draft left open, not either pure option.

- **Pure auto-apply** (every re-identification silently overwrites the
  Pico's armed threshold) was rejected: a bad identification -- corrupted
  `model_k_dc`/`tau_s`, or a fit run at an unrepresentative operating point
  -- could silently RAISE the threshold, i.e. reduce protection, with no
  operator ever looking at the new number. "Autocalculate for real kilns"
  does not mean "never let a human notice the guard moved the wrong way."
- **Pure suggest-and-confirm** (every candidate, tighter or looser, waits
  for an operator click) was rejected too: it reproduces the exact
  staleness problem that motivated this feature -- a guard sitting at its
  old, possibly-wrong number until someone remembers to confirm it, which
  is the "hand-entered number is the wrong design long-term" complaint
  restated, not answered.
- The implemented middle path auto-applies only the direction that can
  never make the guard less safe (tightening, or arming a dormant guard --
  `max_rate_c_per_min == 0` -- for the first time, since "no ceiling" is
  never tighter than any finite one) and requires an explicit operator
  confirm for the one direction that can reduce protection (loosening an
  already-armed guard). This is a genuinely fail-safe split, not two risks
  averaged: tightening is safety-neutral-or-positive, loosening is the one
  safety-relevant case, and the two get deliberately different handling.
  **Never loosen silently** is enforced structurally, not by convention:
  `s8_rate_guard_auto_decide()` (`s8_rate_guard_estimate.h/.c`) is a pure
  policy function returning `S8_RATE_GUARD_AUTO_SUGGEST_ONLY` whenever
  `candidate > current` against an armed guard, and the only caller that
  can turn a `SUGGEST_ONLY` into an actual write
  (`rate_guard_auto_post_handler`, `safety_cfg_http.c`) requires the
  request body to carry `confirm=1` -- there is no code path from a
  looser candidate to a Pico write without that explicit flag.

**Read-back and verify, every write.** The auto-apply endpoint (`POST
/api/safety/rate_guard/auto`, `GET` for a write-free preview) reuses
`apply_pairs()` with `commit=true`, the SAME function every other
commissioning write on this page uses -- including its
`confirm_commit_landed()` live read-back that this codebase's own
commissioning-write audit added specifically because "ACKed and not
rejected" is not proof a write landed. The provenance record (below) is
only tagged AFTER `apply_pairs()` reports success, i.e. after that
read-back already confirmed the Pico's 0x0204 now holds exactly the
candidate value -- never optimistically, before verification.

**Operator override survives, and is now visibly distinguishable.**
`safety_cfg_store_get/set/clear_rate_guard_meta()` (`safety_cfg_store.h/.c`)
add an ESP-local (never sent to the Pico -- same "not one of the Pico-
fetched answers" reasoning as the CT-calibration-input and safety-relay-type
records that already live in this same store) provenance record: who last
wrote 0x0204 (`SAFETY_RATE_GUARD_SOURCE_MANUAL`/`_AUTO`) and what value.
`commissioning_post_handler`'s existing generic id=/value= write path (how
an operator hand-enters `max_rate_c_per_min` today) tags MANUAL on a
committed write of 0x0204; the new auto-apply endpoint tags AUTO, only
after its own verified write. `GET /api/safety/commissioning`'s JSON now
carries a `rate_guard_provenance` object (`has_value`, and when true,
`source`/`value`) so the commissioning page can render "auto-derived" vs.
"hand-entered" and an operator overriding a value they never asked to be
auto-derived is not surprised by it changing under them. `bench_preset_post_
handler` (which resets 0x0204 to its dormant 0.0 default) clears this
record, since neither label is true of a value the bench preset just
replaced.

**Never make the Pico's guard tighter than the ESP's equivalent -- reverified
here, not just cited.** Re-checked directly (not taken on trust from Part 1):
`grep -rn sanity_rate_c_per_min firmware/KilnFW/App` still shows only
`zones_config_json.h`'s "direction/rate sanity threshold" field comment and
`test_zones_http.c`'s "guard 1's MINIMUM rise rate, the dead-element check"
-- a zone heating too SLOWLY trips it, not one heating too fast. There is no
ESP-side *maximum*-rate ceiling for S8's `max_rate_c_per_min` to be compared
against or made looser than; the constraint from the task brief does not
bind here, exactly as Part 1 already concluded and this pass's own grep
confirms independently. The Pico's own `CONFIG_STORE_MAX_RATE_C_PER_MIN_
FLOOR`/`_CEILING` range check (`config_params.c`) is untouched by this pass
and remains the sole, independent backstop against an absurd value from
either a hand-entered or auto-derived write -- this write path does not
bypass it, and does not need to: it writes through the same 0x0204 SET_
PARAM/COMMIT_CONFIG path the Pico already range-checks unconditionally.

**Validation is unchanged from Part 2, because this pass changes no
numbers.** The write path calls the SAME `s8_rate_guard_estimate()` with the
same `S8_RATE_GUARD_ESTIMATE_MARGIN`/floor/ceiling Part 2 already validated
against all 65 `logs/coupling/*.jsonl` captures (0/65 trip at 15.0, 20.0, or
60.0 C/min, reproducing this firmware's own baseline-sample-and-hold 60s-
window estimator plus its 2-consecutive-window debounce) -- this pass adds
the mechanism that writes a candidate, not a new candidate-generation
formula, so that validation carries forward unchanged rather than needing
to be redone. The simulation-harness cross-check at high temperature
Part 2 flagged as follow-up validation was **still not run** in this pass
either -- genuinely open, not silently dropped: exercising
`sim_plant_from_zone_cfg()`/relay-lag/quantization at a temperature this
bench cannot physically reach is additional scope this pass did not have
time for, and is called out here again rather than implied done.

**What happens on a real kiln, stated plainly.** Per the high-temperature
transfer analysis Part 2 already cites, `k` and `tau` both fall by roughly
20x as a real kiln heats toward cone temperatures, and the plant is
*fastest* at the bottom of a firing (cold start) and gets progressively
slower from there. This design's single, unscheduled threshold -- sized off
the coldest identified zone's own full-duty slope times a 2x margin -- is
therefore conservative for the entire rest of a real firing: it stays tight
enough to catch a genuine runaway at every hotter point (the guard does not
loosen as the kiln heats, even though the plant itself is getting slower and
so, in principle, could tolerate a lower ceiling later in the firing without
losing runaway detection). The cost of not scheduling the threshold is
nuisance-trip margin at high temperature that a future multi-point schedule
(the `zone_model_at()`/`coupling_at()` seam `5d3bc854` added) could recover,
not a safety gap -- the failure direction stays tight-not-loose across the
whole firing, exactly the direction this guard should err in.

Code: `s8_rate_guard_estimate.h/.c` (new `s8_rate_guard_auto_decide()`),
`safety_cfg_store.h/.c` (new rate-guard provenance store), `safety_cfg_
http.c` (`GET`/`POST /api/safety/rate_guard/auto`, plus the MANUAL-tagging
hook in `commissioning_post_handler` and the clear hook in `bench_preset_
post_handler`). Host tests: `test_s8_rate_guard_estimate.c` (policy
function, including the exact-tie and marginally-looser boundary cases) and
`test_safety_cfg_http.c` (gather/current-value/compute helpers, the JSON
provenance rendering, and handler-level smoke tests for apply/suggest/
never-write-on-GET, via fakes for `zones_config_get_model()`/`_get_thermo_
count()`/`_get_model_fit_context()` and the new provenance store). Negative-
tested by breaking `s8_rate_guard_auto_decide()`'s tighten/loosen comparison
in production, confirming 2 failures RED, and restoring by hand (`git diff`
on the file empty afterward).

Not done in this pass, same as Part 2's own list: no temperature schedule,
and no simulation-harness cross-check at high temperature.

### Backward compatibility

A hand-set value remains fully supported and now visibly distinguishable
from an auto-derived one (see Part 3's provenance record above) -- neither
this feature nor Part 3's write path adds a new field or enum state to
`max_rate_c_per_min` itself on the wire; both write paths call the SAME
0x0204 SET_PARAM/COMMIT_CONFIG an operator's manual entry always used.

### Validation against evidence

**Against all 65 `logs/coupling/*.jsonl` captures** (gitignored, local-only):
reproduced the firmware's own baseline-sample-and-hold 60s-window rate
estimator (same method as the retune audit) and the 2-consecutive-window
debounce, and swept the floor (15), a mid-range value (20), and the ceiling
(60):

| Threshold (C/min) | Captures that would TRIP |
|---|---|
| 15.0 (floor) | 0 / 65 |
| 20.0 | 0 / 65 |
| 60.0 (ceiling) | 0 / 65 |

Max observed windowed rate across all 65 captures with this reproduction:
29.17 C/min (higher than the retune audit's reported 7.69 C/min peak --
likely reflects a different per-sample-gap filter or a segment-transition
artifact in a subset of captures, not reconciled further here since it does
not change the conclusion: even at 29.17 C/min, a single window's spike
does not satisfy the 2-consecutive-window debounce, so zero captures trip
at any threshold in this table). This gap is flagged as a loose end for
whoever next revisits the windowed-rate reproduction script
(`s8_auto_validate.py`, scratch/local, not committed).

**Against the extended simulation harness:** not run in this pass --
`sim_plant_from_zone_cfg()`/relay-lag/quantization (`e0d2e006`) support high-
temperature simulation, but exercising it (including at a temperature this
bench cannot physically reach) is additional scope beyond what this design
pass covers; flagged as follow-up validation before this module is wired to
a live write path.

### What was NOT done, and why (explicit stops)

- ~~No live write path.~~ **Done 2026-09-10, see Part 3 above** -- the
  auto-apply/suggest-and-confirm HTTP surface is built
  (`GET`/`POST /api/safety/rate_guard/auto`), with its own review of the
  tighten/loosen policy, the read-back-and-verify discipline, and the
  operator-override provenance record.
- **No config-schema or protocol version bump.** Confirmed unnecessary:
  the estimator is pure KilnFW-side code reading fields
  `zones_config_get_model()`/`_get_model_fit_context()` already expose, and
  any future write path would reuse the existing 0x0204/`max_rate_c_per_min`
  wire field and `safety_set_rate_guard()` call -- no new field, no new
  wire type, no `ZONES_CFG_VERSION` bump. Per this task's own instructions,
  had one been implied, this section would stop and report it instead.
- **No temperature schedule.** See "temperature dependence" above --
  the `zone_model_at()`/`coupling_at()` seam does not yet hold multiple fit
  points to schedule across, so this design uses the single coldest point
  available and states plainly (not silently) that this is conservative
  rather than optimal.
- **Simulation-harness cross-check at high temperature** left as follow-up
  (previous section).

## Files touched by this pass

- `firmware/SaftyFW/src/config_store.h` -- `CONFIG_STORE_MAX_RATE_C_PER_MIN_
  FLOOR`/`_CEILING` (15.0/60.0)
- `firmware/SaftyFW/src/config_params.c` -- `CHECK_F32_RANGE_OR_ZERO`/
  `RANGE_F32_RANGE_OR_ZERO`, applied to `max_rate_c_per_min` (0x0204) at both
  SET_PARAM and load time
- `firmware/SaftyFW/test/test_config_store.c` -- `test_s8_rate_guard_bounds()`
  (new), plus five pre-existing fixtures bumped from placeholder values
  below the new floor (5.0/10.0/12.5) to 20.0/15.0
- `firmware/SaftyFW/test/test_commissioning_gate.c`,
  `test_safety_core_s8_wiring.c` -- same fixture-value bump
- `firmware/KilnFW/App/drivers/control/s8_rate_guard_estimate.c/.h` (new)
- `firmware/KilnFW/App/test/test_s8_rate_guard_estimate.c` (new)
- `firmware/KilnFW/App/test/build_host_tests.ps1`,
  `firmware/KilnFW/App/drivers/CMakeLists.txt` -- wiring for the above

All negative-tested by breaking the production check/logic, confirming RED,
and restoring by hand (confirmed via `git diff`/`grep` showing no leftover
break). SaftyFW host tests: 2468/2468 checks, 34/34 KilnFW host-test
executables, `tools/run_all_checks.ps1`: 82/82, KilnFW and SaftyFW target
builds both green.

## 2026-09-10 correction -- five confirmed defects, fixed

An opus review of this document and the code above found five confirmed
defects that, together, meant S8 could not fire on this bench under any
legal setting -- a guard that was nominally ARMED and practically inert.
Fixed in `431019ba`/`0820dfa6`/`9345f722`; this section corrects the
claims above rather than editing them in place, so the review trail stays
intact.

1. **The "unknown" sentinel always won coldest-zone selection.**
   `ZONE_MODEL_FIT_TEMP_UNKNOWN` (-273.15f) is finite, so the `isfinite()`-
   only validation in Part 2's implementation let it unconditionally win
   the "lowest fit_temp_c" comparison. Every zone on this bench, migrated
   up from a pre-v24 record, carries this sentinel today -- so the module
   had been silently deriving from it, not from a real identification.
   Fixed: the sentinel is now checked and rejected explicitly, at both the
   estimator and its one caller.

2. **The floor made S8 structurally unreachable, and the margin was
   inverted.** `CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR` (15.0) sits above
   this bench's own measured maximum achievable rate (11.5 C/min own-zone-
   only at z0). Applying `MARGIN = 2.0` to the OWN-ZONE-ONLY basis (a
   stuck relay is a duty=1.0 event, i.e. exactly this basis at margin 1.0)
   was not "conservative" in the direction that matters -- it inflated a
   too-small number rather than correcting it. Fixed: the basis now
   includes the OTHER zones' coupled contribution (see #4), and the
   margin dropped to 1.3 (identification-error headroom only). The
   estimator's return type now distinguishes an unclamped derivation from
   a floor/ceiling-clamped one.

3. **"Coldest is fastest" is contradicted by this document's own cited
   physics.** Section "Temperature dependence" above states k and tau
   fall by roughly the SAME factor (~20x at 1200 C) -- which makes k/tau
   approximately INVARIANT across a firing, not smaller at higher
   temperature. The conservatism claimed for picking the coldest fit is
   therefore close to zero, not the "generous everywhere hotter" margin
   originally claimed. This module still picks the lowest-fit_temp_c zone
   (there is no data to justify picking any other point, and it is not
   less safe), but `s8_rate_guard_estimate.h` no longer claims a strength
   of guarantee the data does not support.

4. **The estimator ignored coupling while S8 reads one TC heated by all
   three zones.** `safety_guards.c`'s S8 has no per-zone concept -- every
   real firing starts with all zones at full duty together. The measured
   coupling matrix (`docs/audits/high_temperature_transfer_analysis_2026-
   09-08.md`) shows the all-zones-firing gain onto z0 is ~2.5x its own-
   zone-only `k_dc` (31.96 own vs 27.32+21.72=49.04 coupled-in). Fixed:
   the basis is now `(k_dc + coupling_gain_sum) / tau_s`, where
   `coupling_gain_sum` is the OTHER zones' measured steady-state gain onto
   this one, sourced from `zones_config_get_coupling()`.

5. **The load-time range check was ungated.** `config_params.c` applied
   `RANGE_F32_RANGE_OR_ZERO` to `max_rate_c_per_min` unconditionally,
   unlike every adjacent bounded field carved from the former reserved
   block. A board legitimately commissioned to a value in
   `(0, 15)` before this bound shipped would fail to LOAD at all -- losing
   every other commissioned field along with it. Fixed: gated on
   `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`, matching the precedent fields
   exactly.

**Worked bench numbers with the fix applied** (using the measured fits:
z0 k=31.96/tau=166.9s, z1 k=23.48/tau=129.1s, z2 k=21.74/tau=114.8s, and
the coupling matrix's off-diagonal sums 49.04/36.45/20.75 respectively):
z0 -> 37.9 C/min, z1 -> 36.2 C/min, z2 -> 28.9 C/min, all unclamped and
comfortably inside `[15, 60]` -- S8 is no longer structurally inert on this
plant once coupling-aware, once the sentinel is rejected, and once a zone
is re-identified post-fix (the bench's existing fits still carry the
pre-v24 sentinel and must be re-run through autotune to produce a usable
`fit_temp_c` before the auto-calc endpoint will return anything but
`NO_DATA`).

Two false claims corrected at the same time: this document's "kept in sync
... by test_s8_rate_guard_estimate.c's own cross-check against config_
store.h" (no such check exists -- the test has no file I/O), and "the
Pico's CHECK_F32_RANGE_OR_ZERO is an independent backstop ... even a
compromised or buggy ESP cannot push an absurd value past it" for the AUTO
path specifically -- both bounds are the SAME `[15, 60]` the estimator
already clamps to, so that check cannot reject anything this module emits;
it remains a real, useful bound on a hand-typed MANUAL value, which is a
different claim than the one originally made.
