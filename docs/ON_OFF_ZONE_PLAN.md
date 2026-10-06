# On/Off Device Zones — plan

> **Spare-relay binding (2026-10-04):** an on/off device may bind a relay no zone
> uses, without a zone slot, via a separate aux-outputs store (landed host-tested; not bench-verified). See
> `docs/SPARE_RELAY_ONOFF_PLAN.md`; this plan still owns the evaluator and rules it reuses.

> **Status:** step 1 (zone typing + safety exclusions) DONE, 2026-09-07,
> `d58492c9`. Steps 4 and 7 (trigger evaluation core: quasi_dwell classifier
> + precedence-ordered decision function) DONE together, 2026-09-07,
> `3d740f78`. Step 6 (zones page UI for zone type + fail-safe) DONE
> 2026-09-08, `172e3081` -- see its row below. Step 5 (profile-side on/off
> rule storage: schema v3->v4, validation, HTTP API, export/import,
> `profile_resolve_on_off_rule()` wired report-only into the trigger core)
> DONE 2026-09-08, `dd1d6ada` -- see its row below. Step 8 (actual relay
> actuation) DONE 2026-09-08 (host-tested only) -- see its row below,
> **UNEXERCISED ON HARDWARE**: no on/off zone has ever actuated a physical
> relay. Step 9 (bench firing, owner present, dry contacts only) remains
> open and is the only way to close that gap -- **it is now the only open
> item in this plan.** Steps 2 and 3 are DONE, folded into step 1's commit
> rather than landing as separate ones: `zone_is_on_off()`/`zone_needs_
> ceiling()` (`persist/zones_config_accessors.c`), the ramp-lock/lag/
> feasibility/firing-stats/cross-zone-guard-9/coupling-row-column exclusions,
> and guard 5/6/7-only gating via `thermal_guard_input_t.on_off_zone` all
> verified present in code 2026-09-09 -- there never was a separate
> `on_off_guard_tick()` because `thermal_guard_tick()`'s own guard table
> already branches on `on_off_zone` per-guard, which covers the same ground.
> **A genuine gap was found and fixed 2026-09-09, not merely a stale status
> marker**: `escalate_guard_trip()`'s per-zone "every active zone faulted ->
> PROFILE_EXEC_FAULTED" aggregation (`profile_executor_relay_io.c`) counted
> on/off zones on both sides of the check, contrary to this doc's own
> "Executor watchdog inputs" row (sec 1) -- a run whose only unfaulted zone
> was a vent read as "still alive" instead of faulting. Fixed to skip
> `zone_is_on_off()` zones entirely in that loop; negative-tested by hand
> (reverted to the old unconditional loop, confirmed RED via the new
> `test_escalate_guard_trip_on_off_zone_excluded_from_all_faulted()`, restored,
> confirmed GREEN, `git diff` clean). **Opened:** 2026-09-07.
> Owner request, verbatim: *"add a feature that a zone may instead of being a
> heater it can be a on off device. this should be an option to set in a profile
> at a specific part of a ramp or dwell. for this if the dwell time if
> significantly accumulateing then the dwel should be considered on. give options
> for turning it on/off baised on ramp, direction, temp, time think about how our
> dynamic changes in profile effect this."*

> **OWNER DECISIONS (2026-09-07), settling two questions this plan left
> open:**
> 1. **"Extend the existing mechanism"** — build on `PROFILE_SEG_KIND_RELAY_IO`
>    rather than a parallel path, for whatever part of the feature that
>    mechanism can genuinely cover. Investigated for step 1: RELAY_IO is a
>    one-shot, non-zone timeline event (`profile_executor.c`'s segment-
>    stepping fires it once per segment entry/exit; it never appears in
>    `zone_mask`, is never walked by a guard, and holds no per-tick state) --
>    see this doc's own "Already exists, and is not this feature" section
>    above, confirmed unchanged by inspecting `profile_executor.c:553`,
>    `profile_executor_run.c:281,710` and `profile_executor_start.c:337,360`.
>    Step 1 (zone typing) has no RELAY_IO analogue to extend at all -- a
>    `zone_type` field and the guard/ramp-lock/coupling/autotune exclusions it
>    drives are necessarily a new, separate mechanism (RELAY_IO does not
>    represent zones or run per-tick). The owner's instruction is honored by
>    NOT adding any parallel path either: the schema is a tail-append onto
>    the existing `zone_cfg_t` (the same struct every other per-zone knob
>    lives on), and the guard exclusions extend `thermal_guard_input_t` (the
>    existing per-tick guard contract) with two new fields rather than a
>    second guard-evaluation function. Later steps (5, 7: profile-level
>    trigger rules) are where RELAY_IO's actual one-shot machinery may have
>    real reuse potential (e.g. a rule's "no rule for this segment -> OFF"
>    fallback looks structurally similar to RELAY_IO's own segment-boundary
>    firing) and should be evaluated again when that step is implemented.
> 2. **Fail-safe default OFF, per-zone confirm-gated opt-in to ON.** Encoded
>    directly in the schema (`zone_cfg_t::failsafe_state`, 0 = OFF, migration
>    and zero-init default) -- see §5 and §7 below, now implemented for step
>    1. The confirm-gate itself is UI (step 6), not yet built.

**Interpretation (confirm or correct before step 1).** A zone's relay drives
something that is not a heating element — vent, damper, fan, blower, water feed
— switched ON/OFF rather than PID-modulated. A profile schedules that switching
at points inside a ramp or dwell segment. "Dwell significantly accumulating"
= a ramp segment that is stalled/lagging and piling up hold time should count
as *dwell* for triggering purposes, not as *ramp*. Triggers on four axes: ramp
(which segment/phase), direction (heating vs cooling), temperature, time.

**Already exists, and is not this feature.** `PROFILE_SEG_KIND_RELAY_IO`
(`persist/profiles_types.h`) already lets a *segment* command one relay or IO
line, blocking or non-blocking, with `io_leave_on_at_end`. That is a
**timeline event on a non-zone output**. This feature is different in three
ways and must not be folded into it: (a) it is *state-driven*, re-evaluated
every tick against temperature/direction/lag, not fired once at a segment
boundary; (b) the output is a **zone** — it has a `relay_mask`, appears in
`zone_mask`, is counted by the load cap, and is walked by every guard loop;
(c) it must survive a stalled ramp, a mid-run edit and a resume. Where
RELAY_IO suffices (a one-shot "open the damper at the start of segment 4"),
tell the user to use RELAY_IO; this feature is for "vent whenever we are
above 600 °C and cooling."

---

## 1. Zone typing — the safety core

New per-zone field `zone_type`: `ZONE_TYPE_HEATER = 0` (default, every
migrated zone), `ZONE_TYPE_ON_OFF = 1`. It is **not** a new
`zone_control_mode_t` value (`OFF=0/BANGBANG=1/PID=2/PID_FUZZY=3`,
`persist/zones_config_accessors.h:891`). Control mode answers "how is duty
computed"; zone type answers "is this thing a heat source at all". Overloading
mode would make every `mode >= PID` test in the tree silently correct-looking
and semantically wrong, and would make the coupling matrix's meaning depend on
a control-mode field it currently never reads.

**One rule governs everything below: an on/off zone is never a heat source and
never a heat *victim*. It contributes nothing to the thermal model and no
thermal-model conclusion may be drawn from it.**

| Consumer | File | For `ZONE_TYPE_ON_OFF` |
|---|---|---|
| PID tick | `control/profile_executor_pid_tick.c`, `control/pid.c` | Not called. `z->duty` is forced to 0.0 or 1.0 by the trigger evaluator; no integrator, no fuzzy gains. `pid_reset()` on every type change so a stale integral cannot resurface. |
| Feedforward | `control/profile_executor_feedforward.c` | Not called. `ff_hold`/`ff_climb`/`coupling_correction` all 0. |
| Coupling matrix | `control/zone_coupling_solve.c`, `zone_cfg_t::coupling_coeff` | **Row and column both forced to zero, at load and at solve time — belt and braces.** Column zero = "this zone injects no heat into anyone" (true: it is a fan). Row zero = "no neighbour's heat is corrected for on this zone" (it has no setpoint to correct). A non-zero row would put a fan into the Jacobi solve as an actuator with commandable duty and the solve would allocate real duty to it. Zeroing at *load* alone is not enough — the matrix is editable over the API and by autotune's coupling pass. Add a `_Static`-style assertion in the solve: refuse (log + treat as identity row) if any on/off zone has a non-zero row or column. |
| Autotune | `control/autotune_engine.c:978` refuses `thermo_mask == 0` | **Refuse at start, same shape, same place, before any heating** — `zone %u is an on/off device, not a heater — autotune has nothing to identify`. Also refuse in `autotune_engine_run_relay()` and in the coupling pass (`autotune_engine_coupling.c`): an on/off zone must never be *excited* as the perturbing zone nor *fitted* as the responding zone. `iter_tune`/`adaptive_tune` likewise skip it. |
| `thermal_guard` | `control/thermal_guard.c/.h` | Per-guard, below. Simplest correct implementation: the executor **does not call `thermal_guard_tick()` at all** for an on/off zone, and instead calls a new narrow `on_off_guard_tick()` covering only guards 5/6/7 (see below). Not calling it is safer than passing a "disable everything" flag, because a flag has to be threaded through every branch and a missed branch fails *open*. |
| Load cap | `control/profile_executor.c` ~line 980 | See §6. |
| Firing stats / IAE | `control/profile_executor_firing_stats.c` | Skip. An on/off zone has no `target_c` to have an error against; accumulating it would corrupt `iae_normalized` for the whole run. Report on-time seconds and switch count instead. |
| Relay life budget | `persist/relay_cycles.c` | **Unchanged, and important** — an on/off vent on a temperature trigger can cycle far more often than a 60 s-window heater. `relay_type`/rated-life accounting applies as-is; this is why §3 mandates min-on/min-off times. |
| Ramp-lock | `control/profile_executor.c:484` | **Excluded from the lock loop.** An on/off zone has no `actual_c` obligation; leaving it in means a zone with no thermocouple, or one sitting at ambient while the kiln climbs, freezes the shared setpoint forever. This is a *hard* requirement, not an optimisation. |
| Auto-stretch / lag | `control/profile_executor_ramp_assist.c` | Excluded (it is fed by the lock loop above). |
| Dwell credit | same | Excluded. Documented structurally unreachable anyway; do not add a second unreachable path. |
| Executor watchdog inputs | `control/profile_executor.c`, `run_state.c` | An on/off zone counts as `active` for "is the run alive", but **must not** count toward "every active zone faulted ⇒ `PROFILE_EXEC_FAULTED`". A run whose only surviving zone is a fan is not a running firing — it is a stuck run. Rule: `PROFILE_EXEC_FAULTED` when every active **heater** zone is faulted, regardless of on/off zone state. |
| Feasibility | `control/profile_feasibility.c` | Skip on/off zones entirely — no ramp rate is achievable or unachievable for a damper. |
| PC tools / MCP | `tools/PcTools/src/kilnctrl/` | `capability_preflight` and the tuning tools must refuse to tune, and must not report an on/off zone as "untuned". |

### Guard-by-guard

| # | Guard | On/off zone |
|---|---|---|
| 1 | `HEATING_FAILED` (stall) | **DISABLE. This is the false-trip found in this design.** Guard 1 arms when commanded duty ≥ `progress_duty_min` and trips if the measurement does not rise within `progress_window_s`. A correctly working vent commands duty 1.0 for hours and produces *no rise at all* — often a *fall*. Guard 1 as written fires a HEATING_FAILED on a perfectly healthy device, and because guard-1 trips are per-zone-faulting, the vent stops mid-firing exactly when it is needed. **Nothing about guard 1's own logic can distinguish this**: "duty high, temperature flat" is the guard's entire trip condition and is also the vent's normal operating signature. The only correct fix is to not run guard 1 on an on/off zone. The arrival-band and `expected_rate` relaxations do **not** rescue it. |
| 2 | `WRONG_DIRECTION` | **DISABLE**, same reason with the sign flipped: a vent's whole purpose is falling temperature while its relay is on. Guard 2 would trip immediately on the intended use case. |
| 3 | `RUNAWAY` (welded contact) | **DISABLE as written**; it infers a welded output from temperature *rising while commanded off*, which an on/off zone's channel cannot express. Replace with nothing on the ESP — the useful welded-contactor detection for this output is CT-based (S14/S15 on the Pico) or contactor feedback, both out of scope here. Record this as an accepted coverage gap in `docs/SAFETY_CASE.md`. |
| 4 | `DRIFT` | **DISABLE.** Drift is "sustained excursion from setpoint"; there is no setpoint. |
| 5 | `MAX_TEMP` | **KEEP, if the zone has a thermocouple.** A ceiling on a measured channel is meaningful whatever the relay drives, and it is the one guard that protects the *kiln*, not the control loop. Trip action for an on/off zone = drive to its fail-safe state (§5), and fault the run globally as today. |
| 6 | `MIN_TEMP` | **KEEP if TC present**, same reasoning. |
| 7 | `SENSOR_INVALID` | **KEEP if TC present**; if no TC is assigned, not applicable (§2). |
| 8 | `FROZEN` (value identity) | **KEEP if TC present.** Independent of duty. |
| 9 | `CROSS_ZONE` | **Exclude from both sides of every comparison.** An on/off zone's channel is not comparable to a heater channel — a vent zone reading 200 °C below its neighbours is the design working. Leaving it in produces a global trip on a healthy kiln. |
| `RELAY_STALLED` | guards 1/2 discriminator | Dead with 1/2 disabled. |
| **S8** (Pico rate guard, `SAFETY_TRIP_RATE = 9`) | `firmware/SaftyFW/src/safety_guards.*` | **No change, and do not try to make one.** S8 runs on the Pico's *own* safety thermocouples against `abs`-scoped config; it does not know about ESP zone indices or zone types, and per the standing rule the Pico's limits are never made tighter or more conditional than the ESP's. If a vent causes a legitimately fast *fall*, S8 is a rise guard and is unaffected. **Open question for the owner if a vent is ever fitted near a safety TC.** |

---

## 2. Does an on/off zone need a thermocouple?

**Optional, and the semantics must be settled explicitly, because two "zero
means" conventions collide here.**

- **With a TC** (recommended, and required for any temperature trigger):
  guards 5/6/7/8 stay live; `max_temp_c == 0` keeps today's meaning —
  *uncommissioned, refuse to start* (`control/profile_executor_run.c` ~line 330,
  `control/profile_executor_start.c:165`). Do **not** carve an exception:
  a channel that is measured must have a ceiling.
- **Without a TC** (`thermo_mask == 0`): legal *only* for
  `ZONE_TYPE_ON_OFF`, and only if the zone's trigger rules use no
  temperature axis. `profile_executor_run()` must refuse at start if a
  TC-less on/off zone is referenced by any rule with a temperature
  condition — checked at start, not at the tick, matching the existing
  refuse-before-heating discipline.
- **`max_temp_c == 0` for a TC-less on/off zone:** must **not** block the
  run. The "0 = uncommissioned, refuse" rule exists because an unmeasured
  ceiling on a *heater* is lethal; a TC-less fan has no measurement for a
  ceiling to apply to. Encode this as `zone_needs_ceiling(zi) = (zone_type ==
  HEATER) || (thermo_mask != 0)` — one predicate, one place, so the two
  call sites cannot drift. This is the only relaxation of the settled
  `max_temp_c == 0` semantics and it must be spelled out in
  `docs/SAFETY_CASE.md`.
- **Autotune prestart refusal:** the just-added `thermo_mask == 0` refusal
  (`autotune_engine.c:978`) already covers the TC-less case. Add the
  `zone_type` refusal *before* it so a TC-equipped on/off zone gets the
  accurate message rather than passing the TC check and failing later on a
  flat trace.

---

## 3. Trigger model

Per **(profile, segment, zone)** a rule. Storage lives with the **profile**,
not the zone: the same vent is used differently by a bisque and a glaze
firing. Zone config holds only *type*, *fail-safe state*, *hysteresis* and
*min on/off times* — the physical properties of the device.

```c
typedef struct {                 /* one per on/off zone per segment */
    uint8_t  zone_index;
    uint8_t  enable;             /* 0 = rule absent, device follows profile default (OFF) */
    uint8_t  phase_mask;         /* bit0 RAMP, bit1 DWELL — "which part of the segment" */
    uint8_t  direction_mask;     /* bit0 HEATING (target rising), bit1 COOLING, bit2 FLAT */
    uint8_t  temp_source;        /* 0 = none, 1 = measured (this zone's TC),
                                  * 2 = measured (named zone's TC), 3 = executor setpoint */
    uint8_t  temp_ref_zone;      /* for temp_source == 2 */
    uint8_t  temp_cmp;           /* 0 = none, 1 = ABOVE threshold, 2 = BELOW threshold */
    float    temp_threshold_c;
    uint16_t time_start_s;       /* offset into the segment at which the rule becomes eligible */
    uint16_t time_stop_s;        /* 0 = to end of segment; else eligible window ends here */
    uint8_t  invert;             /* device ON when conditions are FALSE (a "close the damper
                                  * while soaking" rule, without needing a second rule kind) */
} profile_on_off_rule_t;
```

**Composition and precedence — defined, not emergent.** Evaluated top-down
each tick; the first level that decides, wins:

1. **Fail-safe override.** Safety trip, `PROFILE_EXEC_FAULTED`, halt, abort,
   `relay_authority_zone_blocked()` — device goes to its configured fail-safe
   state (§5). Nothing below can override this.
2. **Guard 5/6 trip on this zone** — fail-safe state.
3. **Run not `RUNNING`** (IDLE / PAUSED / DONE) — fail-safe state on
   FAULTED/halt; on PAUSE, hold last commanded state *unless*
   `failsafe_on_pause` is set (default: go to fail-safe, matching how
   `io_segs_force_all_off()` treats PAUSE today).
4. **Minimum on/off dwell not yet satisfied** — hold current state. This sits
   *above* the rule evaluation so no rule can chatter the relay, and *below*
   the safety levels so safety is never delayed by it.
5. **Segment rule for this zone**, if `enable`. All configured axes are
   **ANDed**: phase ∧ direction ∧ temperature ∧ time window. An axis left at
   its "none/all" value is a tautology and drops out. `invert` negates the
   AND result.
6. **No rule for this segment** — device **OFF**. Not "hold last state":
   a profile author who deletes a rule expects the device to stop, and
   holding-last is how a vent gets left open across a whole glaze soak.

Only one rule per (segment, zone) — no rule stacking. Multiple simultaneous
conditions are what the AND is for; multiple *alternatives* are what multiple
segments are for. This is a deliberate simplification: OR-composition across
rules has no natural precedence and would need its own conflict policy.

**Hysteresis.** Two mechanisms, both mandatory:

- **Temperature hysteresis** `hyst_c`, default **2.0 °C**, range 0.5–25 °C,
  per zone. An ABOVE rule turns on at `threshold + hyst_c/2` and off at
  `threshold - hyst_c/2` (BELOW mirrored). Why 2.0: the K/S-type noise on
  this board's channels is well under 1 °C sample-to-sample, so 2 °C is
  several noise bands wide and cannot be crossed by noise, while being small
  enough that a 100 °C/hr ramp crosses it in 72 s — the operator does not
  perceive a lag. Anything below ~0.5 °C is inside the combine/calibration
  quantisation and *will* chatter.
- **Minimum on/off time** `min_on_s` / `min_off_s`, default **30 s** each.
  Hysteresis alone is not enough near a dwell setpoint, where the temperature
  sits *on* the threshold and PID ripple can straddle any band. 30 s bounds
  the worst case at 120 cycles/hr; at contactor rated life
  (`docs/RELAY_LIFE_BUDGET.md`) that is visible in the budget within one
  firing, so the UI must show projected cycles for an on/off zone.

---

## 4. The dwell-accumulation rule

**How dwell works today** (`control/profile_executor.c`, segment stepping,
~lines 484–760):

- `s_exec.dwelling` flips true only when `target_c == seg->target_c`, and
  `segment_elapsed_s` then counts the dwell.
- `segment_elapsed_s` and `target_c` advance **only when `lock_ok ||
  stretched_this_tick`**. `lock_ok` is false while any active heater zone is
  more than `EXEC_RAMP_LOCK_BAND_C(zi)` (default
  `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C = 25.0f`,
  `control/profile_executor.h:351` — **25 °C, not 3**) below the shared
  setpoint.
- So a lagging ramp **freezes**: the setpoint stops, the clock stops, and the
  kiln sits at a near-constant temperature for as long as it takes. That is
  the owner's "dwell time significantly accumulating" — and today
  `s_exec.dwelling` reads **false** throughout it. A DWELL-phase rule would
  never fire during the exact stall the owner wants it to cover.

**Definition.** Add an executor-computed `effective_dwell` used by *this
feature only* (never fed back into `dwelling`, firing stats, or credit — see
the reset-one-side bug class):

```
quasi_dwell ENTERS  when s_exec.ramp_lock_held has been continuously true
                    for >= ON_OFF_QUASI_DWELL_ENTER_S  = 120.0f seconds
quasi_dwell EXITS   when ramp_lock_held has been continuously false
                    for >= ON_OFF_QUASI_DWELL_EXIT_S   =  30.0f seconds
                    OR the segment index changes (hard reset)
effective_dwell = s_exec.dwelling || quasi_dwell
```

**Why 120 s enter.** `EXEC_SUSTAINED_LAG_S = 30.0f`
(`control/profile_executor_internal.h:1023`) is already the repo's answer to
"how long before a lag is real rather than settling noise". 120 s is 4× that:
long enough that no plausible sequence of noise, a load-cap-deferred window,
or a single PWM window landing badly can reach it, and short enough to be a
small fraction of any real ramp segment (hours). Directly reusing 30 s was
rejected — 30 s is calibrated for *warning an operator*, and acting on a relay
deserves a wider margin than lighting a status field.

**Why 30 s exit, and why it is asymmetric.** Exit is deliberately 4× *faster*
than entry, because the failure modes are asymmetric: entering late costs a
vent a couple of minutes; leaving late means a vent stays open into a genuine
ramp, fighting the elements and possibly *causing* the next lag. Asymmetric
thresholds with a 4:1 ratio give ≥ 150 s of total round-trip dead time, which
is far longer than the lock's own oscillation period.

**Why it cannot flap.** Three independent stops: (a) the two thresholds are
disjoint, so a single boundary crossing cannot toggle both; (b) the per-zone
`min_on_s`/`min_off_s` (§3 level 4) sits below this and bounds the relay
regardless of how the classifier behaves; (c) `quasi_dwell` is reset hard on
any segment change, so it can never leak across a boundary. The classifier is
recomputed from `ramp_lock_held` each tick and holds no state beyond the two
timers, which are the only new state this feature adds to `s_exec`.

**Reported, not inferred.** `effective_dwell`, `quasi_dwell` and the two
timers go into `/api/status` and the dashboard. An operator seeing a vent open
must be able to see *why* — "segment 3, quasi-dwell for 14 min (ramp-locked,
zone 2 lagging 31 °C)".

---

## 5. Dynamic profile changes

| Event | On/off behaviour |
|---|---|
| **Ramp-lock engages** | §4. Rules keyed to DWELL start firing after 120 s. Rules keyed to RAMP stop. |
| **Auto-stretch engages** | `stretched_this_tick` means the setpoint *is* advancing, so `ramp_lock_held` may still be true while real progress happens. **Decision:** quasi-dwell's timer is gated on `ramp_lock_held && !stretched_this_tick` — a stretched ramp is a ramp, slowly. Not doing this would classify every stretched segment as a dwell. |
| **Approach-rate cap** | Caps the *commanded* setpoint, not the phase. Direction is evaluated from `s_exec.target_rate_c_per_s` sign, which the cap preserves. No change. |
| **Dwell credit** | Documented structurally unreachable. On/off triggering reads `s_exec.dwelling`, which credit can only end *earlier*, never start later. No special handling; do not add code for it. |
| **Feedforward hold vs climb** | Irrelevant — an on/off zone runs no feedforward. But note: `zone_feedforward()` must not be called with an on/off zone's `ff_tau_s`/`k_dc`, which are meaningless and may be garbage from a type change. Zero them on type change. |
| **Setpoint edited mid-run** | Temperature rules read the *live* value each tick, so an edit takes effect immediately — correct. Direction is recomputed from the new rate. A rule whose threshold is now unreachable simply never fires; the UI must warn at save time, not silently. |
| **Profile edited mid-run (rule changed)** | Rules are re-read at the same point config reload already happens (`control/profile_executor_config_reload.c`). A rule removed mid-segment sends its device to OFF (precedence level 6), *subject to `min_on_s`*. |
| **Resume after power cycle** | The warm-start path (`profile_executor_run.c` ~line 680, `plan.entry_dwelling`) restores `dwelling` but there is no persisted `quasi_dwell` and there must not be — the lock state after a reboot is unknown. **Resume starts every on/off device in its fail-safe state and `quasi_dwell = false`,** then re-derives from the first tick's real conditions. Re-entry costs at most 120 s. Persisting it would be a reset-one-side bug: `ramp_lock_held` is rebuilt from scratch on resume while a restored timer would not be. |
| **Abort / halt** | Fail-safe state. |
| **Fault (`PROFILE_EXEC_FAULTED`)** | Fail-safe state. |
| **Safety trip / `relay_authority_zone_blocked()`** | Fail-safe state — **and here is the hard part.** |

**Fail-safe state is per zone, and its default is OFF.**

```c
uint8_t failsafe_state;   /* 0 = OFF (default), 1 = ON */
```

A vent's safe state is arguably ON (dump heat), which is the opposite of a
heater's. But **the default must stay OFF**, for the same reason
`io_leave_on_at_end` defaults to 0: a zero-initialised struct — a fresh save,
a partial form, a migrated v22 blob — must never leave a relay energised with
nothing owning it. An owner who wants a fail-safe-ON vent sets it explicitly,
and the UI must require a confirmation for that choice and show it on the
zones page and in diagnostics.

**Fail-safe ON has a real limit that must be stated to the owner:** on a
safety trip the Pico opens K4, which de-energises the whole heating chain. If
the vent's contactor is fed from the same interlocked supply, `failsafe_state
= ON` is a *request* the hardware cannot honour. **Question for the owner:
which supply feeds the intended on/off device?** If it is behind K4, fail-safe
ON is unachievable and the UI must say so rather than imply protection that
does not exist.

---

## 6. Relay authority and interlocks

- **All writes go through `apply_relay()`
  (`control/profile_executor_relay_io.c`) exactly as a heater does** — same
  `claimed_relay_mask` accumulation, same `relay_authority_zone_blocked()`
  evaluation every tick, same `kiln_io_owner` write. No new relay path, no
  direct `kiln_io_*` call. Six features in this repo have bypassed
  `kiln_io_owner` and every one produced a one-directional interlock; there
  is no reason for a seventh.
- `relay_authority_zone_blocked()` currently means "may this zone *heat*". For
  an on/off zone it means "may this zone *actuate*". **Keep the same gate**:
  it is the conservative choice, and a vent that stops when the safety
  processor is unhappy is defensible. The consequence — a fail-safe-ON vent
  cannot be commanded on while blocked — is the same limitation as above and
  must be documented in one place, not two.
- **`max_simultaneous_relays`: an on/off zone counts, and its suppression
  order is different.** It counts because the cap exists to bound *coil and
  supply current*, and a contactor coil draws the same whatever it switches.
  But the load-cap victim selection (`profile_executor.c` ~980) picks the zone
  with the least `deferred_on_ms`, and defers the denied on-time for later
  repayment — which is meaningless for an on/off device (there is no window to
  repay into, exactly the documented bang-bang gap). **Rule: on/off zones are
  chosen as load-cap victims LAST**, after every heater has been considered,
  and their denial is logged rather than deferred. Rationale: suppressing a
  vent to run an element is the wrong trade in a kiln that is already at its
  relay budget, and silently deferring it would make a vent look commanded-on
  while being off.
- If the cap is *reached* by on/off zones alone, that is a configuration
  error; refuse at run start rather than discovering it mid-firing.

---

## 7. Schema and storage

**Two separate schema surfaces; do them in separate steps.**

1. **Zones config — `ZONES_CFG_VERSION 22 → 23`**
   (`persist/zones_config_json.h:63`). Tail-append to `zone_cfg_t`, exactly
   the `relay_type` precedent (v19→v20, offset 208) and `progress_band_c`
   (v21→v22):
   ```c
   uint8_t zone_type;       /* ZONE_TYPE_HEATER = 0 (migration default) */
   uint8_t failsafe_state;  /* 0 = OFF (migration default) */
   float   hyst_c;          /* 0 -> 2.0f default at read */
   uint16_t min_on_s;       /* 0 -> 30 */
   uint16_t min_off_s;      /* 0 -> 30 */
   ```
   Freeze `zone_cfg_v22_t` byte-for-byte with its `_Static_assert` on the
   `progress_band_c` offset, add `convert_zone_v22()`, and extend
   `convert_versioned_blob_to_current()`. Every migration default is the
   value that reproduces today's behaviour, so a v22 board upgrading gains
   nothing but a zero.
   **Rollback hazard:** per `docs/UPDATE_PROTOCOL.md`, an `ota_rollback_esp()`
   past v23 makes the older firmware reject the blob and run on
   firmware-default PID gains. Same standing warning; read back
   `control_get_zones` after any rollback.

2. **Profiles — per-segment rules.** `profile_segment_t` is
   `PROFILE_MAX_SEGMENTS = 12` and already 20-odd bytes; adding an array of
   up to 5 rules inline multiplies the profile blob by ~4 and would blow the
   existing profile storage budget. **Instead: one `profile_on_off_rule_t`
   array per profile, `PROFILE_MAX_ON_OFF_RULES = 8`, each carrying its own
   `segment_index`.** Sparse, since most segments have no rule, and it keeps
   `profile_segment_t`'s layout — and the RELAY_IO fields — untouched.
   Version the profile blob with the same tail-append discipline.

3. **Step 1 status for the filesystem workstream:** landed 2026-09-07 through
   the existing NVS/serialiser path only, per the sequencing rule below --
   `zones_config_cfg_fs.c`/`dualwrite_window.c`/`cfg_fs_mount.c`/
   `pref_cfg_fs.c` were NOT touched. What that workstream's own cutover will
   need to carry once it reaches `zone_cfg_t`: five new fields at the struct's
   true tail (`zone_type`, `failsafe_state` — both `uint8_t`, `hyst_c` —
   `float`, `min_on_s`/`min_off_s` — both `uint16_t`), ZONES_CFG_VERSION now
   23, and a `zone_cfg_v22_t` frozen snapshot (220 bytes) already in
   `zones_config_json.h` if a file-format migration ever needs to read an
   old v22 blob directly instead of going through the NVS path's own
   `convert_versioned_blob_to_current()`.

   **`cfg` filesystem migration is in flight and must not be collided with.**
   `zones_config_cfg_fs.c`, `profiles_cfg_fs.c`, `dualwrite_window.c`,
   `cfg_fs_mount.c`, `pref_cfg_fs.c` and the format policy are all owned by
   another workstream right now (`docs/CONFIG_FILESYSTEM.md`). **Do not touch
   them.** Sequencing rule: land the v22→v23 zones bump and the profile rule
   array through the *existing* NVS/serialiser path only, and let the
   dual-write layer carry them for free — the dual-write window serialises
   whatever the current struct is. If the filesystem work lands its cutover
   first, this plan's schema step needs no change; if this lands first, the
   filesystem work picks up two extra fields in the same blob it already
   copies. **Neither ordering requires a merge, provided this feature never
   edits a `cfg_fs*` file.** Confirm with that workstream before step 5.

---

## 8. UI and API

- **Zones page** (`http/zones_page.html`, `http/zones_http_post_parse.c`):
  a **Zone type** select (Heater / On-off device) at the top of each zone
  card. Selecting On-off device **hides** PID gains, coupling row, autotune
  button, ramp/approach fields, and guard 1/2/3/4/9 thresholds — they are
  inert, and showing an inert field is how the fuzzy-mode-2 campaign went
  silently inert for weeks. Reveals: fail-safe state (with a confirm on ON),
  hysteresis, min on/off, projected cycles/hr.
- **Profiles page** (`http/profiles_page.html`): per segment, a compact row
  per on/off zone — `[zone] [phase ▾] [direction ▾] [temp ▾ ABOVE 600 °C]
  [from 0 s to end] [invert]`. Save-time validation rejects a temperature
  rule on a TC-less zone and warns on an unreachable threshold.
- **API:** `zone_type`/`failsafe_state`/`hyst_c`/`min_on_s`/`min_off_s` as
  ordinary form fields on `POST /api/zones/config`, echoed by `GET`. Rules go
  on the existing profile POST/GET as an indexed field family
  (`rule0_zone=3&rule0_phase=2&...`), matching how segments are already
  encoded. `/api/status` gains `effective_dwell`, `quasi_dwell`,
  `quasi_dwell_held_s`, and per on/off zone `commanded_on`, `on_time_s`,
  `switch_count`, `rule_reason` (short string).
- **LCD (480×320 landscape, no scroll, no new colours):** on/off zones render
  in the existing zone strip with the temperature slot replaced by
  `ON`/`OFF` and the duty bar replaced by a filled/empty block, reusing the
  relay-state colours already on the topbar. One extra line on the Home page
  when quasi-dwell is active: `SEG 3 HOLD 14m`. No new page.

---

## 9. Ordered steps

Riskiest last. Every step is independently shippable and leaves the tree
green.

| # | Step | Flash? | Reversible? | Test |
|---|---|---|---|---|
| 1 | **DONE 2026-09-07 (`d58492c9`).** `zone_type`/`failsafe_state`/`hyst_c`/`min_on_s`/`min_off_s` in `zone_cfg_t`, v22→v23, `zone_cfg_v22_t` frozen (closing the one missing `_Static_assert` review found), converters. Widened beyond a pure schema-only step per the task's own non-negotiables: guards 1/2/3/4/9 excluded for an on/off zone (`thermal_guard_input_t.on_off_zone`/`peer_is_on_off`), ramp-lock excludes on/off zones, coupling row/column zeroed at both read and write time (`zones_config_get/set_coupling`), autotune refuses an on/off zone at prestart (same shape as the `thermo_mask==0` refusal, checked first). Guards 5/6/7/8 and heater-zone behavior are unchanged (`on_off_zone` defaults false; full pre-existing host-test suite passes unmodified). | No — every existing zone stays `ZONE_TYPE_HEATER` (0); nothing on a live board changes until a zone is explicitly typed on/off, which no UI yet allows (step 6). Safe to flash whenever convenient. | Yes — pure tail-append | `test_zones_http.c` (schema round-trip both directions, v22-blob upgrade, `_Static_assert` offsets, defaults-on-zero, fail-safe-OFF-on-zero-init, coupling row/column zeroing), `test_thermal_guard.c` (guards 1/2/3/4/9 excluded, 5/6/7 kept, negative-tested), `test_autotune_engine_prestart.c` (refusal before thermo_mask check), `test_ramp_lock_onesided.c` (on/off zone never holds the lock) |
| 2 | Read-only predicates + exclusions: `zone_is_on_off()`, `zone_needs_ceiling()`, exclude on/off zones from ramp-lock, lag, feasibility, firing stats, cross-zone guard 9, coupling row/column zeroing. **Still no zone is typed on/off, so behaviour is bit-identical.** | No | Yes | Host tests asserting bit-identical executor output with all zones HEATER; negative test forcing a zone on/off and asserting the lock loop skips it (break the real predicate, restore by hand) |
| 3 | Guard gating: `on_off_guard_tick()` (5/6/7/8 only), autotune/iter_tune/adaptive_tune refusals, `PROFILE_EXEC_FAULTED` counts heaters only. | Yes | Yes | `test_thermal_guard*`: assert guard 1 **cannot** trip an on/off zone under duty 1.0 + flat temperature for 10× `progress_window_s` — this is the false-trip regression test and it must fail before the fix |
| 4+7 | **DONE 2026-09-07 (`3d740f78`).** Trigger evaluation core, done together as one pure module: `control/on_off_trigger_decide.h/.c` (`on_off_trigger_decide()`), following the `link_watchdog_decide.c` extraction pattern -- no FreeRTOS, no locks, no I/O. Precedence levels 1-6 all implemented (fail-safe override, guard 5/6 trip, run-state incl. PAUSE hold-last, min\_on\_s/min\_off\_s hold, the rule's 4-axis AND with `invert`, no-rule-\>OFF). `quasi_dwell` classifier (120 s enter / 30 s exit, hard reset on segment change) lives in a new **feature-local** `on_off_trigger_state_t` (`zone_runtime_t.on_off_trigger_state`, reset in `profile_executor_run()` so a resume also re-zeros it per sec 5) -- never read by or derived from `s_exec.dwelling`/`ramp_lock_held` themselves. Wired into `profile_executor.c`'s per-zone tick as **report-only** (computed every tick for an on/off zone, right after that zone's `thermal_guard_tick()`; verdict is not read by anything else and no relay is touched) -- new accessors `zones_config_get_failsafe_state/_hyst_c/_min_on_s/_min_off_s` added to `zones_config_accessors.c/.h` (schema fields already existed from step 1; step 5's profile-rule storage does not exist yet, so the wired-in `rule.enable` is hardcoded `false`, i.e. every on/off zone currently reports precedence level 6 unless a higher level overrides it -- honest given step 5 is still open, not a placeholder pretending otherwise). Producer check `check_on_off_trigger_input_producers.ps1` added and negative-tested (removing the `.current_phase_is_dwell` producer in `profile_executor.c` fails the check; restored, sha256 byte-identical to before). Test: `test_on_off_trigger_decide.c`, 1 module test file, every precedence level + all 4 axes individually + AND + `invert` + hysteresis both directions + a quantized-noise chatter sweep (0 switches) + the hold blocking a flip + `quasi_dwell` reset on segment change; hysteresis negative-tested by hand (forcing `half = 0.0f` in `axis_temp()` fails 6 checks including the chatter sweep, restored, sha256 byte-identical). | No -- `rule.enable` is hardcoded false in production, so this step's addition to a running board's behavior is nothing observable: the report path computes and discards a verdict, no relay/duty/status output changes. Safe to flash whenever convenient. | Yes | `test_on_off_trigger_decide.c` (all of the above), `check_on_off_trigger_input_producers.ps1` |
| 5 | **DONE 2026-09-08 (`dd1d6ada`).** Profile-side rule storage: `profile_on_off_rule_t` (`profiles_types.h`), a sparse `PROFILE_MAX_ON_OFF_RULES=8` array field-matching `on_off_trigger_decide.h`'s `on_off_trigger_rule_t` plus `segment_index`/`zone_index` keys. `PROFILE_VERSION` 3→4 (`profiles_http.c`), `profile_t_v3`/`profile_persisted_v3_t` frozen with `_Static_assert`s on size (268) and both wrapper offsets, `convert_profile_v3()` migration (default `on_off_rule_count=0`). `validate_on_off_rules()` (called from `profiles_http_save()`, shared HTTP/UART-bridge entry point) rejects a nonexistent `segment_index` or a `zone_index` not typed `ZONE_TYPE_ON_OFF` -- negative-tested by hand (see report). HTTP API: `rule%u_*` indexed POST fields (`profiles_edit_http.c`), GET echo (`profiles_catalog_http.c`), export/import `"on_off_rules"` JSON array with the export document's own version bumped 1→2 (`profiles_export_http.c`) -- old export imports as rule-count 0, a new export imported by old firmware has the unknown key silently ignored. `profile_resolve_on_off_rule()` (new pure lookup, `profile_executor.c`/`profile_executor_internal.h`) replaces the step-4/7 hardcoded `rule.enable=false` stub, so `on_off_trigger_decide()` now evaluates real, persisted rules -- **still report-only, no relay written** (plan step 8 unchanged). A rules-free profile is proven byte-identical (`assert_profiles_equal()` now covers the rule tail on every existing migration test, plus a dedicated rules-free test). | No -- storage/validation/API only; the executor consumes real rules into the still-report-only trigger core, nothing observable changes on a running board (no relay write happens either before or after this step). Safe to flash whenever convenient. | Yes | `test_profiles_http.c` (v3→v4 migration, NVS round-trip with a rule, validation rejections incl. the HEATER-zone negative test), `test_profile_export_import.c` (export/import both directions), `test_profile_executor_prestart.c` (`profile_resolve_on_off_rule()`: exact match, wrong zone/segment, disabled slot, reserved `temp_source`, rules-free profile) |
| 5b | **DONE 2026-09-20.** Web UI for step 5's wire format: `profiles_page.html` had no on/off rule editor at all until now -- it only echoed `on_off_rules` back unchanged on save, so no operator could ever populate a rule from the browser (the `rule%u_*` fields and JSON keys existed server-side, but nothing sent them). Added an "On/off devices" section: per-rule zone picker (ON_OFF zones only, from `GET /api/zones`'s `zone_type`, with an empty-state message and a link to the zones page when none exist), segment picker, phase/direction checkboxes, temperature source/comparison/threshold (Celsius-only input, matching `rampFieldsHtml()`'s existing convention -- no `kcUnit` round trip, since `app.js` has no `fromDisplay()`), time window, enable/invert, add/remove controls, a live per-rule summary line (`ooUpdateSummary()`, the one formatter reused by both the editor and the read-only preview so the two can't drift apart per the "reset one side of a pair" class), and per-field info icons matching `zones_page.html`'s disclosure pattern. Wired into save (`ooRulesToParams()` appended to the POST body), load (`editProfile()`/`loadOoRulesIntoEditor()`), and the saved-profile list's expanded preview (`onOffRulesSummaryHtml()`). Also fixed a real backend gap found while wiring this up: `profiles_edit_http.c`'s form parser never read `rule%u_temp_source`, so any temperature-comparison rule submitted through this new form would have been silently inert (`profile_resolve_on_off_rule()` only honors `temp_cmp` when `temp_source==1`) -- added parsing plus the matching `"temp_source"` JSON key in `profiles_catalog_http.c`'s GET and `profiles_export_http.c`'s export/import, all still within `PROFILE_VERSION`'s existing struct field (no schema bump). | No -- editor/serialization only; still feeds the same step-5 storage. Safe to flash whenever convenient. | Yes | `test_on_off_rules_editor.js` (render-then-serialize round trip for every field incl. `temp_source` derivation, multi-rule independence); `check_lint_pages.ps1` and `check_ui_responsive_sweep.ps1` both clean. **Follow-up 2026-09-20 (code review):** a rule whose stored `zone_index` wasn't among the current on/off zones (empty set, or zone since retyped) used to round-trip as an empty `<option value="">`, which the server's `len<=0` field parser silently dropped -- destroying the rule on save with no error. `ooZoneOptionsHtml()` now emits a flagged orphan option (`value="<idx>" selected`) so the stored index always round-trips instead of vanishing, `refreshOoUi()` no longer wipes `#oorules` when the on/off zone list is empty, and a new `ooHasStaleZoneRow()` refuses save client-side while any row still carries a flagged stale zone. Also widened `PROFILE_DETAIL_JSON_CAP`/export's per-rule byte budget 128->224 (stale comment corrected) since a max-field rule serializes wider than the old figure assumed. New coverage: `test_on_off_rules_editor.js` gained the empty-zones and stale-zone cases; a new host test renders a full 12-segment/8-rule/max-field profile and asserts complete JSON; a new standing check, `check_page_js_tests.ps1`, runs all `test/*.js` page tests under plain node so this class can't silently regress unexercised. |
| 6 | **DONE 2026-09-08 (`172e3081`).** Zones UI: type select, field hiding, fail-safe confirm. zones_page.html gets a Zone type select (Heater/On-off device), a plain-sight (not details/summary) warning stating the guard 1/2/3/4/9 disable + coupling row/column zeroing consequence, and -- for On-off device -- fail-safe state (confirm-gated via `window.confirm()` on ON), hyst_c, min_on_s/min_off_s, and a live projected-cycles/hr readout. PID gains, coupling, ramp rate, control mode and guard 1/2/3/4/9 thresholds hidden via `.heaterOnly` on selecting On-off device; on/off zones excluded from the Autotune zone picker. Wired to ZONES_CFG_VERSION 23 storage via new `z%u_zonetype/_failsafe/_hystc/_minons/_minoffs` POST fields and matching `zone_type/failsafe_state/hyst_c/min_on_s/min_off_s` GET keys (zones_http_post_parse.c/zones_http_get.c), plus range validation in zones_config_json.c/.h (ZONE_HYST_C_MIN/MAX, ZONE_MIN_ON_OFF_S_MIN/MAX). GET /api/zones also emits `on_off_hyst_c_default`/`on_off_min_on_off_s_default` top-level so the page's placeholder text is derived, never a hand-typed duplicate of the firmware constant. LCD untouched (deferred per plan sec 8 -- "No new page"; the web settings surface was sufficient for this pass). No httpd stack buffer enlarged; `json_cap` (7360 B) unaffected by the ~80 B/zone x 3 zones this adds against the documented 895 B headroom. | Yes | Yes | `test_zones_http.c::test_post_on_off_fields_optional_range_and_preserve` (accepted in range, out-of-range refused per field, omitted preserves); `check_ui_responsive_sweep.ps1` (99/99 checks, all pages, all 6 widths); negative-tested by temporarily giving `.onoffPanel` `width:2000px;white-space:nowrap` -- sweep failed with `scrollWidth=6352 > viewport=1920` plus overlap/clipped findings at 1280/1920px, reverted by hand, `git diff` confirmed clean, re-ran clean (99/99) |
| 8 | **DONE 2026-09-08 (host-tested only -- see the warning below).** Wired step 5's real rules through `on_off_trigger_decide()` into an actual relay write. New `profile_executor_on_off_zone_tick()` (`control/profile_executor_relay_io.c/`, declared `profile_executor_internal.h`) chains `on_off_trigger_decide()` -> a NEW, independent actuation-layer min_on_s/min_off_s hold (`profile_executor_on_off_actuation_gate()`, requirement 4: bounds a decision-core hold-timer bug even if `on_off_trigger_decide()`'s own hold fails) -> `profile_executor_on_off_cap_denies()` (max_simultaneous_relays, on/off zones suppressed LAST after every heater, denial logged not deferred). profile_executor.c's tick loop calls this once per on/off zone, then `apply_relay(zi, verdict)` -- the SAME chokepoint/claimed_relay_mask/relay_authority_zone_blocked()/kiln_io_owner path a heater uses, no new relay-write path (`tools/check_relay_authority_paths.py` passes with no allowlist entry). Every run-ending path (global FAULTED/abort, zone fault, authority-block/safety-trip, guard 5/6 trip, halt/IDLE/DONE, PAUSE-with-failsafe_on_pause) drives the configured fail-safe state (OFF by default; ON only when explicitly, per-zone configured) and bypasses BOTH hold layers -- plain PAUSE without the override correctly holds last state instead (not a fail-safe path). `relay_authority_zone_blocked()` queried directly (a pure, side-effect-free check) ahead of `apply_relay()` since an on/off zone's own `apply_relay()` call now happens after its verdict is decided, not before like a heater's -- `apply_relay()` itself still forces OFF under an authority block regardless of `want_on`, which is also the proof of the documented fail-safe-ON-while-blocked limitation (sec 5). Relay-cycle accounting: on/off zones never run `heater_output_bangbang()`/`heater_output_duty()`, so `z->heater_state.cycle_count`/`.relay_on` are now updated directly on every real actuation-layer transition (mirroring `heater_output.c`'s own `note_transition()`), so the pre-existing, unchanged `relay_cycles_add()` call picks them up for free instead of silently attributing zero cycles. `profile_executor_run()` now refuses to start a profile whose on/off zone COUNT ALONE meets or exceeds `max_simultaneous_relays` (sec 6's "refuse at run start"). Heater zones bit-identical: the new on/off block is entered only when `zone_on_off[zi]` is true, and a heater's own `apply_relay()` call site/position is untouched -- proven by the full pre-existing host-test suite passing unmodified (`tools/run_all_checks.ps1`, 63/66 -- the 3 unrelated failures are the in-flight cfg-filesystem migration and an in-flight zones-API field owned by another workstream, none touching control/on_off/relay code). **UNEXERCISED ON HARDWARE: no on/off zone has ever actuated a real relay** -- every zone remains `ZONE_TYPE_HEATER` on every board today, so this is inert in production exactly like step 7 was, except the code path itself is now the real actuation path a host test can drive end to end. Negative-tested by hand (the most important artifact of this step): temporarily disabled `profile_executor_on_off_actuation_gate()`'s `bypass_hold` (`if (bypass_hold)` -> `if (false && bypass_hold)`), reran host tests, got the expected RED -- 6 failures, shortest: `test_profile_executor_prestart.c:7469: a safety-relevant OFF must not be held even with a huge min_on_s` (also failed all 5 run-ending-path cases at `:7354`, i.e. a device left energised after a simulated abort/fault/trip), then restored the line by hand and reran to a clean 4887/4887 pass; `git diff` on the file is empty. | No | Yes (config: set every zone back to HEATER) | `test_profile_executor_prestart.c`'s new on/off actuation section: rule ON/OFF through `apply_relay()`/kiln_io_owner (real chokepoint), every run-ending path drives fail-safe (enumerated + negative-tested), fail-safe ON only when explicitly configured, authority-block leaves the device safe even if the verdict/failsafe_state says ON, actuation-layer min_on_s/min_off_s (including a 60-tick chatter-bound proof against a pathological flip-every-tick decision core), cap suppression last-after-heaters with truthful state on denial |
| 9 | Bench firing with a real device, owner present. | Yes | — | Owner-supervised; verify quasi-dwell on a deliberately lagging segment |

**Risks I would not take**

- Shipping steps 3 and 8 in one flash. Guard gating disables four guards; it
  gets its own flash and its own bench observation before anything actuates.
- Any bench run at step 8 with a load on the relay. Dry contacts only.
- Making S8 or any Pico guard conditional on ESP zone type. The Pico's limits
  stay unconditional and never tighter than the ESP's.
- Persisting `quasi_dwell` across a reboot.
- Defaulting `failsafe_state` to ON for any zone, however obviously a vent it
  looks.
- Touching any `cfg_fs*` file while the filesystem migration is in flight.

**Questions for the owner**

1. Which supply feeds the intended on/off device — is it behind K4? If yes,
   `failsafe_state = ON` cannot be honoured on a safety trip (§5).
2. Is the device near a safety thermocouple? If so, S8's rate window may see
   its effect (§1).
3. Confirm the interpretation at the top, and confirm that
   `PROFILE_SEG_KIND_RELAY_IO` is *not* what you actually wanted — it already
   does one-shot "turn relay 2 on at segment 4" today, without any of this.
4. Expected switching frequency, so the relay-life budget can be sized before
   step 1 rather than after step 9.
