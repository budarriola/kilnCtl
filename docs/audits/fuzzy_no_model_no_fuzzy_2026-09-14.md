# A no-model zone now runs plain PID, not the invented default bands

2026-09-14. Follow-up to `docs/audits/fuzzy_dimensionless_bands_2026-09-13.md`
(commit `2c49465a`), which derived the fuzzy layer's membership bands from
each zone's identified FOPDT model (`rate_band = model_k_dc / model_tau_s`,
`error_band = model_k_dc * 0.5`) but fell back to shipped absolute constants
(`ERROR_BAND_C_DEFAULT = 20.0f`, `RATE_BAND_C_PER_S_DEFAULT = 0.5f`,
`firmware/KilnFW/App/drivers/control/pid_fuzzy.c`) for a zone with no
identified model. Those constants are, by that file's own header comment,
desk reasoning about "a mid-size kiln," never measured on any plant -- so a
never-autotuned zone ran the fuzzy layer on invented numbers, the same defect
the 2026-09-13 pass existed to remove for every OTHER zone.

## The owner's principle

Stated 2026-09-14: fuzzy constants, like PID gains, should be derived per
kiln, never shipped. Bench-convenient values are fine on the bench precisely
because they never ship. A shipped absolute default that DOES get compiled
into every board and silently used until autotune runs is a different thing
entirely.

## Evaluation

"No identified model" is not ambiguous in this codebase. `zones_config_
accessors.h`'s own documented convention (the comment on `zones_config_get_
model()`, echoed by `pid_fuzzy_derive_bands()`'s own header comment in
`pid_fuzzy.c`/`.h`): `model_k_dc`/`model_tau_s`/`model_dead_time_s` all
reading 0 means "never autotuned," and a caller must treat that as "cannot
answer," not "the answer is zero." `zone_model_at()`
(`profile_executor_feedforward.c`'s own passthrough seam, reused by this
call site) and `pid_fuzzy_derive_bands()` both already implement exactly
this check: `model_k_dc > 0.0f && model_tau_s > 0.0f`, both finite.

What the system did before this pass, for such a zone: `resolve_fuzzy_bands()`
(`profile_executor_pid_tick.c`) received `false` from `pid_fuzzy_derive_
bands()` and fell through to `zones_config_get_error_band_c()`/`_rate_band_
c_per_s()` -- the OPERATOR-configurable per-zone bands, which themselves
resolve a 0 sentinel to the same absolute `ZONE_ERROR_BAND_C_DEFAULT`/`ZONE_
RATE_BAND_C_PER_S_DEFAULT` (20.0/0.5, `zones_config_json.h`) pid_fuzzy.c's
own constants mirror. Either way -- operator never touched the field, or
touched it and left it at 0 -- a zone that has never been autotuned still ran
the full fuzzy rule table on desk-reasoning bands whenever `fuzzy_strength_
pct` was configured nonzero.

**Is disabling fuzzy for a no-model zone strictly safer, or does it remove
something an operator depends on?**

Strictly safer, and it removes nothing an operator can currently be depending
on, for two independent reasons:

1. **Fuzzy is documented as pure enhancement, never a requirement.**
   `pid_fuzzy.h`'s own header comment: "a *second selectable mode*, not a
   replacement for classic PID -- selecting it does not change the base
   gains, it only lets them drift within a caller-bounded range." At
   `strength_pct=0` it is bit-identical to classic PID by contract (already
   asserted, `test_fuzzy_prepare_gains_zero_strength_is_base_gains_bit_
   exact()`). Forcing `strength_pct` to that same, already-safe value for a
   no-model zone is not introducing a new mode -- it is refusing to leave the
   existing safest mode when the layer has nothing measured to work from.

2. **This mirrors the standing project posture toward PID gains themselves.**
   Nothing in this codebase invents a substitute Kp/Ki/Kd for an
   un-autotuned zone; the system expects Autotune to supply them, and a zone
   without them simply is not usefully controllable yet -- that is treated as
   an expected state, not patched over with guessed numbers. Fuzzy gain
   scheduling sits on top of that same foundation (it nudges Autotune's
   gains, per the header comment above); it is inconsistent to let the layer
   invent a scale for itself while refusing to invent gains for the loop
   underneath it.

No counter-argument surfaced that fuzzy on invented bands is doing useful
work today: `fuzzy_gain_mirror_drift_check.py`'s own docstring already notes
this call site's whole band-fallback existed only "for a never-autotuned
zone," i.e. the exact case this pass closes, and the live bench board's own
state (below) shows nothing currently exercises it. **Conclusion: the
proposal holds. Implemented as described.**

## What changed

`firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c`:

- `resolve_fuzzy_bands()` now returns the `bool` `pid_fuzzy_derive_bands()`
  gives it, instead of swallowing a `false` and substituting the config/
  firmware-default bands itself. It no longer calls `zones_config_get_error_
  band_c()`/`_rate_band_c_per_s()` at all.
- `pid_fuzzy_prepare_gains()` reads that bool and, when false, forces
  `strength_pct = 0` before calling `pid_fuzzy_adjust()` -- routing a
  no-model zone through the EXISTING, already-tested `strength_pct == 0`
  bit-exact-base-gains short-circuit in `pid_fuzzy.c`, rather than adding a
  second way to be inert. `log_fuzzy_disabled_no_model_once()` logs this once
  per zone per boot (not per tick -- this is a control-loop path).
- Both statements (`if (!bands_from_model) strength_pct = 0;` / `if
  (!bands_from_model) log_fuzzy_disabled_no_model_once(zi);`) are single-line
  `if` bodies with no braces, so `fuzzy_gain_mirror_drift_check.py`'s
  statement splitter (which assumes this fragment has no nested compound
  statements) still works unmodified; both statements are folded away in
  that check's normalization since `test_closed_loop.c`'s mirror has no
  model concept at all -- the same treatment the `strength_pct`
  config-derivation block already gets.

`firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py`: updated
`PROD_ONLY_STMT_RES` and the module docstring for the new statement shapes.
Re-run directly: `FUZZY-GAIN MIRROR DRIFT CHECK: OK (8 normalized lines
match...)`.

`pid_fuzzy.c`/`.h`: **unchanged**. `pid_fuzzy_derive_bands()`'s contract
(false + the two absolute constants on an invalid model) stands exactly as
documented -- see "fate of the two constants" below for why.

## Making it visible

An operator whose kiln has never been autotuned can now tell fuzzy is
inactive two ways:

1. **Log line**, once per zone per boot: `zone %u: fuzzy layer disabled,
   running plain PID -- no identified plant model yet (run Autotune to
   enable fuzzy gain scheduling for this zone) (further occurrences this
   boot are suppressed)`.
2. **Existing status surfaces, unchanged wiring**: `GET /api/zones` already
   reports each zone's `model_k_dc`/`model_tau_s` (0 means never identified)
   AND its own configured `fuzzy_strength_pct` -- an operator (or the
   dashboard) can already derive "fuzzy is configured but has no model to
   run on" by reading both fields together, with no new field needed.

**No new field was added anywhere.** `GET /api/zones` was checked first per
this task's own instruction and found to have only 161 bytes of headroom in
its 7360-byte cap (`docs/audits/zones_json_headroom_plan_2026-09-14.md`) --
not spent here, and not needed here, since the two existing fields already
let a reader derive the "disabled, no model" state without an explicit
boolean. `GET /api/adaptive_tune` and the MCP status surface were considered
as alternatives; neither was extended, because the derivable-from-existing-
fields answer above is free. If an owner later wants an explicit "fuzzy_
active" boolean instead of requiring the reader to cross-reference two
fields, that is a small, separable follow-up -- not required by this pass,
and this doc names the gap rather than forcing a field in under headroom
pressure.

## Fate of the two constants (`ERROR_BAND_C_DEFAULT`, `RATE_BAND_C_PER_S_DEFAULT`)

**Recommendation: keep them exactly as they are, in `pid_fuzzy.c`, for two
narrower, still-legitimate roles distinct from the no-model band-selection
role this pass removes:**

1. **`pid_fuzzy_adjust()`'s own internal defense-in-depth.** Independent of
   model status, that function still falls back to these two constants if a
   caller ever hands it a non-finite or non-positive `error_band_c`/`rate_
   band_c_per_s` (its own comment: "a bad value reaching this function --
   however it got here -- falls back to the documented firmware default
   rather than corrupting the membership math"). This is a last-line-of-
   defense against a caller bug, not a policy default for an un-autotuned
   zone, and after this pass it is provably unreachable from the production
   no-model path (which never even reaches `pid_fuzzy_adjust()`'s band math,
   since `strength_pct=0` short-circuits before it) -- but it remains the
   right thing for `pid_fuzzy_adjust()` to do if called directly (as the host
   tests do) with a bad band value.
2. **`pid_fuzzy_derive_bands()`'s own documented false-return contract and
   test fixtures.** `test_pid_fuzzy.c` exercises this function directly
   (invalid model -> false + exactly 20.0/0.5) as a unit-level property of
   `pid_fuzzy.c` in isolation, independent of whether any production call
   site still uses the false-branch value. Bench/test values are legitimate
   test fixtures precisely because -- per the owner's own framing -- they
   never ship as a live control-path default; that is now enforced STRUCTURALLY
   by this pass (the value is computed but never consulted by the production
   caller for a no-model zone), not merely by convention.

Deleting the constants was considered and rejected: it would remove
`pid_fuzzy_adjust()`'s legitimate internal defensive fallback (a different,
still-needed role), and would require `pid_fuzzy_derive_bands()` to either
change its return contract (writes nothing on `false`, forcing every caller,
including host tests that call it directly, to handle uninitialized outputs)
or invent a different sentinel -- more churn than the constants' continued,
narrower existence costs. Keeping them but restricting to host-tests-only
(via `#ifdef`) was also considered and rejected: `pid_fuzzy_adjust()`'s
defensive fallback is legitimately needed in the shipped binary, not just in
tests.

## Tests

`firmware/KilnFW/App/test/test_profile_executor_prestart.c`:

- `zones_config_get_model()`'s stub is now settable
  (`g_stub_model_k_dc[]`/`g_stub_model_tau_s[]`), defaulting to the all-zero
  "never autotuned" sentinel every pre-existing test in this file implicitly
  assumed -- no behaviour change for any test that does not touch it.
- **`test_fuzzy_prepare_gains_no_model_forces_plain_pid_bit_exact()`** (new):
  the central regression guard. No model, `strength_pct=100`, a large
  POS/STEADY error/rate cell that WOULD move gains hard with a model present
  -- asserts `out.kp/ki/kd` are exactly `base_kp/ki/kd`. **This is the test
  that fails if someone reintroduces a shipped numeric default into this call
  site's band-resolution path** -- verified directly (see Negative test
  below).
- **`test_fuzzy_prepare_gains_with_model_uses_derived_bands_not_default()`**
  (new): a zone WITH a model (the live bench z0 fit, k_dc=42.731,
  tau_s=255.6) gets bands from `pid_fuzzy_derive_bands()`, confirmed
  numerically distinct from the 20.0/0.5 firmware default, and
  `pid_fuzzy_prepare_gains()`'s output matches a direct `pid_fuzzy_adjust()`
  call using those derived bands exactly.
- `test_fuzzy_prepare_gains_matches_pid_fuzzy_adjust_directly()` and
  `test_fuzzy_prepare_gains_uses_zone_commanded_setpoint_when_capped()`
  (pre-existing, both exercise a nonzero `strength_pct`) now give zone 0 a
  model so the fuzzy layer actually still runs in the case they mean to
  test, with their "expect" `pid_fuzzy_adjust()` calls updated to use the
  model-derived bands instead of the hardcoded 20.0/0.5 literals.
- `test_hold_membership_change_resets_fuzzy_prev_effective_ki()`
  (pre-existing, needs `fuzzy_prev_effective_ki` to actually move away from
  base Ki to be discriminating) likewise now gives zone 0 a model.
- All model-stub state is explicitly reset to "no model" at the end of every
  test that sets it, matching the file's existing discipline for
  `g_stub_approach_rate_cap_c_per_hr` (file-scope static, not cleared by the
  generic reset helpers).

## Negative test (by hand)

Broke `profile_executor_pid_tick.c` line 435 from
`if (!bands_from_model) strength_pct = 0;` to `if (false) strength_pct = 0;`
(simulating a silent revert to the old fallback behaviour where a no-model
zone kept running fuzzy). Rebuilt host tests:

```
FAIL .../test_profile_executor_prestart.c:2865: kp must be exactly base_kp
  -- no model means no fuzzy adjustment, regardless of strength_pct
```
Exit code 1 (was 0). Confirmed this is the ONLY new failure (checked full
output; every other failing-sounding line was pre-existing descriptive test
text containing the word "FAIL", not an actual `FAIL` result line).

Restored the line by hand to `if (!bands_from_model) strength_pct = 0;`,
confirmed via `git diff` that the file matches its pre-breakage state, then
deleted `firmware/KilnFW/App/test/build/` entirely and ran a full clean
rebuild: **38/38 host test executables built and passed**, exit code 0.

## Why this is safe to land now

The live bench board has all three zones autotuned (non-zero `model_k_dc`:
z0=42.731, z1=32.397, z2=33.849) and `fuzzy_strength_pct=0.0` on every zone
(confirmed via `control_get_zones`
over the kilnctrl MCP). Every zone already has a model, so `bands_from_model`
is `true` for all three today regardless of this change, and `strength_pct`
was already 0 regardless of the band source -- **this change cannot alter
live behaviour on this board today.** It only changes what happens the next
time a zone's model is deliberately cleared (e.g. after a load/element change
invalidates the old fit, `zones_config_set_model(zi, 0, 0, 0)` -- documented
as legal in that function's own header comment) or on a fresh, never-
autotuned board.

## Check tally

- `fuzzy_gain_mirror_drift_check.py`: OK (8 normalized lines match).
- KilnFW host tests: 38/38 executables built and passed, clean rebuild
  (build dir deleted first).
- `tools/run_all_checks.ps1`: see this pass's commit message / session notes
  for the run this doc accompanies.
