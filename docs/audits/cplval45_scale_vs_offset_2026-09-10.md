# cplval45 discriminating-plateau capture — completed, SCALE favored over OFFSET (2026-09-10)

## Pre-registered predictions (restated verbatim, before any data below)

From `docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md` (commit
`238e0eb9`): the zone-2 forward residual `G·u − ΔT` at a settled 45 °C
plateau should read approximately

- **−2.9 °C if the discrepancy is a per-row SCALE error**
- **−8.4 °C if it is a constant OFFSET error**

These were derived from the 62/70/75 °C plateau data
(`docs/audits/cplval75_coupling_verdict_2026-09-10.md`, commit `d2e570ad`;
raw points in `logs/coupling/cplval75_20260910_settled_hold_points.tsv`,
commit `1efbdc0c`), which spans only 1.4x in rise — too narrow to
discriminate the two hypotheses. This run targets 45 °C (a ~2.9x span
against the same ~34 °C ambient) specifically to discriminate them.

## Why the previous attempt aborted, and why it did not recur

The 2026-09-10 morning attempt aborted after the safety processor (Pico)
rebooted twice, unexplained, ~30 s into the ramp
(`docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md`). Since then:
the Pico was reflashed to current firmware; three tasks that were on bare
`configMINIMAL_STACK_SIZE` were fixed; `link_task`/`update_task` stacks were
raised with the FreeRTOS heap raised to match; the stack-budget check now
reports 9/9 ok; and a spurious guard-1 trip (zone 0's `wrong_dir_window_s`
hardcoded to 60 s against a plant with τ≈264 s / 53 s dead time) was cleared,
with firmware now deriving a ~317 s floor. This run watched for both classes
of prior failure throughout and saw neither: the Pico held one `boot_id`
(255) and one continuously-climbing `uptime` (296 s → 3676 s, no reboot,
`trip_mask 0x0000` throughout, `context frames bad=0`) for the full run, and
no guard tripped.

**Mid-run addition to the abort criteria** (relayed during the run, not
present in the original brief): a same-day review flagged that commit
`1132d13a`'s `safety_ceiling_sync_reconcile_on_link_up()` runs every
`safety_poll_task` tick and can perform blocking UART round-trips with no
budget/backoff, and that the Pico refuses config writes while a relay is
armed — exactly this run's state — so a raise-needed condition could retry
twice a second indefinitely and starve the 5 s task watchdog. Assessed as
dormant on this board (cached ceiling already adequate) and not grounds to
abort, but watched for explicitly: `get_heap_status`'s `reset_reason`/
`uptime_s` was checked periodically (ESP uptime climbed 7428 s → 11017 s
with no reset and no unacknowledged-crash banner throughout) and the device
log was checked for a flood of ceiling-sync warnings (none found — the only
warnings logged were this session's own harmless probes of the
nonexistent `/api/safety` HTTP endpoint). No evidence the retry loop was
ever live.

## Pre-flight (all nominal)

- `get_heap_status`: ESP reachable, `reset_reason='panic/exception'` but from
  a **stale** prior boot (`uptime_s=7428` at the first check, climbing
  cleanly thereafter with no reset) — no unacknowledged-crash banner.
- `safety_get_status`: link up, armed, not tripped, `ct_counts` sane.
- `safety_get_commissioning`: `commissioned=True`, config CRC matches live
  (not stale), S1 `abs_max_temp_c=80C` ARMED, S8 `max_rate_c_per_min=20C/min`
  ARMED, **`mains_voltage_v=120`** confirmed still persisted correctly (no
  revert).
- `capability_preflight_check(name="bench_fixture")`: **ok to start**, no
  HTTP-gated capability missing.
- `profiles_get_exec_status`: idle. `autotune_get_status`: idle.
- `thermo_read`: CH0/1/2 = 34.53 / 34.45 / 34.36 °C — closely clustered,
  plausible, board rested (no residual heat from prior work; no wait needed).
- `control_get_zones`: all three zones `range=0..80C` — matches the Pico's
  80 °C ceiling, never tighter on the Pico than the ESP.
- `io_read` / `GET /api/status`: `R1=R2=R3=R4=0`/`relays: all off`.

## User profile (borrowed slot, restored)

All 8 user slots were occupied. Slot **#0** (`cplval75`, the 62/70/75 °C
profile used for the earlier plateau run) was read back and recorded before
being overwritten:

```
zone_mask=0x7
segment 0: target=62.0C ramp=60.0C/hr dwell=30min
segment 1: target=70.0C ramp=40.0C/hr dwell=30min
segment 2: target=75.0C ramp=40.0C/hr dwell=45min
```

Overwritten with a single-segment `cplval45` profile: target 45.0 °C, ramp
60 °C/hr, dwell 60 min, `zone_mask=0x7` (dwell sized past 8τ ≈ 36 min at the
live `model_tau_s`≈264–271 s). After the run, slot #0 was restored to the
exact segments above and confirmed by `profiles_get` read-back matching
verbatim.

## The run

`profiles_start(profile_id=0)` at 13:19 local. Ramp reached the 45 °C dwell
at run-elapsed 638 s (~10.6 min), all duties tracking 0.10–0.25 throughout
ramp and dwell — never approaching the 0.35 ceiling in the brief. The dwell
ran 2758 s (~46 min, ~10.4τ at τ=264 s) before being stopped manually once
settling was confirmed from flat duty and flat temperature (not from
elapsed time alone): the final-10-minute window shows per-zone temperature
std ≤0.48 °C with duty oscillating in a stable, non-drifting band. Safety
processor and ESP were both stable throughout (see above). No trip, no
fault_guard, no zone above 55 °C at any point (peak zone reading ≈46.7 °C
transiently near dwell entry).

Ambient at plateau (thermocouple cold-junction, near-board, as a room-air
proxy since none of this board's TCs measure a separate ambient channel):
CJ 35.0–35.2 °C pre-run, 35.4–36.1 °C at plateau — a small (~0.5–0.9 °C)
warm drift over the run, consistent with heat soak near the enclosure rather
than a room-temperature swing.

### Settled hold points (final 600 s of dwell)

| zone | ambient_c (first reading) | mean_temp_c | temp_std_c | mean_duty | ΔT_c |
|---|---|---|---|---|---|
| 0 | 34.45 | 44.989 | 0.476 | 0.1485 | 10.539 |
| 1 | 34.42 | 45.057 | 0.358 | 0.1672 | 10.637 |
| 2 | 34.23 | 45.028 | 0.347 | 0.1864 | 10.798 |

Full derivation and provenance: `logs/coupling/cplval45_20260910_settled_hold_points.tsv`.

### Forward residual `G·u − ΔT`

Using the live matrix (diagonal from `model_k_dc`, off-diagonals as stored
on-board — unchanged from the same-day cplval75 reading):

```
G = [[39.2459, 27.32,  21.72 ],
     [14.30,   31.9669,22.15 ],
     [ 8.33,   12.42,  31.681]]
```

| zone | G·u | ΔT | residual | G·u / ΔT |
|---|---|---|---|---|
| 0 | 14.446 | 10.539 | **+3.907** | 1.371 |
| 1 | 11.599 | 10.637 | **+0.962** | 1.090 |
| 2 | 9.220 | 10.798 | **−1.578** | 0.854 |

## Which hypothesis the data supports

**z2 residual = −1.58 °C.** Against the pre-registered figures (−2.9 SCALE /
−8.4 OFFSET), this is:

- **~45 % of the SCALE prediction's magnitude**, same sign.
- **less than a fifth of the OFFSET prediction's magnitude.**

The OFFSET hypothesis is decisively excluded: a −8.4 °C constant error at
this plateau's ΔT≈10.8 °C would put the matrix's predicted required duty
*negative*, which is not what was observed (the matrix still over-predicts,
just less than at 62–75 °C). SCALE is the better-supported hypothesis — same
sign as both the 62–75 °C data and the pre-registration — but the magnitude
undershoots a naive linear (ΔT-proportional) extrapolation from the z2 75 °C
point (that extrapolation predicts ≈−2.5 °C here). Plausible reasons for the
gap, not adjudicated by this run: this plateau's smaller ΔT gives
proportionally larger relative measurement noise (temp_std/ΔT ≈3.2 % here
vs <1 % at the 75 °C point), and/or the per-row scale factor is not itself
ΔT-invariant all the way down to ΔT≈11 °C.

**New finding, outside either pre-registered hypothesis:** z0's residual
**flips sign** relative to the 62–75 °C runs (+3.91 °C here vs −3.36/−4.28/
−5.58 °C there — the matrix now *over*-predicts z0's required duty at this
low ΔT, having under-predicted it at every higher plateau). Neither SCALE
nor OFFSET as stated predicts a sign flip on any row. This is worth a
follow-up pass before the matrix is revised, and is flagged here rather than
adjudicated.

## CT channel-2 counts at plateau — real draw still an open question

Only CT channel 2 (`ct_topology="summed"`, the whole-kiln GPIO28 channel) is
fitted; channels 0/1 are not. `power_w`/`ct_current_a` read `0.0` throughout
— the board's `k_ct_v_per_a` is uncalibrated (`cs_counts_to_amps()` returns
0 when uncalibrated, per `firmware/SaftyFW/docs/CURRENT_SENSE.md` §5), so
raw ADC counts are the only signal available; they were NOT converted to a
trusted watts figure by firmware and are reported here as raw counts plus
two candidate conversions, not as a settled number.

Over the full run: channel-2 counts ranged 52–168 (mean 90). Split by
whether any relay was instantaneously energized (`/api/status` `relays`):

| state | n samples | mean counts | max counts |
|---|---|---|---|
| any relay ON | 422 | 141.5 | 168 |
| all relays OFF | 819 | 63.3 | — |

Restricted to the final-10-minute plateau window: ON mean 143.6 (max 161,
n=67), OFF mean 63.2 (n=150) — consistent with the whole-run figures, so
this is not a ramp-only transient.

Using `firmware/SaftyFW/docs/CURRENT_SENSE.md`'s own transfer function,
`I_rms = V_adc / (0.715·√2·k_ct)`, with `V_adc = counts·3.3/4096`, ON−OFF
delta = 78.2 counts ⇒ ΔV_adc ≈ 0.0630 V:

- **k_ct = 1.0 V_rms/A_rms** (the owner's stated CT transfer, 1 V AC rms per
  1 A AC rms, taken at face value): I_rms ≈ 0.0623 A ⇒ **P ≈ 7.5 W** at
  120 V — closer to the earlier session's ~10 W implied swing than to the
  ~4 W estimate.
- **k_ct = 2.0 V_rms/A_rms** (folding in the owner's separately-stated
  conditioning-stage gain, "2 V DC per 1 V rms"): I_rms ≈ 0.0311 A ⇒
  **P ≈ 3.7 W** — closer to the ~4 W estimate.

These two readings differ by exactly the disputed 2x gain factor, and this
capture does not resolve which applies: `CURRENT_SENSE.md`'s own worked
transfer function (§2) derives `V_adc ≈ 1.011·V_ct_rms` for a clean
sinusoid — i.e. very close to unity gain through the peak-detector stage,
not 2x — which is in tension with the owner's "2 V DC per 1 V rms"
description of the conditioning stage as stated. Also unresolved: this
capture's 5 s poll interval undersamples the zones' 60 s PWM window
(`heater_window_ms=60000`), so an "ON" sample only proves at least one zone
was mid-pulse at that instant, not that it read a settled peak — the true
peak-envelope value could be somewhat higher than this ON-sample mean.
**Recommendation:** calibrate `k_ct_v_per_a` properly (commissioning flow,
`CURRENT_SENSE.md` §5) before trusting either figure, and/or capture at the
safety link's own 500 ms cadence (`safety_capture_ct_counts`) rather than
5 s HTTP polling to avoid the PWM-window aliasing.

## Post-run verification

- `profiles_stop()` → `ok - stopped`.
- Relays confirmed off through **both** paths: `io_read` → `R1=0 R2=0 R3=0
  R4=0`, and `GET /api/status` → `relays: [{1,false},{2,false},{3,false},
  {4,false}]`.
- `profiles_get_exec_status` → `state=0` (idle).
- `safety_get_status` → link up, armed, not tripped.
- User profile slot #0 restored to `cplval75`'s exact original segments,
  confirmed by `profiles_get` read-back.

**Board left safe and idle: relays off (both `io.relays` and
`/api/status`), executor idle, no trip latched, safety processor stable
throughout with no reboot.**

## Data

- `logs/coupling/cplval45_20260910.jsonl` — raw 5 s HTTP poll capture
  (local only, `.gitignore:100`).
- `logs/coupling/cplval45_20260910_examexec.jsonl` — same data reshaped to
  the `HH:MM:SS {exec body}` form `coupled_ident_report`/`log_analysis`
  expect (local only, same gitignore pattern).
- `logs/coupling/cplval45_20260910_settled_hold_points.tsv` — derived
  summary, committed alongside this document.
