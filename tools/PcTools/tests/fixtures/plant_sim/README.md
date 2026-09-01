# plant_sim calibration fixtures

Five full `/api/profile_exec` poll captures of profile 7 firings on real
hardware, one per firmware build the calibration in
`../../src/kilnctrl/plant_sim.py` had to reproduce:

| file | build | `climb_mode` | `integral_floor` |
|---|---|---|---|
| `baseline.jsonl` | pre-fix | `uncoupled` | `ff_u` |
| `after.jsonl` | coupled ff shipped | `coupled` | `ff_u` |
| `ifix.jsonl` | superseded integral fix | `coupled` | `ff_u` |
| `holdfix_clean.jsonl` | shipped integral fix | `coupled` | `ff_hold` |
| `final.jsonl` | end of calibration session | `coupled` | `ff_hold` |

Checked in whole (~2.9 MB total), not excerpted, because they are the
evidence the simulator is calibrated, not incidental test data: the
regression test in `../test_plant_sim.py` pins the simulator's residual
against `after.jsonl` specifically, and the calibration report the
simulator was built from (see `plant_sim.py`'s module docstring) measured
its 1.45 °C aggregate RMS across all 60 windows in all five files.
Truncating them would make that number unverifiable from the repo alone.
2.9 MB was judged acceptable in a repo that already carries
`tests/fixtures/track3zone_full.jsonl` (~260 KB) for the same reason.

`holdfix_clean.jsonl` is the de-contaminated version of a capture that,
in its original form, mixed in three runs from a stray poller — use this
file, not a raw re-export of that firing.

These captures are quantized to the thermocouple's real resolution at the
executor's real 10 s poll cadence. Do not resample, smooth, or otherwise
"clean up" them for a new test — synthetic, idealized input is exactly what
let the pre-calibration simulator's hardcoded-rate bug hide for as long as
it did (see `plant_sim.py`'s module docstring, "THE BUG THIS REPLACES").
