# On/off zone bench-readiness pack — 2026-09-08

Step 9 (real device, owner present) is the only unexercised step of
`docs/ON_OFF_ZONE_PLAN.md`. Code-complete through relay actuation:
`d58492c9` (typing + guard exclusions), `3d740f78` (trigger core),
`dd1d6ada` (rule storage), `b46c120c`/`172e3081` (UI), `bf1db47f`
(actuation). This pack is the checklist for that one bench session — slots
into `docs/audits/one_firing_bench_plan_2026-09-07.md` (see §6), not a
second session.

All commit hashes below verified with `git cat-file -e <hash>^{commit}`.

---

## 1. Pre-flight checklist — run BEFORE energising anything

Configure via **either** the setup wizard step 4 (`stepBody4` in
`firmware/KilnFW/App/drivers/http/setup_wizard_page.html`) **or** the zones
page (`/settings/zones`, `firmware/KilnFW/App/drivers/http/zones_page.html`)
— both write the same `POST /api/zones` fields.

- [ ] **Zone type.** Set `zone_type = 1` (`ZONE_TYPE_ON_OFF`) for the target
      zone. Selecting "On-off device" in the zones page hides PID gains,
      coupling row, and autotune controls for that zone (`zones_page.html`
      `.heaterOnly` class toggle).
- [ ] **Fail-safe state.** Set `failsafe_state` — **false/OFF is the
      default** (`zone_cfg_t.failsafe_state`, `zones_config_json.h:790`,
      "0 = OFF (default)"). Leave it OFF for the dry-contact test: OFF is
      the safe default for any device whose real-world effect is unknown,
      and a vent/fan left OFF on abort is a non-event, while a device
      accidentally left ON on abort is the actual failure this feature
      exists to prevent. Only set it true for a device whose designed
      fail-safe really is energised-on (rare; document why if you do).
- [ ] **Hysteresis.** Set `hyst_c` (0.5–25 °C range; 0 = firmware default
      2.0 °C, `zones_http_post_parse.c:235-241`). This is what prevents
      relay chatter at the threshold — do not leave it at an aggressively
      small value for the first test.
- [ ] **Min on/off holds.** Set `min_on_s` and `min_off_s` (1–3600 s each;
      0 = firmware default 30 s, `zones_http_post_parse.c:244-274`).
- [ ] **Attach a rule to a profile segment.** Rules are NOT a separate
      endpoint — they ride inside `POST /api/profile`'s body as
      `on_off_rules[]`, one entry per `(segment_index, zone_index)` pair
      (`profiles_types.h:126-146`, `profiles_edit_http.c:196-272`). Each
      rule needs: `zone_index` (must reference a zone already typed
      ON_OFF), `segment_index`, phase mask (RAMP/DWELL), direction mask
      (HEATING/COOLING/FLAT), an optional temp comparison
      (`temp_source`/`temp_cmp`/`temp_threshold_c`), and optional
      `time_start_s`/`time_stop_s` inside the segment. Confirm the profile
      saved correctly by re-fetching it (`GET /api/profile?...`) and check
      `on_off_rules` is present and non-empty before trusting it at the
      bench.
- [ ] **Confirm the write landed:** re-read `GET /api/zones` and check
      `zone_type`, `failsafe_state`, `hyst_c`, `min_on_s`, `min_off_s` on
      the target zone match what was just set (`zones_http_get.c:356-368`).
- [ ] **max_simultaneous_relays check:** if other zones are also armed to
      heat this session, confirm `max_simultaneous_relays`
      (`GET /api/zones`, same field the zones page shows) is not already
      exceeded by heater zones alone before adding the on/off zone's relay
      into the mix — see §2 for what happens if it is.

## 2. Dry-contact test procedure (no heat applied)

Goal: prove the relay follows trigger logic with nothing dangerous
attached. Use dry contacts (an LED+resistor jig, a multimeter on the relay
terminals, or a spare contactor coil with no load side wired) — **not** a
real heating/venting load for this first pass.

1. **Configure** per §1 above. Use a short segment (e.g. a 2-minute dwell)
   with a rule that's easy to force both ways — e.g. `temp_cmp = ABOVE`
   with a threshold near current ambient, so the relay toggles as the
   already-live zone temperature drifts across it, or a phase-only rule
   (any temp) so it fires as soon as the segment's phase/direction match.
2. **max_simultaneous_relays gate:** confirm the profile actually starts.
   `profile_executor_run.c:462-482` and `profile_executor.c:1055-1068`
   count on/off zones against the same cap as heater relays — a run
   **refuses to start** if on/off zones alone (or combined with heaters
   already wanting on) exceed `max_simultaneous_relays`; on/off zones are
   suppressed last when the cap is tight (`profile_executor.c:1055` comment).
   If the run refuses, check the error names `max_simultaneous_relays (%u)
   already reached` (`profile_executor.c:1380`) before assuming a config bug.
3. **Arm** the run (`profiles_start`/dashboard "Start"). Confirm via
   `GET /api/status` that the run is `RUNNING` and the target segment is
   active.
4. **Observe the relay** two ways in parallel:
   - `GET /api/status` → `"relays":[{"relay":N,"on":bool}]`
     (`dashboard_status_http.c:248-251`) — poll this for the ground truth
     of actual relay state.
   - `/diagnostics` page (`diagnostics_http.c:1440-1441`) for the
     human-readable view during the session.
5. **Force the rule's condition** (e.g. nudge the zone's actual/measured
   temperature past the threshold with a heat gun near the TC, or wait out
   the phase/direction window) and confirm the relay follows within one
   control tick.
6. **Verify min-on hold:** immediately after the relay turns ON, try to
   force the OFF condition (cross back over threshold minus hysteresis).
   Confirm the relay stays ON until `min_on_s` has elapsed since it turned
   on, not before.
7. **Verify min-off hold:** symmetric — once OFF, force the ON condition
   again and confirm it stays OFF until `min_off_s` has elapsed.
8. **Verify hysteresis:** hover the forcing input within `hyst_c` of the
   threshold and confirm the relay does NOT chatter — it should only
   switch when the reading clears threshold ± hyst_c in the direction that
   matters.
9. **Abort mid-ON:** with the relay commanded ON, hit Stop/E-stop
   (whichever the session is using) and confirm via `/api/status` that the
   relay drops to the configured `failsafe_state` (OFF, per §1) within one
   tick, and stays there — do not clear the abort and expect it to
   re-energise from memory.
10. **Repeat abort during PAUSE** if the run supports pause: confirm the
    relay's behavior matches whichever `failsafe_on_pause` setting is
    configured (holds last commanded state if false, goes fail-safe if
    true — `on_off_trigger_decide.h`'s precedence-3 comment).

## 3. Predicted behaviour at each step (so a deviation is obvious)

| Step | Expected | Why |
|---|---|---|
| Config save | `zone_type=1` accepted; PID gains, coupling row, autotune controls hidden for that zone in the zones page | `zones_page.html:830` zoneType hint + `.heaterOnly` toggling |
| Autotune attempted on the zone (should NOT be attempted, but if someone tries) | Refused immediately with `"zone %u is an on/off device, not a heater — autotune has nothing to identify"` | `autotune_engine.c` (`zone_is_on_off()` check, ahead of the `thermo_mask==0` check — same file/shape per `ON_OFF_ZONE_PLAN.md` line 97-98) |
| Coupling matrix | Target zone's row AND column both read zero, at every `GET`, even after an autotune coupling pass elsewhere | `zones_config_accessors.c:481-487` zeroes both at read time, "belt and braces" |
| Guards 1 (heating-failed), 2 (wrong-direction), 3 (runaway), 4 (drift), 9 (cross-zone) | **Never trip on this zone**, even with duty effectively 1.0 for hours and a flat or falling reading — this was the design's central finding (a working vent looks exactly like guard 1's trip signature) | `thermal_guard.h:190-215`; negative-tested in `test_thermal_guard.c` per the plan's step-1 entry |
| Guards 5 (max/min temp), 6 (sensor validity), 7 (frozen sensor) | **Still active** if the zone has a TC — these protect the sensor/chamber regardless of what the relay drives | `thermal_guard.h:205` comment, explicit carve-out |
| Relay ON/OFF transitions | Only at threshold ± `hyst_c`, and only after the finishing side has held `min_on_s`/`min_off_s` | `on_off_trigger_decide.c` precedence 4/5 |
| Abort (any source: safety trip, FAULTED, halt, abort, run stopped) | Relay drops to `failsafe_state` (OFF by default) on the very next tick, unconditionally — precedence level 1, overrides everything else including an in-progress min-on hold | `on_off_trigger_decide.h` precedence-1 comment: "the caller ORs together every source that means 'this zone must go to its fail-safe state right now'" |
| PAUSE without `failsafe_on_pause` | Relay **holds** its last commanded state (does not go fail-safe) | `test_profile_executor_prestart.c:7374-7390`, explicitly tested |
| PAUSE with `failsafe_on_pause=true` | Relay goes fail-safe, same as a full abort | `test_profile_executor_prestart.c:7340` case row |
| `relay_cycles` accounting | The on/off zone's relay toggles are counted the same way a heater's PWM edges are — `profile_executor.c:1392-1408`'s "Contact-cycle accounting" comment explicitly notes on/off zones flow through `relay_cycles_add()` unchanged | `profile_executor.c:1408` |

## 4. Failure signatures to watch for

Drawn from this project's actual bug history — do not wait for a crash,
watch for these specific shapes:

- **Relay chattering at the threshold.** If it toggles multiple times per
  minute while the reading sits near `temp_threshold_c`, hysteresis is not
  being applied (or `hyst_c` landed as 0 without picking up the 2.0 °C
  default) — check `GET /api/zones` for the actual stored `hyst_c` before
  assuming a logic bug.
- **Device left energised after abort.** The relay stays ON after a
  Stop/E-stop. The negative test for this (`test_profile_executor_prestart.c`'s
  failsafe-on-every-ending-path cases) has run in host tests only — it has
  **never run against real hardware I/O**. This is the single highest-value
  observation of the whole session; do not skip step 9 of §2.
- **A guard trips on the on/off zone anyway** (most likely guard 1,
  heating-failed, given a vent-shaped duty-1.0-for-hours signature). This
  means the `on_off_zone`/`peer_is_on_off` exclusion isn't reaching the
  live guard evaluation — check whether the zone's `zone_type` actually
  persisted (re-read `GET /api/zones`) before assuming a guard-code defect;
  a config write that silently failed to commit would look identical.
- **Relay cycles mis-attributed.** After the session, check
  `GET /api/status`'s `relay_cycles`/`relay_life` array — the on/off zone's
  relay slot should show a cycle count matching the number of ON/OFF
  transitions actually observed in step 2's log, not zero (would mean the
  accounting path silently excluded it) and not double-counted against a
  neighbouring heater relay sharing a physical channel.
- **This project's "reset one side of a pair" bug class.** Two related
  pieces of state that should move together but don't: e.g. `quasi_dwell`
  (feature-local, per `on_off_trigger_decide.h`'s top comment) drifting out
  of sync with `s_exec.dwelling` after a segment change, or `commanded_on`
  surviving a resume-after-power-cycle when the plan says it must reset to
  fail-safe-shaped false. Confirm a resume (if exercised) actually reruns
  `on_off_trigger_state_reset()` rather than resuming last-known relay
  state.

## 5. What CANNOT be tested without the real device

- **Nothing about the trigger/hysteresis/hold logic itself** — that's
  exactly what §2's dry-contact test proves without a real load.
- **Actual thermal effect of the device** (does the vent actually cool the
  chamber, does the fan actually do what the profile author intended) —
  needs the real device physically wired, which is step 8 in the plan and
  explicitly out of scope for this dry-contact pass.
- **Load-side electrical behaviour** (inrush, contact wear under real load
  current, whether the relay's contact rating suits the device) — needs
  the real device on the load side; a dry contact/LED jig proves the
  control side only.
- **Owner must physically have present for THIS session:** the target
  relay's dry-contact side accessible for observation (multimeter or LED
  jig — no real load yet), the profile/rule already configured per §1
  (do this before the bench session to save owner time), and a way to
  force the rule's trigger condition (heat gun near a TC, or patience for
  a phase/direction-only rule).

## 6. Slotting into the existing one-firing bench plan

`docs/audits/one_firing_bench_plan_2026-09-07.md` is a separate ~13-item,
3-4 hour guard/CT session. This on/off dry-contact pass:

- **Best slot: Stage 0 (bench, cold, before ignition)**, alongside that
  plan's steps 1-6 (S6a, S7, CT step 0/2) — the dry-contact test applies no
  heat and needs no live firing, so it can run entirely before "Stage 1 —
  heat-up" begins. Do it after step 6 (CT install flags set) and before
  step 7 (S3 stuck-on, which needs a real relay energised under guard
  scrutiny) so the on/off zone's config is stable before any guard
  provocation touches the same relay bank.
- **No conflict with S1/S3/S6b/S8/S10/S11/S13/S9** — those all provoke
  guards on **heater** zones. The on/off zone's whole point is that guards
  1/2/3/4/9 must NOT fire on it, so running the dry-contact test alongside
  those steps is safe as long as the on/off zone's relay is on a different
  physical channel than whichever relay S3 (stuck-on) forces — confirm the
  channel assignments don't overlap before combining them in one session.
- **Does conflict with:** nothing structurally, but do not run the
  dry-contact abort test (§2 step 9) at the same moment as the
  bench plan's S9 (`TRIP_INEFFECTIVE`, stage 4, irreversible) — S9
  deliberately bypasses K4, and observing the on/off zone's fail-safe
  response is only meaningful if the safety chain is behaving normally.
  Sequence the on/off dry-contact test entirely before S9's bypass jig
  work begins.
- **Time cost:** ~20-30 minutes added to the existing 3-4 hour estimate —
  it needs no heat and no cooldown, only configuration + a handful of
  forced transitions with a stopwatch for the min-on/min-off checks.
