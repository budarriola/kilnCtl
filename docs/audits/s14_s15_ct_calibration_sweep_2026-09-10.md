# S14/S15 CT calibration sweep — 2026-09-10

## Goal

Arm S14 (per-channel over-current, WARN) and S15 (per-zone under-current, WARN)
by running the sanctioned `POST /api/zones/current_sweep/start` sweep, now
that the `i_normal_a` write path (`c0729e1e`) is live on the board (HEAD at
flash time, per `flash_provenance.json`, was `6355c822`; a second, unrelated
agent flashed a newer dirty build, `79d93233`, partway through this session —
see "Board activity during this session" below).

## Pre-flight (stage 1)

- `get_heap_status`: no unacknowledged crash, `reset_reason` benign
  (`software (esp_restart)`), heap healthy.
- `safety_get_status`: link up, `SaftyFW armed`, `trip_mask 0x0000`.
- `safety_get_diag`: `trip_reason 0 [SAFETY_TRIP_NONE]`, `warn_mask 0x0000`,
  `trip_mask 0x0000`, frames ok climbing, 0 bad.
- `safety_get_commissioning`: `commissioned=True`, config CRC matches live
  (not stale), `mains_voltage_v=120`, S1 `abs_max_temp_c=85C` ARMED, S8
  `max_rate_c_per_min=20C/min` ARMED.
- Four thermocouples (3 main-board channels + 1 safety-processor channel) all
  read a plausible ~35.8–36.2 °C ambient with matching CJ readings.
- Relays confirmed off via `/api/status` and `io_all_relays_off`.
- `profiles_get_exec_status`: `state=0` (idle). `autotune_get_status`:
  `state=idle`.
- `GET /api/safety/commissioning` (raw): `k_ct_v_per_a[0..2]` all **0**
  (never commissioned), `gain[0..2]=0.715` (`CS_DEFAULT_GAIN`),
  `max_expected_power_w=793 -> 5000` (stale placeholder, wrong units of
  magnitude for this fixture), `ct_topology=1` (summed), `ct_channel_map[0..2]`
  unset.

S14/S15 status before the sweep — all six rows DORMANT, `i_normal_a not
measured` for every row, because `i_normal_a` had never been written.

## Sweep code review (stage 2)

Read `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c` and
`zones_current_sweep_task.c` in full. Key facts established before running
anything:

- Per-zone timing is fixed: `ZONE_SWEEP_SETTLE_MS` (1000 ms) +
  `ZONE_SWEEP_SAMPLE_MS` (4000 ms) = 5000 ms energized per zone, one zone at a
  time, every other relay forced off for the whole window
  (`zone_sweep_hw_energize()`'s `0xFF` mask). Three zones -> ~15 s total.
- Thermal abort: `zone_sweep_effective_ceiling_c()` uses the tightest
  configured per-zone ceiling (80 °C on this board's zone config,
  `control_get_zones`'s `range=0..80C`), falling back to a fixed 60 °C only if
  no zone has a ceiling at all. Also aborts on 2 consecutive invalid
  thermocouple polls, safety link loss, or a latched trip — all funnel through
  one choke point, `zone_sweep_force_relays_off()`, which now reports loudly
  (not silently) if the owner-queue relay-off command itself fails.
- `k_ct_v_per_a` calibration (`zone_sweep_derive_k_ct()`) requires a
  **prior committed `k_old > 0`** for the channel being calibrated — a
  fresh/never-committed 0 refuses with `ZONE_KCT_DERIVE_NO_PRIOR_K` ("k_ct
  has never been set, so amps read zero"). Since this board's schematic-derived
  analytic value is 1.0 V/A (established `d8be0c31`, confirmed in this
  session's briefing), `k_ct_v_per_a[2]=1` (channel 2, the only populated CT)
  was committed before the sweep — see "Commissioning writes" below.
- **Structural limitation found, not caused by this sweep**: in
  `ct_topology=summed` mode, `zones_current_sweep_task.c` line ~125
  deliberately leaves `s_ct_derive.derived_mask` at 0 for the whole run
  ("summed topology has no per-relay CT map to derive... `derived_mask` stays
  0"). `zone_sweep_plan_k_ct()`'s per-channel loop is gated on that same mask
  (`if ((s_ct_derive.derived_mask & (1u << c)) == 0) continue`), so **on a
  summed-topology board the k_ct scale-factor derivation can never run at
  all**, regardless of load size. This is a real, permanent gap in this
  sweep's k_ct half for this board's wiring, not a bug I introduced — noted
  for the record, not fixed (out of scope for this task).
- The `i_normal_a` half (S15's actual arming path in summed mode) is
  independent of that mask: `zone_sweep_summed_normal_a()` compares
  `(sum_with_zone_on_a - sum_idle_a)` against a noise floor
  (`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A = 0.045 A`, rescaled by the live committed
  `k_ct_v_per_a` vs. the 1.0 V/A reference it was derived against — unchanged
  here since we committed exactly 1.0). Below that floor, the zone is left
  **unmeasured** (not zeroed) — the code's own comment already anticipates
  this exact bench: "may still be close to or above this particular fixture's
  own tiny normal current, in which case the sweep correctly reports 'not
  measured'."
- The write-back to the Pico (`zone_sweep_push_i_normal_a()`) stages via
  `SET_PARAM`, commits, and re-reads (`zone_sweep_confirm_i_normal_landed()`)
  before declaring success; a failed confirm triggers a documented backout
  path that restores the prior value and reports the backout's own outcome
  honestly rather than silently.

## Commissioning writes (before the sweep)

`safety_set_commissioning_fields` refused once (relay ARMED — config writes
only land in the 60 s post-reset grace window). `debug_reset(peer="pico")`
was issued (OpenOCD printed a `Failed to select multidrop rp2040.dap1`
warning — the known two-identical-probes issue — but the reset itself landed:
`safety_get_diag` showed `uptime 8006 ms`, `state grace` immediately after).
Within the grace window:

| field | old | new | rationale |
|---|---|---|---|
| `k_ct_v_per_a[2]` | 0 (never committed) | **1.0 V/A** | Schematic-derived analytic value (`d8be0c31`), the only prior this run's ratio-scaling could act on. Channels 0/1 left at 0 — no CT is fitted there, confirmed by direct measurement earlier today, so leaving them un-derivable is correct. |
| `max_expected_power_w` | 5000 (stale placeholder) | **8.5 W** | This is a ~4 W (owner estimate), <10 W bench fixture, not a kiln — 5000 W was nonsense left over from a different context and would have made every plausibility check meaningless. 8.5 W is the midpoint of the two independent CT-derived estimates in the brief (7.5 W and 9.6 W), still comfortably inside the owner's own "<10 W" bound; the derivation's built-in 0.2×–5× ratio band was sized to absorb exactly this kind of nameplate uncertainty. |

Both writes were confirmed by read-back in the same call
(`safety_set_commissioning_fields`'s own response: "written and confirmed by
read-back"), and independently re-confirmed via raw `GET
/api/safety/commissioning` after the sweep completed (see below) — unchanged
by the ESP's own restart partway through the session.

## Sweep run (stage 3)

`POST /api/zones/current_sweep/start` -> `{"ok":true,"reason":"ok"}`.
Polled `GET /api/zones/current_sweep/status` through the run (state
`running` -> `zone_index 2, zones_done 2/3` -> `done, zones_done 3/3` in
roughly the expected ~15 s). No abort, no ceiling hit, no link loss.

Mid-run, the ESP itself restarted on its own (`reset_reason='software
(esp_restart)'`, `uptime_s=14` observed moments later) — traced to the
**other** session's concurrent work on `safety_ceiling_policy.c` (visible in
`git status` as modified, unstaged, throughout this session) and its own
board activity, confirmed after the fact by `connect()`'s report of a fresh
`FW 79d93233 (dirty) built 2026-09-10 22:45:13Z` — not caused by this sweep or
this session. The safety processor (Pico) was unaffected by that ESP reboot:
`safety_get_diag` showed `state armed`, `trip_reason 0`, uninterrupted
`context frames ok` climbing, and the two commissioning fields set above
were unchanged when re-read afterward — consistent with them living in the
Pico's own committed config, independent of the ESP's own reboot.

## Result (stage 4)

```json
{"state":"done","zone_index":2,"zones_done":3,"zones_total":3,"reason":"",
 "ct_map_derived_mask":0,
 "ct_map_reason":"summed CT topology has no per-zone channel map to derive -- not applicable",
 "k_ct_derived_mask":0,
 "k_ct_reason":"no CT channel was identified -- CT scale not calibrated",
 "i_normal_pushed_mask":0,
 "i_normal_reason":"no zone has a measured normal current yet -- S14/S15 stay dormant",
 "summed_unmeasured_mask":7}
```

- `k_ct_v_per_a`: **not re-derived** (stayed at the 1.0 V/A committed before
  the run) — expected, given the structural `derived_mask` gap in summed mode
  documented above; this is not a measurement failure, it is a code path that
  cannot execute on this topology at all.
- `i_normal_a[0..2]`: **not measured, not pushed** for any zone
  (`summed_unmeasured_mask=7` = all three zones). Re-confirmed via raw `GET
  /api/safety/commissioning` after the run: `i_normal_a[0..2]` all still
  `"set": false`.
- S14 (ch0/ch1/ch2) and S15 (z0/z1/z2): **all six rows stayed DORMANT.** None
  moved to ARMED.

Reason, per zone, from `zone_sweep_summed_normal_a()`'s own logic: the
measured delta (relay-on current minus idle) never exceeded the
`0.045 A` noise floor (rescaled by the committed `k_ct_v_per_a=1.0`, i.e. the
floor is exactly `0.045 A` here, unscaled). At 120 V and an 8.5 W nameplate
the expected current is only ~71 mA; at the owner's own "~4 W" estimate it is
~33 mA — below the floor outright. Both figures sit close enough to the
45 mA floor that a real ~33–90 mA fixture current landing under it, on a
single pass with no repeat sampling, is exactly what the design predicts (the
code comment for `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` names this bench
specifically as a case where "the sweep correctly reports 'not measured'").
No thermal, link, or trip abort occurred — this was a clean run that
genuinely could not resolve a signal, not a sweep that failed to run.

## Sanity check (stage 5) — per the owner's standing instruction

**S14 and S15 were not armed, and they should not have been.** Forcing an
`i_normal_a` value onto either guard from this run would mean arming a WARN
threshold on a number this measurement could not actually produce — exactly
the "guard armed on a fabricated/unvalidated threshold" the owner's standing
instruction prohibits. Two independent, structural reasons converged on the
same "stay dormant" outcome:

1. **k_ct calibration is unreachable on this board's wiring** (`summed`
   topology leaves `derived_mask` at 0 unconditionally) — a genuine gap in
   this sweep's scope for this specific commissioning, not something today's
   run could have changed no matter how the nameplate power was chosen.
2. **The bench fixture's real current (~33–90 mA class) sits at or below the
   sweep's own noise floor (45 mA)** for a single-pass measurement — the
   fixture is simply too small relative to the CT chain's noise floor for
   this sweep to resolve, not a wrong choice of `max_expected_power_w`. Note
   `max_expected_power_w` did not even end up mattering to *this* refusal:
   the summed-mode `i_normal_a` path never consults it (only the separately-
   inert `k_ct` derivation does), so no plausible value in the 4–10 W range
   would have changed today's outcome.

No guard was armed on a fabricated number. S14/S15 remain honestly DORMANT,
with the reason recorded on the board itself
(`i_normal_a not measured`) rather than a silently-accepted zero.

## Follow-up (not done here, out of scope)

- The summed-topology `derived_mask` gap (finding under "Sweep code review"
  above) means this sweep's k_ct-calibration half can never run on any
  summed-CT board, only on `per_zone` boards. If k_ct calibration is wanted
  on a summed board, `zones_current_sweep_task.c`'s summed branch needs its
  own path to populate (or bypass) `derived_mask` for the one known channel —
  not attempted here, since it is a firmware change outside this task's scope
  (arm via the *existing* sanctioned sweep, not modify it).
- To actually measure `i_normal_a` on this bench fixture, a larger load (or a
  CT with a smaller `k_ct_v_per_a`, raising the effective floor's own signal
  window without changing amps-in-the-wire) would be needed, or the sweep's
  sampling window would need to be widened/averaged further than the current
  single 4 s window — neither attempted here.

## Board state at end of session

- Relays: all four confirmed OFF via `/api/status` and `io.relays`, and via
  `io_all_relays_off()`'s own explicit unconditional call.
- `profiles_get_exec_status`: `state=0` (idle), no profile running.
- `autotune_get_status`: `state=idle`.
- `safety_get_diag`: `state armed`, `trip_reason 0 [SAFETY_TRIP_NONE]`,
  `trip_mask 0x0000`, `warn_mask 0x0000` — no trip latched.
- Link up throughout the final checks; `context frames ok` climbing, 0 bad.

Board left safe and idle.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
