# TC sheath lag compensation: setting and simulation

A thermocouple inside a protection tube (sheath) reads late: roughly a first-order
lag with time constant `tau`. During a ramp it reads low by about `rate * tau`, so
the controller overshoots on holds and the over-temperature guard trips late.

## Setting

| Field | Where | Range | Default |
|---|---|---|---|
| `tc_sheath_tau_s` (uint16, s) | per zone, `zones_cfg_t` v27, next to `cal_offset_c`; form key `z%u_sheath_tau` | 0..600 | 0 = off |

- 0 is byte-identical to the firmware before this setting: the estimator is not run.
- Order: the calibration offset is applied first, then the lag lead.
- Estimator (`App/drivers/control/tc_lag_comp.[ch]`, reusable API): two cascaded
  low-pass stages with `tf = clamp(tau/4, 5, 60)` s estimate the slope; the lead
  is `tau * slope`, clamped to +/-60 C. A one-tick glitch is held, not
  differentiated. The state resets on a sample gap > 5 s, on a tau change, and on
  an invalid reading.
- The control loop uses the compensated value. `GET /api/status` reports the
  applied lead as `tc_lag_corr_c`.
- Wizard step 3 and `/settings/zones` take the value; client bounds are pinned
  to the parser by `tools/PcTools/tests/test_sheath_tau_client_bounds.py`. The
  wizard says the lag is configured, never that it was verified.

## Safety treatment

Each guard uses whichever of raw and compensated is more conservative for that guard:

- guard 5 (over-temperature): `raw + max(0, lead)`. A negative lead (cooling)
  never delays a trip.
- guards 1/2 (no heat rise): the smaller rise of raw and compensated.
- guard 3 (runaway rise): the larger rise of raw and compensated.
- The Pico safety processor is unchanged: it still reads its own TC raw.

## Simulation (`App/test/test_tc_lag_sim.c`)

Ramp tracking error at 300 C/h, in C (mean absolute error after settling):

| true tau (s) | uncompensated | tau correct | tau 50% low | tau 50% high |
|---|---|---|---|---|
| 60 | 4.41 | 0.74 | 1.92 | 3.25 |
| 180 | 14.82 | 0.82 | 6.95 | 9.00 |
| 300 | 30.83 | 1.68 | 11.99 | 15.76 |

At tau 300 s, hold overshoot drops from 26.41 to 2.59 C, and cooling error drops from 19.43 to 1.42 C.
Spike input gives a 0.82 C error with 0 resets. A dropout gives 14.34 C
compensated vs 17.69 C uncompensated.

Guard 5 trip time with a stuck-on relay, in s. The true crossing is at 509 s:

| true tau (s) | raw only | tau correct | tau 0.5x | tau 1.5x |
|---|---|---|---|---|
| 60 | 569 | 507 | 538 | 476 |
| 180 | 689 | 512 | 598 | 444 |
| 300 | 797 | 532 | 651 | 439 |

An over-estimated tau trips early, which is safe. An under-estimated tau still trips earlier than raw.

## Config migration

Zones config v27 stores the tau in the old v26 padding. It is the first D1 step,
`zones_cfg_step_v26_to_v27`, and it writes tau 0. The D2 floor
`ZONES_CFG_MIGRATION_EXPIRY_FLOOR = ZONES_CFG_VERSION - 8` means v1..v18 blobs are
now refused as too old (`docs/CONFIG_MIGRATION_CHAIN.md` sec 4.2), not migrated.
