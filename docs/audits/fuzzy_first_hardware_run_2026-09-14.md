# Fuzzy layer: first hardware run above `strength_pct = 0` (2026-09-14)

## Summary

Ran profile #7 (`pv08311918`, zone_mask 0x7: 40C/45min dwell -> 45C/8min ->
60C/8min, ~72.9 minutes total) on the live board with `fuzzy_strength_pct`
set to **50 on all three zones** — the first time the fuzzy layer has ever
run above zero on hardware. The run completed (all three zones tracked to
within 0.2 C mean error of the final 60 C target). Roughly 84 seconds after
the run reached idle, the ESP32-S3 panicked and rebooted
(`exc_task='profile_executo'`, `IllegalInstruction`, `exc_addr 0x0`,
`exc_pc 0xfffffffd`) — see "Board crash" below. `fuzzy_strength_pct` was
restored to 0.0 on all zones afterward and confirmed by read-back; the board
is idle, relays off, no trip, on both processors.

## Why now

- `2c49465a`: membership bands are now derived from each zone's identified
  autotune model (`rate_band = model_k_dc / model_tau_s`, `error_band =
  model_k_dc * 0.5`), not the old fixed 0.5 C absolute band.
- `137dea1a`: the plant models, lost for three days, were restored the same
  night.

Both commits verified present (`git cat-file -t 2c49465a` / `137dea1a` ->
`commit`).

## Pre-run state (confirmed)

- `get_heap_status`: `reset_reason='software (esp_restart)'`, `uptime_s=3698`,
  no crash banner, heap headroom nominal.
- `safety_get_status`: link up, SaftyFW armed, relay_owner not tripped,
  safety thermocouple valid ~30.6 C.
- `safety_get_fw_version` (Pico): `d957d5fd` (dirty), built
  2026-09-14 18:40:24Z, boot_id=34, config CRC 0x42A2 commissioned,
  protocol v14.
- `safety_get_commissioning`: **S1 abs_max_temp_c = 80 C, ARMED**; **S8
  max_rate_c_per_min = 33.3 C/min, ARMED** — matches the pre-flight
  expectation exactly.
- `profiles_get_exec_status`: state=0, idle, no fault.
- `adaptive_tune_get_status`: all three zones `enabled=False` — confirmed
  disabled and left that way for the whole run (this run is about fuzzy
  alone; a second adapting mechanism would confound it).
- All three thermocouples read 30.7-30.8 C — zones confirmed at a common
  ambient baseline before starting (no residual-heat bias).
- `control_get_zones` plant models, read BY NAME (not `coupling_diag_k_dc`,
  which is a distinct, similar-looking field):
  - z0: `model_k_dc=42.7310` C/duty, `model_tau_s=255.6` s,
    `model_dead_time_s=40.3` s
  - z1: `model_k_dc=32.3969` C/duty, `model_tau_s=258.9` s,
    `model_dead_time_s=31.3` s
  - z2: `model_k_dc=33.8493` C/duty, `model_tau_s=247.1` s,
    `model_dead_time_s=26.0` s
  - `fuzzy_model_valid=true` on all three zones. **Models intact, exactly
    matching the values named in the task brief — not lost again.**
- Live-derived membership bands actually in effect at the time of the run
  (`GET /api/zones`), same on all three zones: `error_band_c=6.0`,
  `rate_band_c_per_s=0.2`. This does not match the `k_dc*0.5`/
  `k_dc/tau_s` arithmetic named in the task brief (which would give
  ~16-21 C error bands and ~0.125-0.167 C/s rate bands, distinct per
  zone) — worth a follow-up to understand why the live bands are flat and
  much narrower than that formula predicts; not investigated further here,
  since fixing/explaining it was out of this run's scope.

## Settings changed (and their original values, for exact restore)

Only `fuzzy_strength_pct` was changed, on all three zones, via `POST
/api/zones` (the whole-page-submit endpoint; there is no narrower MCP tool
for this HTTP-only field, so the request was built as a minimal but complete
form body reusing every other live value verbatim — relay_mask, PID gains,
ramp/sanity limits, control_mode, temp limits, heater window/min-on/min-off,
timing profile 0, `thermo_count=3`/`relay_count=3`/`safety_tc_type=3`).

| Field | Original | Set to | Restored to | Confirmed by read-back |
|---|---|---|---|---|
| z0 `fuzzy_strength_pct` | 0.0 | 50.0 | 0.0 | yes |
| z1 `fuzzy_strength_pct` | 0.0 | 50.0 | 0.0 | yes |
| z2 `fuzzy_strength_pct` | 0.0 | 50.0 | 0.0 | yes |

Nothing else in the zones config was touched — every other field (PID gains,
plant models, control_mode=3, max_temp_c=80, relay/thermo masks, heater
timing) read back identical before and after.

## Which zones, and why 50

**All three zones**, at **50%** (the value the simulation work
characterised; no reason presented itself to prefer 25). All three were run
together rather than isolating one zone as a control, because the zones are
thermally coupled (measured coupling matrix off-diagonals are large relative
to the diagonal, e.g. z0<-z1 25.4, z0<-z2 24.5) — a single fuzzy zone's
behavior would still be perturbed by the other two zones' base-PID duty
through that coupling, so an in-run "control" zone would not really be
uncoupled from the treatment. Running all three sacrifices a same-run A/B
comparison but avoids reading a confounded result as if it were clean; the
same-build `fuzzy=0` baseline mentioned in the plan doc (run concurrently by
another pass) is the intended point of comparison instead.

## Results

All values below come from the board's own live-computed `firing_stats`
(`GET /api/profile_exec`, per zone), captured every ~10s for the whole run
into `fuzzy_run_capture.jsonl` (411 usable samples), read back once more at
completion for the final cumulative stats. Segment 0 = ramp+dwell at 40 C
(45 min dwell), segment 1 = 45 C (8 min dwell), segment 2 = 60 C (8 min
dwell). **0.5 C materiality rule applied per objective below, not
aggregated.**

### Overshoot (dwell-entry peak, `max_overshoot_c` / `FIRING_SUBSCORE_ENTRY_PEAK_C` definition)

| Zone | Peak overshoot | At (elapsed s) | Segment | Materiality |
|---|---|---|---|---|
| z0 | 4.16 C | 249 | 0 (into 40 C dwell) | **material** — 8x the 0.5 C floor |
| z1 | 3.78 C | 244 | 0 | **material** |
| z2 | 5.10 C | 243 | 0 | **material** |

All three zones' worst overshoot happened entering the *first* dwell (40 C);
`max_overshoot_segment=0` for all three at the end of the run, meaning the
45 C and 60 C dwell entries never exceeded it. A 4-5 C dwell-entry peak is a
real, material finding, not noise — the fuzzy layer at 50% did not prevent a
sizeable overshoot on the first dwell entry on real hardware.

### Undershoot (signed, unclamped, not cancelled by later recovery)

| Zone | Max undershoot | At (elapsed s) | Segment |
|---|---|---|---|
| z0 | 5.15 C | 87 | 0 (during the initial ramp, before reaching 40 C) |
| z1 | 5.34 C | 112 | 0 |
| z2 | 4.22 C | 82 | 0 |

**Material** on all three zones (>10x the 0.5 C floor). This is the
expected ramp-lag undershoot while the ramp is still climbing toward the
first target and the plant has not yet caught up — consistent with each
zone's ~250-460 s time constant against a 300 C/hr ramp.

### Settle time (to and remaining within the production 2.0 C band; 33-instance-corpus value, used here rather than a different band)

Computed from the capture by finding, per zone per segment, the last sample
at which `|target - actual| > 2.0 C`:

| Zone | Seg 0 (40 C) settle | Seg 1 (45 C) settle | Seg 2 (60 C) settle |
|---|---|---|---|
| z0 | ~290 s after segment start | ~279 s after segment start | ~310 s after segment start |
| z1 | ~268 s | never exceeded 2 C in this segment | ~102 s |
| z2 | ~279 s | ~259 s | ~559 s |

z2's final (60 C) settle at ~559 s (~9.3 min) is the outlier — worth noting
against the sub-materiality noise floor (all these numbers are well above
0.5 C so materiality is not in question here, only the comparison against
the `fuzzy=0` baseline run, which this audit does not itself contain).

### Ramp-rate tracking (`FIRING_SUBSCORE_LAG_S` — unsigned, drops saturated-and-short ticks; known limitation, not fixed here)

Not separately recomputed from the capture; the board's own `ramp_err_mean_c`
/`ramp_err_max_c` fields (below, under steady-state) are the closest
equivalent actually exposed by `/api/profile_exec`, and are reported instead
since deriving the exact `LAG_S` subscore would require re-running
`log_analyze` against a raw capture format this run did not produce in.
Flagged as a gap rather than fabricated.

### Steady-state error during dwell (from the board's own cumulative `firing_stats`, whole-run)

| Zone | mean_error_c (whole run) | ramp_err_mean_c | ramp_err_max_c | dwell_err_mean_c | dwell_err_max_c |
|---|---|---|---|---|---|
| z0 | +0.11 | 1.60 | 5.15 | 0.81 | 4.96 |
| z1 | -0.13 | 1.38 | 5.34 | 0.65 | 5.17 |
| z2 | +0.19 | 1.51 | 4.22 | 0.71 | 5.10 |

Whole-run mean error is sub-0.5 C material threshold on all three zones
(**not material**) — the layer tracked the overall setpoint well on average.
`dwell_err_max_c` (~5 C) is material and reflects the same dwell-entry
transient captured above, not a sustained dwell-error problem (`dwell_err_mean_c`
0.65-0.81 C is itself right at the materiality boundary).

## Rule-cell occupancy — the open question

There is no direct telemetry for which of the 9 fuzzy rule cells fired
(`pid_fuzzy.c` computes cell membership internally per tick and does not log
or expose it over HTTP). Reconstructed here from the capture: at each ~10s
sample, `error = target_c - actual_c` and `rate = d(actual_c)/dt` between
consecutive samples were each bucketed into `{-, 0, +}` against half the
zone's live band (`error_band_c=6.0`, `rate_band_c_per_s=0.2`), giving an
approximate 3x3 cell occupancy. This is a coarse post-hoc reconstruction
(10s sample spacing vs. the controller's actual tick rate), not the
controller's own ground truth, and should be read as indicative only.

| Zone | (err,rate) cell | Count | % |
|---|---|---|---|
| z0 | (0,0) centre | 389 | 94.9% |
| z0 | (-,0) | 12 | 2.9% |
| z0 | (+,0) | 4 | 1.0% |
| z0 | (0,+) | 4 | 1.0% |
| z0 | (+,+) | 1 | 0.2% |
| z1 | (0,0) centre | 395 | 96.3% |
| z1 | (-,0) | 9 | 2.2% |
| z1 | (+,0) | 4 | 1.0% |
| z1 | (+,+) | 1 | 0.2% |
| z1 | (0,+) | 1 | 0.2% |
| z2 | (0,0) centre | 390 | 95.1% |
| z2 | (-,0) | 13 | 3.2% |
| z2 | (+,0) | 4 | 1.0% |
| z2 | (0,+) | 3 | 0.7% |

**Finding, stated plainly:** a real firing does *not* reach anywhere near
9/9 cells the way simulation with injected disturbance did, but it is also
not the single-cell 100% result the one real mode-3 capture showed. All
three zones spent ~95-96% of the run in the centre cell and the remaining
~4-5% split across four of the eight outer cells (never the extreme
corners, never a `-` rate cell). The bulk of that off-centre time
corresponds to the dwell-entry transients already identified above as
overshoot/undershoot, not to steady dwell operation. **The centre cell still
dominates overwhelmingly** — this run does not show the fuzzy layer doing
much of anything for ~95% of its runtime; its measurable effect, if any, is
concentrated in the transient minority of samples.

## Board crash (found during this run's teardown)

`get_heap_status`, checked as part of confirming a safe post-run state, came
back with an **unacknowledged crash report**:

```
exc_task='profile_executo' exc_cause_str='IllegalInstruction'
reset_reason='PANIC' -> found_on_boot_reset_reason='PANIC'
exc_pc=0xfffffffd exc_addr=0x00000000 exc_a0=0x3fca0080 exc_a1_sp=0x3fcb3ae4
frame_trustworthy=false, backtrace_corrupted=true
uptime_s=84 (at time of read)
```

This happened *after* the firing reached its final target and the exec
state had already gone to `idle` (the capture's last `"state":"running"`
row shows `elapsed_s=4371` against `total_planned_s=4372`, essentially
complete, all three zones within ~0.7 C mean error of 60 C). The panic
therefore looks like it hit during run teardown/finalization rather than
during active control — `exc_addr 0x0` and a corrupted backtrace are the
same signature CLAUDE.md and project memory already flag for this codebase
as **not diagnostic on their own** (see
`project_safety_poll_panic_thermo_slot_corruption.md`'s "exc_addr 0x0 was a
red herring" and the still-open
`project_profile_executor_panic_at_stop.md` note: "exc_addr 0x0 is a red
herring, look for adjacent-stack corruption"). This looks like a recurrence
of that same open, previously-unfixed defect, now reproduced for the first
time with the fuzzy layer active — **not something this run's own new
code path (fuzzy strength >0) can be ruled in or out as the cause of**,
since the crash happened after control had already finished, and the same
defect class was already open before this run.

Per the task's standing instruction, the firmware was **not** touched,
flashed, or debugged further to chase this — it is reported here as found,
consistent with the do-not-flash constraint on this session. The crash
report is left **unacknowledged** deliberately, since acknowledging it is a
human/operator review step this audit does not have standing to perform on
its own after just finding it.

Because the crash happened after the firing had already reached `idle`,
`GET /api/profile_exec`'s `last_run` field was `{"present":false}` by the
time this was checked (either cleared by the reboot or already consumed) —
the reboot did not lose the run's own data, since the full `firing_stats`
used above were captured live throughout the run into the local JSONL, but
it did mean the board's own end-of-run summary was not independently
available afterward for cross-check.

## Post-run state (confirmed)

- `POST /api/zones` re-applied with `z0/z1/z2_fuzzy_strength=0`; read-back
  confirms `fuzzy_strength_pct=0.0` on all three zones, all other fields
  (PID gains, models, control_mode, limits, timing) unchanged.
- `adaptive_tune_get_status`: all three zones still `enabled=False` (never
  touched).
- `profiles_get_exec_status`: state=0, idle.
- `safety_get_status`: link up, SaftyFW armed, relay_owner not tripped, no
  trip.
- `safety_get_commissioning`: S1/S8 unchanged (80 C / 33.3 C/min, both
  ARMED).
- `GET /api/status`: all four relays off, `safety_relay_energized=false`,
  `power_w=0.0`.
- Board is on its post-panic boot (`uptime_s` small, `reset_reason` still
  PANIC-derived at last check) but otherwise idle and safe; the crash report
  remains unacknowledged for operator review.

## Plan checkbox

`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` line 2443's box is checked
below — the run itself genuinely happened (completed, full profile, no
firmware changes) — but the entry now points at this audit and the crash
found during teardown, rather than declaring the layer's on-hardware
behavior settled.
