# Autotune Readiness — 2026-09-07

Goal: make an autotune run as likely to succeed as a firing run now is. Static
code read of the current tree (`firmware/KilnFW/App/drivers/control/
autotune_engine*.c`), plus a live-board readiness snapshot
(`docs/audits/firing_preflight_2026-09-07.md`, same day, ESP `e584067f`).

## 1. Entry path and guard coverage

`autotune_engine_run()` / `autotune_engine_run_to_target()` both validate,
then call `autotune_begin_run_locked()`, which arms the **full**
`thermal_guard` suite from the target zone's own config — deliberately
identical to a firing (`autotune_begin_run_locked()`'s own doc comment: "a
relay test drives real elements around a real setpoint, so it is the run
that most needs guards 1-8, not the one that can afford a relaxed set").

Prestart refusals, in order, before any heating starts
(`autotune_begin_run_locked()`, `autotune_engine.c`):

| Check | Refusal message | Notes |
|---|---|---|
| Engine started | "autotune engine not started" | `s_at.lock == NULL` guard against recovery-mode / prestart calls |
| OTA in progress | (from `ota_http_heat_blocked_by_update()`) | mutual interlock with OTA on either processor |
| Zone current sweep active | "a zone current sweep is running" | checked twice — early, and again atomically at commit |
| Heat blocked at all | "heat is blocked (...)" | `relay_authority_on_blocked()` — catches a down/tripped safety link before any run starts |
| Zone config invalid | "zone config failed to load..." | `zones_config_is_valid()` |
| No relay mask | "zone has no relay mask configured" | |
| **No thermocouple assigned (NEW, this pass)** | "zone %u has no thermocouple channel assigned..." | see §3 |
| Another profile driving this zone | "a profile is running on zone %u..." | |
| Another profile on a DIFFERENT zone (STEP method only) | "a profile is running on another zone..." | protects the step test's element-alive detection |
| (target mode only) target too close to ceiling | "target %.1fC too close to zone ceiling..." | |
| (target mode only) no max_temp_c and no explicit target | "cannot derive a default target..." | |

Once running, every tick re-checks the whole-run time budget and the
other-zone-profile hint (`autotune_engine_tick_locked()`), then runs
`thermal_guard_tick()` with the same guard config a firing would see.

### Guard-by-guard table (during autotune)

| Guard | Trip enum | Can fire during autotune? | Consequence |
|---|---|---|---|
| 1 — heating failed / no progress | `THERMAL_GUARD_TRIP_HEATING_FAILED` | Yes, both methods. STEP relaxes it once `step_element_proven` latches (see §2). | `autotune_escalate_and_abort()` — relays forced off, zone latched blocked |
| 2 — wrong direction | `THERMAL_GUARD_TRIP_WRONG_DIRECTION` | Yes. Relay method uses `relay_min_swing_c` (amplitude, not direction) specifically so a healthy limit cycle's downswing can't false-trip it (§2). | same |
| 3 — runaway (heat off, still climbing) | `THERMAL_GUARD_TRIP_RUNAWAY` | Structurally near-inert for STEP (duty is the intended value, never silently 0 outside the one relay-law edge case `d==MAX_D`); genuinely armed for RELAY. | same |
| 4 — drift at/near setpoint | `THERMAL_GUARD_TRIP_DRIFT` | STEP: fed a synthetic setpoint, `no_setpoint=true` tells the guard not to treat it as real (fixed — see §2). RELAY: fully live, real setpoint. | same |
| 5 — max temp | `THERMAL_GUARD_TRIP_MAX_TEMP` | Yes, if `max_temp_c` configured. | same |
| 6 — min temp | `THERMAL_GUARD_TRIP_MIN_TEMP` | Yes. | same |
| 7 — sensor invalid | `THERMAL_GUARD_TRIP_SENSOR_INVALID` | Yes — this is the one a missing thermo_mask used to dodge for hours (§3). | same |
| 8 — frozen sensor | `THERMAL_GUARD_TRIP_FROZEN` | Yes. | same |
| 9 — cross-zone | `THERMAL_GUARD_TRIP_CROSS_ZONE` | Not evaluated by this engine's single-zone tick (firing-only, per `profile_executor.c`). | n/a here |
| relay-cycling stall discriminator | `THERMAL_GUARD_TRIP_RELAY_STALLED` | RELAY method only. | same |
| Autotune's own element-alive/death check (not a `thermal_guard` guard) | `THERMAL_GUARD_TRIP_WRONG_DIRECTION` (reused code) | STEP, STEPPING only — latches "proven" on rise, then requires either a relative drop from the running peak or an absolute floor breach (both closed, see round-3 fixes in `autotune_engine.c`'s own comments). | same |
| Whole-run time budget | n/a (own `abort_locked()`) | STEP only, checked every tick from `run_start_tick`. | run aborted, no escalation |
| Neighbour-profile-started-mid-run | n/a | STEP only. | run aborted |

A guard trip always ends in `autotune_escalate_and_abort()`: relays are
forced off, and (for a per-zone trip) the zone is **latched blocked** —
carried forward into the *next* run/firing on that zone until explicitly
cleared. `autotune_begin_run_locked()` now clears a stale per-zone or
global latch from an *earlier* run automatically when the operator starts a
new autotune (loud log line, same policy `profile_executor.c` uses when a
new firing starts) — so a leftover trip from a previous session does not
silently block a fresh attempt, but also does not silently forgive whatever
tripped it.

## 2. Historically dangerous interactions — status as of this tree

| Issue (from standing memory) | Status | Evidence |
|---|---|---|
| Fake setpoint breaks guard 1 (error pinned at 0, guard 1 never runs) | **Fixed** | `autotune_engine.c`'s `thermal_guard_input_t` construction: STEP feeds `raw_c + step_test_guard_headroom_c` (or `max_temp_c` when configured) instead of bare `raw_c`, keeping `error` strictly positive so guard 1 (not guard 2) sees every window. |
| Fake setpoint falsely arms guard 4's drift backstop | **Fixed** | `.no_setpoint = (method != RELAY)` tells guard 4 not to treat STEP's placeholder as a real target; a further SETTLING-only exception feeds `raw_c` itself (error pinned at 0) during SETTLING specifically, since `commanded_duty` is always 0 there anyway. |
| Guard 1 fires exactly when identification succeeds (old settle bug) | **Fixed, recent** (commit `992f3954`, "Wire thermal_guard_cfg_t.progress_band_c to zones config") | Guard 1 now has a configurable "arrival band" (`progress_band_c`, `thermal_guard.c` line ~16/237) so a zone that has genuinely arrived near its target is not treated as stalled. Read live 2026-09-07: all three zones report `progress_band_c=0.000`, which is the documented "unset → default (3.0C)" sentinel, not a broken value (`PROGRESS_BAND_C` default in `thermal_guard.c`). |
| Relay identification: 60s PWM window swallows 1Hz relay-law switching | **Fixed** | `heater_output_duty_relay_step()` forces the actuator's PWM window to end/restart on the exact tick the relay law's branch flips (`relay_level_changed`), rather than waiting up to `window_ms`. |
| Rested-baseline check was self-referential (compared a zone to its own history) | **Fixed, current design** | `check_thermal_readiness_locked()` (§ `autotune_engine_step_identify.c`) now checks (1) each zone's own recent slope since SETTLING started (`SETTLE_ABS_SLOPE_FLOOR_C_PER_S`), which is self-referential but about *drift*, not about a ratcheting baseline, and (2) cross-zone spread against the coolest *currently valid* zone (`min_baseline_c`) using `autotune_min_rise_c()` as the threshold — not a fixed absolute-ambient number tuned to one rig. Explicitly checks **every** zone with a valid reading, not just the zone under test (this is the "all zones actually at ambient" requirement). |
| Autotune feeds a fake setpoint that other guard-reading code (outside this engine) might trust | **Live risk, not addressed here** | `thermal_guard_input_t.setpoint_c` is a real field read by exactly one consumer (`thermal_guard_tick()`), gated by `no_setpoint`. No other module was found reading `s_at`'s internal setpoint value directly in this pass — out of scope to re-audit exhaustively here; flagged for anyone adding a new consumer of autotune status to check `no_setpoint` semantics first. |
| No prestart check that the tested zone actually has a thermocouple assigned | **Fixed, this pass** | See §3. |

## 3. New prestart check added this pass

**Gap found:** `autotune_begin_run_locked()` validates `zones_config_is_valid()`
and the zone's relay mask, but never checked `zones_config_get_thermo_mask()`.
A zone with `thermo_mask == 0` (no thermocouple channel assigned) passes
every existing prestart check, then passes `check_thermal_readiness_locked()`
too — that function's own "cannot judge it, so it cannot block the run"
degrade path skips exactly the zone whose reading is always invalid. The run
proceeds into STEPPING with `actual_valid` permanently false, every
commanded duty gated to `0.0f`, and drives nothing for the **entire**
step-test budget (up to `AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S`, i.e.
hours) before `autotune_finalize_fit()` finally refuses the flat trace.
Exactly the "wasted 3-hour tune" this task asked to prevent cheaply.

**Fix:** `autotune_begin_run_locked()` (`autotune_engine.c`, right after the
relay-mask check) now refuses immediately if `zones_config_get_thermo_mask()`
returns 0, with:

> `zone %u has no thermocouple channel assigned -- autotune cannot measure a response with no sensor to read`

**Host test:** `test_run_refuses_zone_with_no_thermo_mask()`
(`firmware/KilnFW/App/test/test_autotune_engine_prestart.c`), through the
real `autotune_engine_run()` prestart path (not a unit test of the check in
isolation). Includes a control case proving the refusal is specifically
about `thermo_mask`, and a mutation check confirming the assertion is
load-bearing: temporarily disabling the new condition (`if (false && ...)`)
was confirmed to turn the test RED before the fix was restored (standing
"negative-test every check" practice — see `feedback_negative_test_every_check.md`).
Full suite: 714/714 checks in `kilnctl_host_tests_autotune_engine.exe`,
29/29 host test executables, 59/61 `run_all_checks.ps1` guards (1 skip —
headless UI sweep timeout, environmental; 1 unrelated pre-existing failure
in `check_no_duplicate_crc.ps1` from a stale concurrent-session temp
directory, not touched by this change).

No guard was weakened or disarmed to make this pass — every check above
that legitimately conflicts with a step test (guard 4's synthetic setpoint,
guard 1's stall detector) was already resolved by feeding it honest data
(`no_setpoint`, the arrival band), not by turning it off.

## 4. Operator procedure

### Preconditions (verify before starting)

1. **Every zone is actually at ambient — not just the zone under test.**
   `check_thermal_readiness_locked()` enforces this automatically after the
   180s SETTLING hold (`AUTOTUNE_ENGINE_SETTLE_S`), but do not rely on it as
   your only check: it degrades open on a sensor fault, and its cross-zone
   spread check is a coarse divider, not a tight bound. If the kiln fired
   recently, wait for genuine cool-down, not just for the settle timer to
   run out on the one zone you are watching.
2. **Safety processor not tripped.** Read `safety_get_diag()` /
   `safety_get_status()` first. `autotune_begin_run_locked()` refuses
   cleanly ("heat is blocked...") if `relay_authority_on_blocked()` sees a
   fault, but confirm *why* it tripped before clearing it — starting a new
   autotune auto-clears a stale per-zone or global guard latch from an
   earlier run (loud log line), which is correct behavior but means the
   operator, not the firmware, is the one who should have understood the
   prior trip first.
3. **Zone commissioning:** `max_temp_c` configured (or pass an explicit
   `target_c` for target mode), relay mask assigned, thermo_mask assigned
   (now refused automatically if missing, §3), no OTA in progress, no zone
   current sweep running.
4. **No other profile running.** A profile on the *same* zone always
   refuses; a profile on *any other* zone also refuses (STEP method) because
   the element-alive detection cannot separate that heat from this zone's
   own.
5. Check for an unacknowledged crash report (`get_heap_status()`) before
   starting anything — `capability_preflight` refuses runs on a board with
   one, but confirm directly if using a raw HTTP call instead of the MCP
   gate.

### What to watch during a run

- **SETTLING** (first ~180s): duty is 0 on every zone the run touches. If
  `check_thermal_readiness_locked()` refuses here, that is doing its job —
  wait, don't retry immediately without understanding which zone/why.
- **STEPPING** (STEP method): duty steps to the configured value. Expect a
  monotonic rise; watch `step_element_proven` transition (visible via
  `autotune_engine_get_status()`) — this is the point guard 1's rise
  requirement relaxes and the running-peak death check takes over instead.
- **RELAY_APPROACH / RELAY_CYCLING** (relay method): the kiln climbs to the
  setpoint, then oscillates. Expect `relay_cycles_seen` to climb toward
  `AUTOTUNE_RELAY_TARGET_CYCLES`; a run stuck at 0 cycles well past the
  approach window is not identifying anything.
- **Trace sample count** (`autotune_engine_get_trace()`/status
  `sample_count`) should be climbing at the expected ~10s sample period —
  flat for minutes is the same "nothing is happening" signal whether or not
  a guard has noticed yet.

### Good run vs. bad run

| Signal | Good run | Bad run |
|---|---|---|
| `state` at completion | `DONE` | `ABORTED` |
| `abort_reason` | empty | populated — read it, it names the zone and the specific failure |
| `model.k_gain_c_per_duty` / `model.tau_s` | nonzero, physically plausible for this plant | zero, or `proposed_gains.refusal != AUTOTUNE_REFUSAL_OK` (now correctly surfaced on the dashboard since commit `813ad90` — a refused tune no longer LOOKS like a successful one) |
| Rise trace | clear rise from baseline, no return to flat before `step_element_proven` | flat/noise-dominated for the whole budget (missing thermo_mask — now refused before it starts, §3; or a genuinely dead element) |
| relay method cycles | steady period, consistent amplitude around setpoint | erratic cycle timing, or amplitude collapsing toward `relay_h`'s floor |
| Accept button state | enabled only on a genuine, unrefused fit | **never** enabled on a refusal, post-`813ad90` |

### Known failure signatures and their tells

- **"response too small to fit (trace flat or noise-dominated)"** — check
  `thermo_mask` first (§3, now refused before this can happen); otherwise a
  genuinely weak/dead element, or `step_duty` too low for this plant's dead
  time (34–53s measured on this rig — see
  `project_relay_ident_actuation_lag.md`).
- **Abort naming a specific zone as "still drifting...C/s"** — a rested
  baseline was never reached; the kiln fired recently or another heat
  source is still cooling. Not a bug — wait longer before retrying.
- **Abort naming a *different* zone's spread** — that zone plateaued hot
  relative to the coolest zone; a previous firing or autotune left it
  warm. Confirm with a manual temperature read on that zone before
  retrying.
- **"element died after proving alive: dropped ...C from a ...C peak" /
  "...stayed below the ...C floor"** — a real thermal excursion (a
  loose connection, tripped breaker, or genuinely failed element) during
  the run, not a fitting artifact — the running-peak/absolute-floor
  double check (round-3 fix) specifically closed the dead zone where this
  used to be unreachable below a 5.0C peak.
- **Relay run stuck in RELAY_APPROACH past `AUTOTUNE_RELAY_APPROACH_MAX_S`
  (4h)** — "did not reach ...C within ...s -- setpoint out of reach": the
  setpoint is unreachable at the plant's max duty, or the element is too
  weak. Not the same failure as a step test's flat trace — this one never
  even got the chance to record data.
- **Fit computed but `proposed_gains.refusal != OK`** — read the reason
  string; this is `pid_autotune_fit_fopdt()`/`tune_from_fopdt()` refusing on
  its own criteria (dead time too small for the chosen rule, tau
  inconsistency, extrapolation not converged, etc.) even though the run
  itself completed cleanly. This is the exact case `813ad90` fixed the UI
  for — confirm the dashboard actually shows the refusal banner and Accept
  is disabled, don't just trust that "it finished" means "it produced usable
  gains."
