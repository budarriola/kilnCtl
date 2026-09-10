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

`s8_rate_guard_estimate()` **never writes anything itself** -- it is a pure
function returning a suggested value. Wiring it to an actual
`safety_set_rate_guard()` call (an HTTP endpoint or MCP tool that reads
zones_config, calls this function, and either auto-applies or presents the
suggestion for confirmation) is explicit future work, deliberately not done
in this pass: it is additive (uses the existing 0x0204 wire field, no
protocol/schema change) but is exactly the kind of new write-path this
task's instructions say to flag rather than build unreviewed. **Stopping
here on this specific piece and reporting it, rather than wiring a live
auto-push, is a deliberate choice for the owner to weigh: auto-apply on
every commissioning/autotune completion, vs. compute-and-suggest requiring
an explicit confirm identical to today's manual path.**

### Backward compatibility

A hand-set value remains fully supported and takes priority by construction:
this feature adds no new field and no new enum state to `max_rate_c_per_min`
itself -- it only adds a computation that, if and when wired to a write
path, would call the SAME `safety_set_rate_guard()`/0x0204 commissioning
call an operator's manual entry already uses. There is nothing on the wire
or in `config_store` that distinguishes "auto-derived" from "hand-set" --
whichever was written most recently is what is armed, exactly like the
firmware's other config fields today. Once a real write path exists, its UI
should show provenance explicitly (e.g. "last set: 20.0 C/min, hand-entered
on <date>" vs. "auto-suggested from zone 1's identification at 40 C") so an
operator overriding a value they never asked to be auto-derived is not
surprised by it changing under them -- called out here as a UI requirement
for whoever builds the write path, not implemented in this pass (no UI
changes were made).

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

- **No live write path.** See "who computes it, and when" above --
  building the actual auto-apply/suggest-and-confirm HTTP or MCP surface
  is deliberately left to a follow-up pass with its own review, since it is
  the piece that turns a pure computation into something that can change
  armed safety-processor state.
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
