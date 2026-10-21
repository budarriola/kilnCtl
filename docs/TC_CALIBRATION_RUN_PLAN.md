# TC calibration run (sheath lag tau and offsets): plan

Owner request 2026-10-16. Design only, nothing implemented. During setup, the
user measures each thermocouple's mullite sheath lag (tau) and its offset. A
pyrometer or reference thermometer is optional, never required. The stored
fields (`zone_cfg_t` tau next to `cal_offset_c`, Pico tau next to
`tc_offset_c`) and the manual wizard entry step belong to a separate work item. This plan only
fills those fields from a measurement.

## Recommendation

1. **Measure inside the STEP autotune, then add a short hold tail.** The step
   method already settles at duty 0 and applies an open-loop duty step. That
   duty step is the constant-ish ramp the lag estimate needs. Record the Pico
   TC next to the zone TC for the whole trace. After the fit, hold briefly at
   a bounded temperature, where the user can take an optional reference
   reading. This needs one heat cycle and no extra cool-down to ambient.
2. **Also offer a standalone "TC calibration run"** as wizard step 10a, using
   the same engine and the same phases. It is for users who tune with the
   relay method, type PID gains by hand, re-measure after replacing a tube,
   or skipped the measurement during autotune.
3. **Ordering is solved analytically, with no second autotune.** The FOPDT fit
   runs on the raw (sheathed) signal, so its apparent dead time is
   `L_app ≈ L_true + tau_s`, since the sheath pole is much faster than the
   kiln pole. If tau compensation later feeds the control loop, re-derive the
   gains with `pid_autotune_tune_from_fopdt()` from the stored
   `(K, tau_k, L_app - tau_s)`. The Accept screen shows both gain sets. A
   full re-run is suggested only if `tau_s > 0.5 * L_app`: then the
   one-pole-plus-delay approximation is weak.
4. **Accept is separate per item and nothing is saved before Accept.** That
   gives three separate decisions: PID gains (existing autotune accept),
   zone tau and offset (`POST /api/zones`), and Pico tau and offset
   (`/api/safety/commissioning`, named confirm, write window).

## What each method can identify

Symbols: `r` = ramp rate (C/s), `tau_s` = sheath lag, `tau_k` = kiln
dominant lag. With a 60 s tube at 150 C/h, the lag is 2.5 C. At 300 C/h it
is 5 C. Lag is a few degrees, so it is only identifiable with a fast ramp
and a low-noise comparison.

| Method | Gives | Cannot give | Error bound | Status |
|---|---|---|---|---|
| Ambient check at power-on (kiln rested at least 8 h, every TC vs room thermometer or each other) | absolute offset at ambient (mostly cold-junction error); TC-to-TC agreement | anything rate-dependent or high-temperature | ±1 C with a room thermometer; relative ±0.3 C without one | always run, room thermometer optional |
| MAX31856 cold-junction read vs ambient | CJ sensor fault/offset | TC wire error | ±1 C (CJ spec ±0.7 C) | always, free |
| **Two-sensor cross-fit** (zone TC vs Pico TC, tips within about 5 cm) | **differential tau** `tau_a - tau_b`: absolute tau if one tip is bare or thin-walled | absolute offset; tau if both sensors have the same tube (it only shows they match) | ±15 % or ±10 s at a ramp of 150 C/h or faster | **pyrometer-free primary** |
| Two zones (different heights) | relative offset after a long soak | tau: the vertical gradient is rate-dependent and confounded with lag | offset ±2 C after a 30 min soak | relative only, label it so |
| Single sensor, FOPDT fit | **upper bound** `tau_s ≤ L_app` | tau itself: sheath and kiln lag are confounded; the second pole sits below the noise when `tau_s << tau_k` | n/a | report the bound and use the default |
| Single sensor, heat-off coast overshoot `≈ tau_s * (r_before - r_after)` | another upper bound | tau: element and wall heat storage also keep the air rising | n/a | bound only, never stored as tau |
| Reference probe (handheld type K through the peephole, tip within about 5 cm of the zone TC) at a hold | absolute offset at the hold temperature | rate behaviour unless read during the ramp | ±(2.2 C or 0.75 %) probe class + ±2 C gradient | optional, best offset |
| Reference probe during the ramp, 3+ readings | tau = `((T_ref - T_tc)_ramp - (T_ref - T_tc)_hold) / r` | tau when the lag is under 3× the reading noise: the estimator returns NOT_IDENTIFIABLE | ±30 % at best | optional |
| IR pyrometer through the peephole | gross offset check | air temperature: it sees the far wall or the ware, not the TC tip | ±1-2 % instrument; emissivity 0.95 set vs 0.85 true at 8-14 µm and 300 C is about -15 C; window glass blocks IR | optional, a sanity check only, never stored automatically |
| Witness cone (Orton, at a known heating rate) | offset near peak temperature, after a real firing | anything inside the bounded cal run (temperature too low) | about ±5-10 C equivalent | optional, post-first-firing advice text only |
| Ice point / boiling water (probe off the kiln) | wire and channel offset at 0 and 100 C (correct boiling point for altitude) | high-temperature drift and inhomogeneity | ±0.5 C | optional, only for a removable probe |

**Pyrometer-free values:**
- ambient absolute offset for every TC (with a room thermometer), or relative
  offsets without one;
- CJ health;
- differential tau between the zone TC and the Pico TC (absolute tau when the
  Pico TC is bare or thin-walled);
- the `tau_s ≤ L_app` bound.

**Defaults:**
- **Absolute tau for a single-sensor zone** gets the default 60 s for a
  standard 10 mm mullite tube (owner to confirm). The fit refuses any value
  above `L_app`.
- **High-temperature absolute offset** stays 0 unless a reference or a cone
  supplies it.

## Phases (both placements)

Bounded temperature: `T_hi = min(300 C, zone max_temp_c - 50, Pico abs_max_temp_c - 100)`.
A lower bound fails the run with a clear reason rather than running.

| Phase | Action | Captured |
|---|---|---|
| P0 ambient | Relays off, 120 s average | Ambient offsets, CJ, relative offsets. Refuse if any TC is more than 5 C from the median (not rested) |
| P1 ramp | STEP autotune duty step, or for 10a a fixed duty that gives 150 C/h or more (start at 100 %, measure the slope over 5 min) | Zone and Pico traces for the cross-fit. Optional "take reading now" prompts at `T_hi - 100` and `T_hi - 50` |
| P2 hold | Closed loop at `T_hi` with current or default gains until the slope is under 0.2 C/min for 5 min (cap 45 min) | Hold offsets. Optional reference readings, averaged |
| P3 off | Heat off, record 10 min | Coast bound |
| Result | Estimator per sensor, sanity bounds | Old vs new, Accept/Reject per item |

The STEP autotune runs P0 (already its settle phase), then P1 (its step),
then P2/P3 as an opt-in tail ("Also measure thermocouple lag/offset (+30-50
min)"). The autotune fit is unchanged.

**Engine.** A new `autotune_engine` method `AUTOTUNE_METHOD_TC_CAL` plus a
`tc_cal` tail flag on STEP. That reuses relay authority, the refusal list,
the guards, abort, and accept ownership (`autotune_engine.c`). No new relay
writer is needed. The estimator lives in a pure module (`tc_cal_estimate.c`),
host-testable, and fits `T_s = lowpass(T_b, tau) + c` by a 1-D search over
tau with closed-form c.

## User guidance

**Before starting** (one screen, checkboxes, Start disabled until checked):
- Kiln rested at least 8 h, lid closed, empty or with the usual furniture
  only (no ware: ware adds mass and a gradient).
- Time: about 2-3 h for 10a. The STEP autotune tail adds 30-50 min.
- Optional room thermometer for the ambient check.
- Optional reference: a type K handheld probe long enough to reach the zone
  TC tip through the peephole, or an IR pyrometer. Set emissivity 0.90
  (refractory), aim at the wall nearest the TC, never through glass.
- Peephole access. Heat-resistant gloves. Do not look into the peephole
  without IR/shade #3+ eyewear above about 500 C. The run stays at or below
  300 C, but the sides, lid and peephole plug are hot.
- This is a firing. Stay nearby. Closing the tab does not stop it: use Stop.

**During each phase:**
- Phase name, elapsed/remaining, live readings for each TC, the current
  slope, and a big Stop button.
- Prompt N seconds ahead, then "Take reading now" with a 60 s window
  countdown on both the LCD and the web.
- Entry is a number field (LCD `ui_num_pad`) with a Skip button.
- A late entry is timestamped and used at its true time. An entry that
  misses the window is discarded with a message.

**At the end**, a results table per sensor:
- **Columns:** old value, new value, the method used (cross-fit /
  reference / default / bound), the confidence, and Accept/Reject.
- **Sanity bounds** (outside the bounds means Reject only, with the reason):
  - tau 5-600 s, and tau ≤ `L_app`;
  - offset ±15 C;
  - an offset change of more than 5 C from the stored value asks a second
    confirmation.
- **Advice text:** cone witness for the high-temperature offset after the
  first firing.

**Skip paths:**
- Every prompt has Skip.
- P2/P3 can be skipped (no offset beyond ambient).
- The whole step 10a is skippable. The wizard then records `deliberately_off`
  with the default tau, and readiness says "TC lag default, not measured".

**LCD (320x480, no scroll):**
- **Prompt screen:** title, phase line, countdown, up to 4 TC rows (3 zones +
  Pico), entry, Skip, Stop.
- **Results screen:** 4 rows × (old/new/accept toggle), plus Save and
  Discard.
- More rows go on a second page, never a scroll.

**Auth and mode gate:**
- Start, reading entry and Accept need a web admin login. The LCD needs the
  admin level, as for other commissioning writes.
- Viewing a run is dashboard-level.
- The cal run counts as running for the mode gate. Zone, relay and safety
  writes get 409 during it, including its own results. Accept is only
  possible after P3 ends with the relays off.
- The Pico write then waits for the write window (the existing retry
  banner).

## Safety

- Existing guards are unchanged:
  - ESP side: thermal guard, ramp lock, S8 rate.
  - Pico side: abs_max, S6, S9 and the rest.
- A guard trip aborts the run, and nothing is offered for Accept.
- `T_hi` is bounded as above. The engine aborts at `T_hi + 15` C.
- The P1 fixed duty is capped by the zone `max_ramp_c_per_hr` when that is
  set.
- On the bench, heat runs are pre-authorized. For real users this is a
  firing with the usual obligations: presence warning, E-stop, the "does
  not stop on tab close" text.
- Compensated tau must never feed the Pico guards: the Pico keeps raw
  readings (owner question 3).

## Test plan (host sim: plant + sheath model)

- **Sim inputs:**
  - FOPDT kiln: tau_k ∈ {1500, 4000} s, L ∈ {30, 120} s.
  - First-order sheath: tau_s ∈ {0, 20, 60, 120, 240} s.
  - Pico tip: bare (tau 3 s) or same tube.
  - Gaussian TC noise σ 0.25 C plus 0.0078 C quantization.
  - True offsets ∈ {-10, 0, +8} C.
  - Reference readings with σ ∈ {1, 3} C, 0-5 readings.
- **Pass criteria:**
  - Cross-fit (bare partner, ramp 150 C/h or faster): tau within ±15 % or
    ±10 s, whichever is larger.
  - Same-tube partner: reports "matched, not absolute", never a tau.
  - Reference ramp: tau within ±30 % when the lag is at least 3σ, otherwise
    NOT_IDENTIFIABLE. It is never a number in that case: negative-test this.
  - Hold offset with 3+ reference readings at σ 1 C: within ±1 C.
  - Ambient offset with a room thermometer: within ±0.5 C (sim reference σ
    0.3 C).
  - Single sensor: the estimator returns only the bound, and `L_app - tau_s`
    retune gains are within 10 % of gains fitted on the unsheathed plant.
  - Unsteady hold (slope 0.2 C/min or more): the offset is refused.
- **Mutation tests** (`tools/negtest.ps1`, kilnfw-host ExpectPattern):
  - drop the bound check;
  - drop the NOT_IDENTIFIABLE branch;
  - swap the hold/ramp difference sign.

## Open questions (owner)

1. Default tau for single-sensor zones. **Recommend** 60 s, editable in the
   manual step.
2. Is the Pico TC bare or sheathed on real kilns? **Recommend** asking at
   commissioning (radio card). Only a bare or thin Pico tip gives a
   pyrometer-free absolute tau.
3. Does tau compensation feed the PID, display only, or the guards?
   **Recommend** PID plus display on the ESP. Pico guards stay raw, so
   protection never depends on a fitted value.
4. Cal run `T_hi` 300 C cap. **Recommend** 300 C: enough power margin for a
   fast ramp, below red heat (no eyewear issue), and short.
5. Should a pyrometer reading ever be auto-stored as an offset?
   **Recommend** no: show it as a sanity check only, because emissivity
   error exceeds the offsets being measured.
