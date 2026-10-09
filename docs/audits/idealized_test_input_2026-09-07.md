# Idealized test-input sweep — 2026-09-07

Scope: KilnFW and SaftyFW host tests, looking for the "idealized synthetic
input hides a real branch" shape (see MEMORY.md
`project_idealized_test_input_bug_class.md`). Effort was capped; this is not
an exhaustive pass, it targets the sensor-facing tests most likely to carry
this defect (CT current, MAX31856 temperature, ADC counts, tick timing).

| Input | Current form | Realistic form | Branch hidden | Fixed |
|---|---|---|---|---|
| `current_presence_is_flowing()` counts delta (`test_current_presence_policy.c`) | Round, arbitrary deltas (10, 20, 100, 200, exactly-at-margin) with no tie to measured board noise | Real capture from `docs/CURRENT_SENSE.md` "Measured noise floor" (2026-09-06, channel 3): mean 66.89 counts, std 4.678 counts, observed range 60-76 counts over 60s idle | The uncommissioned fallback margin (`CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS = 25`) had never been checked against the actual measured noise floor it exists to reject — only against invented round numbers that were chosen to obviously pass | **Yes** — added `test_uncommissioned_real_measured_noise_does_not_false_trigger()` using the measured mean/std/range directly. Result: real worst-case observed excursion is 9 counts and 3σ is ~14 counts, both well clear of the 25-count margin. Test **passes** — this confirms the existing margin is safe against real noise, it does not surface a defect, but it closes a real gap (nothing previously exercised the policy against a measured noise figure at all). |
| `max31856_decode_cj`/`_decode_tc()` register vectors (`test_max31856_decode.c`) | Exact round degC outputs (12.0, -1.0, 1000.0, 0.015625) | N/A — reviewed and judged **not** idealized in the harmful sense: this is a bit-exact fixed-point register decode (datasheet LSB weights), not a sensor-noise-bearing quantity. Exact-equality is the correct test style here; float noise/quantization belongs at a higher layer (the guard/PID consumer of the decoded value), not in the decoder itself. | None — the decode math has no noise-rejection or hysteresis branch to hide | No (reviewed, not a defect) |
| `ct_amps_cal_apply()` gain/offset vectors (`test_ct_amps_cal.c`) | Round floats (2.0, 0.5, 3.0, 99.0) | Reviewed: this is a pure linear calibration transform (`gain*raw+offset`, clamped ≥0) with no quantization or noise-dependent branch — round numbers exercise the linear-math and clamp paths exactly as well as noisy ones would. Not a defect. | None found | No (reviewed, not a defect) |
| `test_guard_nuisance.c` (S3/S4/S9/S10) thermocouple deltas (150C, 250C off, exact 900/750C) | Exact round degC boundary values | Reviewed: these are threshold-crossing tests where the guard margin (150-250C) is 3-4 orders of magnitude larger than any MAX31856 quantization (0.0078125C) or realistic noise band, so quantization cannot flip these decisions. Round numbers do not hide a branch here. | None found at these margins | No (reviewed, not a defect) |

## Follow-up not done (out of this pass's scope)

- KilnFW's PID/dead-time/ramp-fit host tests (`test_closed_loop.c`,
  `test_adaptive_tune_model.c`, etc.) were not swept for idealized tick
  intervals (`dt` exactly 1.0s, perfectly monotonic timestamps) — flagged as
  the next place to look for this shape given `docs/PROJECT_STATUS.md`'s
  `project_ramp_fit_measures_ramp_rate` finding already documents a related
  closed-loop-fit artifact.
- No raw ADC-counts noise capture exists for channels 1/2 (unfitted) beyond
  the 2026-09-06 measurement already in `docs/CURRENT_SENSE.md`; this sweep
  did not attempt a new hardware capture (out of scope, no board access).

## Net result

One idealized-input gap found and fixed: `test_current_presence_policy.c`
had never been checked against the actual measured CT noise it is meant to
reject, only against invented round numbers. The fix uses the real
2026-09-06 measurement from `docs/CURRENT_SENSE.md` and the test **passes**,
confirming (not merely assuming) the 25-count fallback margin holds against
real board noise. No production-code defect was surfaced by this pass; the
other three candidates inspected were judged legitimately exact rather than
idealized.
