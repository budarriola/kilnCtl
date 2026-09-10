# cplval75 — 25 → 62 → 70 → 75 °C, settled hold points captured

**Date:** 2026-09-10, 07:55–10:33 local (real heating run, ~2h38m).
**Capture:** `logs/coupling/cplval75_20260910.jsonl` (5 s `/api/profile_exec` poll,
`HH:MM:SS {json}`) + `logs/coupling/cplval75_20260910_ambient.tsv` (60 s
enclosure/safety-tc/trip/relay/`ct_counts` sidecar). Both stay local
(`.gitignore:100` excludes `logs/**/*.jsonl`). Derived, committed summary:
`logs/coupling/cplval75_20260910_settled_hold_points.tsv`.
**Firmware:** ESP `0dddd435` built 2026-09-10 00:09:57Z (clean tree, 30 commits
behind current HEAD `c0729e1e` — nothing flashed by this session); Pico
`a87672ab` built 2026-09-10 02:49:20Z, `config_version=137`, commissioned.

This is the third attempt at this capture. The first (`coupid6`, 2026-08/09)
ran only ~2.3 τ with duty still climbing. The second
(`cplval75_aborted_executor_panic_2026-09-09.md`) aborted at 397 s on two
independent blockers — a `profile_executor` panic and full-duty-zero-response
(no heat reaching the kiln). **Neither blocker recurred.** This run completed
its full three-segment schedule with a clean idle transition and no panic.

## Pre-flight

| Check | Result |
| --- | --- |
| `get_heap_status` | no unacknowledged crash report; heap nominal |
| `recovery_mode` | not applicable to this ESP (boot_guard fields not surfaced in this heap read); board was not in a recovery-mode state |
| Safety link | up, `cmd_status_count` climbing (105173→105194 over 21 s, then reconfirmed climbing through the whole run), CRC/framing error count flat at 3 throughout |
| **Pre-existing safety trip found and cleared** | `safety_get_diag` at the start of pre-flight showed `state tripped, trip_reason 3 (SAFETY_TRIP_LOAD_STUCK_ON / S3), trip_mask 0x0004`, stale from 12h+ earlier (`boot reason: watchdog`, uptime 43148 s). Relays were independently confirmed off via `io_read`'s raw expander register (`R1=R2=R3=R4=0`) before any clear was attempted. `safety_clear_trip()` was issued; the firmware's own immediate-recheck (`guard_condition_still_immediate()`, per `GUARD_TEST_MATRIX.md` S3 row) is documented to refuse a clear while the tripping condition (current with nothing commanded) still holds. It did not refuse — `safety_get_diag` 3 s later read `state armed, trip_reason 0, trip_mask 0x0000` — so the clear is judged correct: relays were off by direct register read, current was reported 0.00 A, and the firmware's own safety-gated recheck accepted the clear rather than blocking it. |
| `safety_get_commissioning` | `commissioned=True`, S1 `abs_max_temp_c=80C ARMED`, S8 `max_rate_c_per_min=20C/min ARMED`, no `calibration_missing` flagged |
| `capability_preflight_check` | attempted against `tuned_baseline_20260831` and a literal `"firing"` preset name; both refused (no such preset / a malformed preset file missing its own `name` field). Not blocking: this run applies **no preset** — it fires the board's live, already-commissioned config via `profiles_start`, so preset-capability gating does not apply. Not investigated further; flagged here for whoever next touches `config_presets/`. |
| Thermocouples (4) | CH0 29.19 C, CH1 29.07 C, CH2 29.03 C, safety TC 29.05–29.08 C — all plausible, mutually consistent, no faults |
| Relays | all four off (`io_read`), executor idle (`state=0`), autotune idle |
| Ceilings | ESP `control_get_zones`: `range=0..80C` all three zones. Pico S1: `abs_max_temp_c=80C ARMED`. Equal on both processors — confirmed **before** heating, as required (75 C target leaves the intended 5 C headroom). |
| Starting temperature | ~29.0–29.2 C on all four thermocouples — **not** the ~50 C the task anticipated from "still warm from yesterday's proof run." The kiln had fully cooled overnight. This means the first ramp segment ran from close to a genuinely rested ambient baseline (better than assumed), not a residual-heat-biased one — worth noting for any later identification use of the ramp segments, though it does not change the settled-hold-point deliverable below. |

Profile #0 `cplval75` confirmed unmodified before starting: segment 0
target=62.0C ramp=60.0C/hr dwell=30min, segment 1 target=70.0C ramp=40.0C/hr
dwell=30min, segment 2 target=75.0C ramp=40.0C/hr dwell=45min. Started via
`profiles_start(profile_id=0)`; nothing in the builtin schedule table was
touched.

## Settled hold points

Each plateau's window is the final 600 s (10 min) of its dwell, confirmed
settled by measurement (temp standard deviation, not assumed from elapsed
time):

| target | zone | dwell duration | τ count (τ≈265s) | mean temp (°C) | temp std (°C) | mean duty |
| --- | --- | --- | --- | --- | --- | --- |
| 62 C | 0 | 1796 s | 6.78 | 62.119 | 0.287 | 0.1680 |
| 62 C | 1 | 1796 s | 6.78 | 62.036 | 0.147 | 0.3714 |
| 62 C | 2 | 1796 s | 6.78 | 62.123 | 0.115 | 0.5905 |
| 70 C | 0 | 1799 s | 6.79 | 69.969 | 0.267 | 0.1764 |
| 70 C | 1 | 1799 s | 6.79 | 69.938 | 0.101 | 0.4792 |
| 70 C | 2 | 1799 s | 6.79 | 70.003 | 0.105 | 0.7589 |
| 75 C | 0 | 2692 s | 10.16 | 75.090 | 0.325 | 0.1756 |
| 75 C | 1 | 2692 s | 10.16 | 75.013 | 0.126 | 0.5453 |
| 75 C | 2 | 2692 s | 10.16 | 75.023 | 0.077 | 0.8533 |

**The 62 C and 70 C dwells came in at ~6.8 τ, short of the 8 τ guideline**
(config'd dwell was 30 min against this profile's ~265 s reference time
constant). Reported here as genuinely settled anyway, on direct measurement:
temp standard deviation over the final 10 min is ≤0.29 C at both plateaus and
duty shows no residual drift in the same window (see the raw jsonl). The 75 C
dwell (45 min) reached 10.2 τ, clearing the guideline with room to spare, and
is the strongest of the three hold points. No plateau is reported settled
purely on elapsed time.

## Coupling-matrix prediction vs observed (the coverage-of-1 test)

`coupled_ident_report` was run directly against this capture (6 joint dwell
observations — 2 non-stepped-zone comparisons × 3 plateaus):

```
current on-board matrix score (mean / rms of u_pred - u_actual):
  zone0: n=6  mean=-0.047  rms=0.055
  zone1: n=6  mean=+0.009  rms=0.016
  zone2: n=6  mean=+0.303  rms=0.305
```

**Zone 0 and zone 1's predictions were borne out reasonably well** — mean
error under 0.05 duty (5 percentage points), small enough to be within normal
tuning/ambient noise. **Zone 2's prediction was not borne out**: the matrix
underpredicts zone 2's steady hold duty by 0.30 (30 percentage points) on
average, with an rms of the same size — not a small residual, a systematic
miss. Zone 2 also carries this rig's highest absolute duty at every plateau
(0.59 → 0.76 → 0.85), so it is both the zone under the most thermal load and
the zone the current matrix most badly mis-predicts.

A re-fit was attempted from this capture's own 6 observations; it is **not**
a usable replacement (reported here for completeness, not adoption): the
fitted matrix has negative entries `(0,0)` and `(0,1)` — physically impossible
for a coupling coefficient — and rows 0 and 1 are not diagonal-dominant, both
flagged by the tool itself as a strong overfitting signature from only 6
observations. The self-check against `PID_EXPANSION_PLAN.md` sec 3.2's known
figures failed on all three zones (zone2 in particular: known -0.007 vs.
observed +0.298 for zone1, known -0.086 vs -0.023 for zone0). This run's data
should feed a properly-designed multi-point coupling re-identification later,
not stand in for one on its own.

## `ff_hold_infeasible`

**Never appeared true, on any zone, at any point in the run** — including at
the peak observed temperature (76.1 C, zone 0). This is a discrepancy against
the CLAUDE.md expectation ("expected to appear above roughly ambient+38°C on
this power-limited rig," i.e. ~67 C here) and against the
`ff_hold_infeasible_above_62c` memory note from an earlier session. Worth a
closer look by whoever owns that finding — either the rig's power margin at
this load point improved, or the two prior observations were made under
different conditions (relay wear, different duty distribution, different
ambient) that this run's specific duty split (max ~0.85 on zone 2) doesn't
reproduce. Not treated as a fault; recorded as new information that revises
the standing expectation.

## Ambient drift

Enclosure temperature: 29.8 C at run start (07:55:45) → 31.8 C at run end
(10:33:28), i.e. **+2.0 C drift over ~2h38m**, recorded continuously via the
60 s sidecar (not just start/end).

## CT channel response

**Confirmed**: only CT channel 2 responded to heater activity. Channels 0 and
1 stayed pinned in a narrow flat band the entire run (ch0: 16–17 counts, 152
of 153 sidecar samples; ch1: 17 counts, all samples). Channel 2 swung from a
quiescent ~60–70 counts (relays off) up to **283 counts** during
high-duty periods late in the run — a wider swing than yesterday's proof run
(64 → 150–172), consistent with this run reaching higher sustained duty on
zone 2 (up to 0.85 vs. the proof run's lower demand). This corroborates the
prior finding and extends its observed range.

## Safety and abort criteria — none triggered

- Max temperature reached: 76.1 C (zone 0), well under the 78 C abort
  threshold and the 80 C ceiling.
- Max measured rate: 3.6 C/min, far under S8's 20 C/min guard.
- `trip_mask` stayed `0x0000` for the entire run after the pre-flight clear;
  `warn_mask` stayed `0x0000` throughout.
- Safety-link `cmd_status_count` kept climbing and CRC/framing error counts
  stayed flat at every spot-check (roughly every 9 min) through the whole
  run; Pico uptime climbed monotonically (43.6M ms → 53.2M ms across the
  run) with no reset.
- No trip, no fault, no relay-authority anomaly at any point.

## State left behind

Firing completed on its own (dwell reached 0 remaining, executor transitioned
to idle cleanly — no stop/abort was needed). Confirmed after completion:
`profiles_get_exec_status` → `state=0` (idle); `io_read` → all four relays
off (`R1=R2=R3=R4=0`); `autotune_get_status` → idle; `safety_get_diag` →
`state armed, trip_mask 0x0000`. Board is safe, idle, and cooling
(61.6/60.3/58.6 C on the three zone TCs, 59.1 C on the safety TC, moments
after completion).

## Capture files

- `logs/coupling/cplval75_20260910.jsonl` — raw 5 s poll (local only, gitignored)
- `logs/coupling/cplval75_20260910_ambient.tsv` — raw 60 s sidecar (local only)
- `logs/coupling/cplval75_20260910_settled_hold_points.tsv` — derived, committed summary

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
