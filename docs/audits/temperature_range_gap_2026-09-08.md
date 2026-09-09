# Why measurements stop at ~62-70 °C when the fixture reaches 80 °C (2026-09-08)

## 1. The real reason — verified from the record

**Answer: (a), the profile that was run, not a guard, not the hardware, and
not a deliberate safety cap.** Evidence:

- `logs/coupling/tracking_synthesis_20260904_report.md` §2: of the 64 raw
  firing logs under `logs/coupling/` (27 usable), **all are profile 7, and
  none ever commands a `target_c` above 60.0 °C.** The 60 °C figure in
  memory ("coupling matrix fitted from settled runs below ~60 °C") and the
  "profile 7 never left that region" note both trace to this one fact: one
  profile, never edited to go higher.
- `logs/coupling/hightemp_validation_proposal_20260904.md` §1 checked the
  enforcement chain in code (`profile_executor_run.c` lines 252-338,
  `zones_config_accessors.c` line 771) and read the live board
  (2026-09-04): **all three zones' `max_temp_c` was already 80 °C**, above
  any proposed 65-80 °C target — "on the current live board, a profile
  commanding 65-80 °C is permitted by every KilnFW-side check that exists."
  The only real gate identified was the safety processor's `abs_max_temp_c`,
  which was `0` ("never trip") on 2026-09-04 — an owner-only commissioning
  decision, not something this task can or should change unilaterally.
- The gap was in fact tested: commit `94b1a2a486cc05b33e5224ef0e3a147060d69d13`
  ("Record cplval70 (26-70C) as first above-62C ff_hold validation firing",
  2026-09-05) ran a 26→70 °C ramp + 60 min dwell (profile `cplval70`,
  distinct from profile 7) and completed clean, no fault/ramp-lock. So the
  ceiling was never (c) "kiln can't get there" — 70 °C was reached and held
  the same day the proposal was written.
- The `ff_hold_infeasible` claim itself was, until that run, "the
  feasibility-sweep math ..., not a measured run" per the proposal's §0 —
  i.e. purely theoretical until `cplval70`. `cplval70`'s final snapshot
  showed `ff_hold_infeasible=true` on all three zones simultaneously with
  `ff_hold_used_matrix=true` at the 70 °C dwell end (commit `94b1a2a4`),
  confirming the >62 °C claim by direct flag evidence for the first time —
  **but no PC-side capture ran for that firing**, so only an end-of-run
  snapshot exists, not a tick-by-tick tracking-error series. That is the
  actual remaining gap: not "can't reach it," but "reached it once, with no
  instrumentation attached."

Not (b): no guard refused a higher target on 2026-09-04 (verified in code
and live read). Not (d): no one capped the *profile* deliberately for
safety — the fixture's `max_temp_c=80` ceiling (`firmware/KilnFW/TODO.md`,
2026-08-28) is a fixture limit set *above* the unexplored band, not a cap
imposed to keep firings below 62 °C.

## 2. Current constraints, verified live (read-only, 2026-09-08)

```
control_get_zones:
  Zone 0/1/2 (mode 3): range=0..80C          <- max_temp_c = 80 on all three zones
safety_get_commissioning:
  S1 abs_max_temp_c=80C   ARMED
  S8 max_rate_c_per_min=33.3C/min   ARMED
  commissioned=False (ct_channel_map[0..2] still unset; CT gain cal outstanding)
safety_get_status:
  link up; SaftyFW armed; relay_owner not tripped (no latched trip right now)
```

`abs_max_temp_c` has since been commissioned to 80 °C and armed on the
safety processor (`firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`, step 6a
family, 2026-09-08) — the single-point-of-failure gap the 2026-09-04
proposal flagged is closed. **The 80 °C figure is the same number on both
processors**: KilnFW's `max_temp_c` and SaftyFW's `abs_max_temp_c` are both
80, so there is no independent safety headroom above the fixture ceiling —
80 °C is the hard stop on both sides.

**Headroom available**: highest point ever actually held with any
instrumentation is `cplval70`'s 70 °C dwell (snapshot only, no tracking
series). Highest point ever *targeted* is 70 °C. The ceiling is 80 °C. That
leaves **at most 10 °C of clean headroom above the one existing data point,
and the entire 60-80 °C band still has no tracking-error capture** (only
profile-7 data below 60 °C, and one snapshot at 70 °C). S8's 33.3 °C/min
rate ceiling is not binding anywhere near the ramp rates used (60 °C/hr =
1 °C/min in `cplval70`).

## 3. What a 62-80 °C capture would settle, and what it cannot

**Settles:**
- **`ff_hold` infeasibility, quantitatively.** `cplval70` confirmed the flag
  flips true above 62 °C but gave no measurement of the resulting tracking
  degradation (or absence of one — final offsets were all sub-0.5 °C
  despite infeasibility, which is itself a finding worth explaining: does
  the uncoupled fallback (`diagonal_hold()`/`diagonal_climb()`,
  `zone_coupling_solve.c`) recover cleanly, or was `cplval70` just a mild
  case that happened to be recoverable?). A capture with PC-side tracking
  attached (`run_queue.py`) would produce the ramp-lag/dwell-overshoot/
  steady-error triple the `ITER_TUNE_REDESIGN_PLAN.md` scoring scheme uses,
  bucketed at the 60-70-80 °C temperature buckets it already defines.
- **Whether the coupling matrix still tracks at 70-80 °C.** The matrix's
  own-diagonal terms were fit below ~60-65 °C
  (`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` lines 157-168, adopted in
  `78f21344` "Adopt the re-solved 3x3 coupling matrix into the
  tuned_baseline preset"). `cplval70` is one snapshot suggesting the
  fallback holds up to 70 °C on this bench; 70-80 °C is untested.
- **z2 (bottom zone) saturation margin.** `cplval70` measured z2 duty 0.849
  at 60 °C/hr by 70 °C — a capture that continues to 80 °C would show
  whether z2 saturates (`duty=1.0`, structurally unable to hold) before the
  ceiling is reached, which bounds the fastest ramp rate usable in that band
  going forward.

**Cannot settle:**
- **Radiative-dominance behavior at cone temperatures.** 80 °C is far below
  where radiative heat transfer starts to dominate conductive/convective
  loss (cone ranges run into the hundreds to ~1300 °C for cone 10); nothing
  about high-cone thermal behavior is observable in this band, and this
  capture should not be read as evidence either way about it.
- **Long-run creep/drift of the coupling matrix** — one more firing adds
  one more data point, not a re-identification; per project memory
  ("Self-referential rested check... six firings yielded n=1"), a single
  62-80 °C run is not statistical confirmation, only the first real
  measurement in the band.

## 4. Proposed capture (ready to run, not scheduled by this task)

A **user profile**, not an edit to the builtin schedule table (per standing
prohibition on altering builtin `target_c`/`ramp_c_per_hr`/`dwell_min`/
`segment_count`). Working name `cplval80`:

| Segment | Kind | Target | Rate | Dwell |
|---|---|---|---|---|
| 1 | RAMP_UP | ambient (~25) → 62 °C | 60 °C/hr | — |
| 2 | DWELL | 62 °C | — | 45 min (settle + scored dwell) |
| 3 | RAMP_UP | 62 → 70 °C | 60 °C/hr | — |
| 4 | DWELL | 70 °C | — | 45 min |
| 5 | RAMP_UP | 70 → 80 °C | 60 °C/hr | — |
| 6 | DWELL | 80 °C | — | 60 min (longest — this is the ceiling point, and the point most likely to expose z2 saturation) |

Rationale for stepped dwells rather than one long ramp: each dwell gives a
scoreable segment at a distinct 25 °C-bucket boundary consistent with
`ITER_TUNE_REDESIGN_PLAN.md` §2.1's segment-class scheme (`temperature_bucket`
at 25 °C granularity), and each is long enough to clear the
`MIN_SCORED_TICKS`/capture-transient exclusion (60 s settle window is the
floor; 45-60 min dwells give a genuinely settled steady-state read, not just
enough ticks to pass the floor).

**Preconditions before starting:**
- **Rested baseline.** All three zones at ambient before the firing starts —
  residual heat biases the fitted gain low (project memory,
  "Autotune needs a rested baseline"). Confirm via `control_get_zones` /
  `get_device_log_json` showing all three `actual_c` within ~2 °C of each
  other and of room temperature, not just the zone nominally "under test."
- **PC capture attached this time.** `cplval70` explicitly did not have one;
  `run_queue.py` (or equivalent) must be running so a real tracking-error
  series is produced, not another end-of-run snapshot.

**Estimated duration:** ramps ≈ (62-25)+(70-62)+(80-70) = 65 °C at 60 °C/hr
≈ 65 min of ramp, plus 45+45+60 = 150 min of dwell ≈ **215 min (~3.6 hours)**
total, plus rested-baseline cooldown time beforehand if the fixture is not
already cold.

**Data to collect:** per-zone `actual_c`/`target_c`/duty
(`bd_final_commanded`), `ff_hold_used_matrix`, `ff_hold_infeasible`, and
coupling-matrix diagnostics, at the PC capture's normal tick rate, for the
whole firing — not just at dwell ends, so the transition tick where each
zone's `ff_hold_infeasible` first flips can be read directly instead of
inferred (the explicit open follow-up from `94b1a2a4`).

## 5. Practical blockers, checked live (2026-09-08)

- **S3 (`LOAD_STUCK_ON`) latched trip pending CT calibration — RESOLVED,
  not currently blocking.** `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`
  records the trip's root cause (channel 2 never zero-calibrated) found and
  fixed 2026-09-08 (`zero_counts[2]=63` written, `safety_clear_trip()`
  cleared it, held clear through a 65 s poll). Live read confirms this
  today: `safety_get_status` reports **"relay_owner not tripped"** — no
  latched trip is currently blocking a firing.
- **CT gain calibration — genuinely outstanding**, per the same doc: "Gain
  (`k_ct_v_per_a[2]`/`gain[2]`) was deliberately left untouched... a bench
  step needing a known load with the owner present." `current_a[2]` reads
  `0.00 A` as a result. This affects S14 (over-current WARN) and S15
  (under-current WARN), both currently **DORMANT** (confirmed live,
  `safety_get_commissioning`) — it does **not** affect S1 (`abs_max_temp_c`,
  ARMED) or S8 (rate, ARMED), which are the guards actually relevant to a
  62-80 °C capture. So CT calibration is not a hard blocker for this
  specific capture, only for the over/under-current WARN coverage during it.
- **`ct_channel_map[0..2]` unset** — the reason `commissioned=False` still
  reads live. Same scope as above: affects CT-derived WARN guards, not the
  temperature ceiling guards this capture depends on.
- **Net**: nothing currently blocks running `cplval80` from a safety-config
  standpoint. The only prerequisite worth insisting on is operational —
  rested baseline and PC capture attached — not a commissioning gap.

## Sources

- `logs/coupling/tracking_synthesis_20260904_report.md` §2
- `logs/coupling/hightemp_validation_proposal_20260904.md` §0, §1, §2
- `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §3.6i (lines ~5744-5796),
  §3.2/§3.6c (lines 157-168, 1178-1205)
- `docs/ITER_TUNE_REDESIGN_PLAN.md` §2.1 (segment-class buckets)
- `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` (step 6a family,
  S3 root-cause and clear, CT gain-cal outstanding)
- Commits: `94b1a2a486cc05b33e5224ef0e3a147060d69d13` (cplval70 record),
  `78f21344` (coupling matrix adopted into `tuned_baseline`)
- Live reads 2026-09-08: `safety_get_status`, `safety_get_commissioning`,
  `control_get_zones`
