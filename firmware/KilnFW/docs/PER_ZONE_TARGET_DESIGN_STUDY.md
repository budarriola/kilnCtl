# Per-zone target_c: design study (2026-09-04; option (b) IMPLEMENTED 2026-09-04)

Gates `PID_EXPANSION_PLAN.md` §3.6d shortlist item 1 (slow z0's own commanded
approach to a dwell — the ranked #1 fix for z0's 2.2 °C dwell-entry
overshoot, ~4x the owner's 0.5 °C bar). That item's own cost note already
flagged the real blocker: `s_exec.target_c` is a single scalar shared by
every zone, and nothing today lets one zone approach a target at a different
commanded rate than its peers without a change to the executor's core data
model. This document answers the question that note left open before any
campaign or code is costed: **does giving each zone its own target break
anything the shared scalar currently guarantees?**

No board access beyond read-only was used. No files under `firmware/**`,
`SAFETY_MODEL.md`, or `SaftyFW/**` were modified — this is a study of the
existing source, read only. Campaign `fuzzy_ab_20260904d` (arm B1) was left
untouched throughout.

## 1. What the shared scalar currently guarantees

Every dependency on "one `target_c` for every zone" found by tracing
consumers, with citations.

### 1.1 Ramp-lock — "the slowest zone sets the pace"

`profile_executor.c:473`: each active zone is tested against the *same*
`s_exec.target_c`:

```c
if (!sensor_ok[zi] || (s_exec.target_c - s_exec.zones[zi].actual_c) > EXEC_RAMP_LOCK_BAND_C(zi)) {
```

If any zone falls more than `EXEC_RAMP_LOCK_BAND_C` (25 °C, per
`PROFILES.md:272-287` and `project_ramp_lock_band_is_25c.md`) behind that one
shared value, `s_exec.ramp_lock_held` freezes `target_c` and
`segment_elapsed_s` for *every* zone (`profile_executor.c:478-479`,
`internal.h:513`) until the lagging zone catches up. Documented purpose
(`PROFILES.md:272-287`): a zone that falls behind schedule does not get
"cooked" later by being force-marched to the group's setpoint while its
neighbours have already arrived and are dwelling — the schedule itself
stalls so no zone is ever asked to catch up by exceeding its own safe rate.
This is a **protection against a lagging zone being over-driven to catch
up**, not a target-value discipline per se.

### 1.2 Dwell-entry / segment-advance decision

`profile_executor.c:605-633`: `new_target` is computed once per tick from
the segment's `ramp_c_per_hr` and applied identically to `s_exec.target_c`
(the one value every zone reads). Segment advance
(`s_exec.target_c == seg->target_c`, `:633`) and the dwell timer
(`internal.h` dwell-credit comment block, `nominal_dwell_s`/
`credited_threshold_s`) both key off this single value reaching the
segment's single `target_c`. Dwell credit itself is explicitly scoped
per-zone already (`ramp_assist_dwell_credit_tick`, `internal.h`
comment above the prototype: "`segment_target_c` is the CURRENT ramp
segment's own final target... NOT the moving `s_exec.target_c`") — the credit
machinery already tolerates per-zone reasoning about *progress*, but the
moving setpoint each zone is judged against today is still the one shared
value.

### 1.3 Segment feasibility checking

`profile_feasibility.c:80-160` (`profile_segment_feasibility()`) already
takes a `zone_index` and looks up that zone's own `k_dc`/`tau_s`/
`max_ramp_c_per_hr`, evaluating the *same* `seg->target_c` against each
zone's own plant model (`ceiling_c = t_amb + k_dc`, per-zone, `:95-96`).
**Feasibility checking is already effectively per-zone in its physics** —
the only shared quantity it consumes is `seg->target_c` itself, which is a
profile-format field, not an executor-internal one. This guard is the
cheapest of all to redefine (§2.3).

### 1.4 Guards that read `setpoint_c` — every consumer found

- **`thermal_guard.c:160,404,413`** (`thermal_guard_input_t.setpoint_c`) —
  fed at the sole call site `profile_executor.c:1042-1045`:
  `.setpoint_c = s_exec.target_c` — the shared value, passed once per zone's
  guard evaluation. This is the exact mechanism the project's own memory
  entry (`project_autotune_feeds_fake_setpoint.md`) names: a guard rule
  reading `setpoint_c` broke once before because autotune fed a placeholder
  there while the executor stayed green. Any redefinition of `target_c`
  must re-verify this call site did not silently start feeding the wrong
  per-zone value.
- **`autotune_engine.c:398-413`** — autotune's own `setpoint_c` for
  `thermal_guard_input_t` is `s_at.relay_setpoint_c` (RELAY method) or a
  placeholder (STEP method, `s_at.actual_c` — see the `raw_c` comment at
  `:342`). **Not connected to `s_exec.target_c` at all** — autotune runs
  outside the executor and has its own setpoint plumbing. A per-zone
  `target_c` does not touch this path; noted only because it is the
  precedent for what happens when a guard's setpoint feed is wrong (nothing
  crashes; the guard's answer is just quietly bogus).
- **`safety_link_frames.c:459-466`** — the frame sent to SaftyFW:
  `z->setpoint_c = zone_active ? pstat.target_c : NAN;` with an explicit
  comment: *"each active zone runs its own independent PID/guard/relay
  against one shared setpoint... there is no per-zone setpoint to report, so
  every currently-active zone reports the one shared target."* This is the
  single clearest textual statement of today's invariant, and the one line
  that must change if `target_c` becomes per-zone (§2.4).

### 1.5 The zone-ceiling refusal

`profile_feasibility.c:107`: `if (target > ceiling_c - FEASIBILITY_CEILING_MARGIN_C) return PROFILE_SEG_UNREACHABLE;` —
already per-zone in the physics (§1.3); the "ceiling" here is each zone's
own identified `k_dc`-derived steady-state limit, not a shared board-wide
ceiling. This guarantee does not depend on the scalar being shared at all.

### 1.6 SaftyFW's consumption of the wire setpoint — S2 (over-setpoint trip)

`firmware/CommonFW/docs/LINK_PROTOCOL.md:359`: the `KILNLINK` context frame
carries a **per-zone** `setpoint_c` field already — the wire format is not
the bottleneck. On the SaftyFW side, `snapshots.h:144-193`
(`safety_context_reduce()`, name approximate) folds every zone's own
`setpoint_c` down to `out_max_setpoint_c`, taking the **maximum across
zones** (`:175-176`: `if (count == 0u || z->setpoint_c > max_setpoint) max_setpoint = z->setpoint_c;`).
`safety_guards.c:190-194,825-831` (guard S2,
`SAFETY_TRIP_OVER_SETPOINT`) then trips when the agreed chamber
thermocouple reads above `max_zone_setpoint_c + overshoot_margin_c`.

**This is the one guarantee in this whole survey that already tolerates
per-zone values without any change on the SaftyFW side.** SaftyFW never
assumed the frame's `setpoint_c` values were identical across zones — it was
written to take a max, which is exactly the conservative, safe reduction a
genuinely per-zone setpoint needs. Today, because KilnFW happens to send the
same value in every zone's slot, the max is trivially that value; if KilnFW
starts sending distinct per-zone values, S2's behaviour is unchanged in kind
(it still trips on the single highest commanded target across zones plus
margin) and, if anything, becomes *more* correct — right now a zone
deliberately lagging behind a shared target could theoretically be closer to
its physical ceiling than the shared value implies without S2 knowing which
zone that is; a true per-zone max fixes that rather than weakening it.

## 2. What breaks under a per-zone target — the crux, resolved per guarantee

| Guarantee | Survives / redefine / lost |
|---|---|
| Ramp-lock (§1.1) | **Redefine — this is the crux, resolved below** |
| Dwell/segment advance (§1.2) | Redefine (per-zone target reached, not the shared one) |
| Feasibility (§1.3) | Survives near-unchanged (already per-zone in physics) |
| `thermal_guard` `setpoint_c` (§1.4) | Redefine (feed each zone's own target) |
| Zone-ceiling refusal (§1.5) | Survives (already per-zone) |
| SaftyFW S2 (§1.6) | **Survives, and arguably strengthens** |

### 2.1 The crux: does per-zone target undermine what ramp-lock protects?

Ramp-lock exists so a **lagging** zone is never force-marched to catch up to
a group schedule it fell behind on by accident (thermocouple slow to
respond, more thermal mass, coupling stealing its heat) — the protection is
against the *executor* commanding a rate the plant cannot deliver safely.
A per-zone target for z0's dwell approach is a **deliberate, planned**
choice to have z0 lag z1/z2 by design, not an accidental fall-behind. These
are different failure modes and the tension is real but resolvable:

- Ramp-lock's band (25 °C) is far wider than the ~0-3 °C offset a slower z0
  approach would ever produce relative to its peers over one final ramp
  segment (z0's own overshoot is 2.2 °C; the *rate* cut under discussion is
  ~1 °C/min over a segment lasting minutes, not a sustained 25 °C gap). A
  correctly-sized per-zone rate change would never come near tripping
  ramp-lock's band in the direction ramp-lock exists to catch.
- The danger ramp-lock guards against — a zone being cooked by continuing to
  chase a target its neighbours have already reached and are dwelling at —
  is **still fully present** for a *deliberately* slow zone. If z0's own
  target lags but z1/z2 finish their approach and start dwelling (with their
  associated relay duty easing), z0 is not "protected from being cooked" by
  ramp-lock in the deliberate case any more than in the accidental case —
  the coupling injection this whole investigation is about (§2b of the
  mechanism report, row-sum 49.04 into z0) comes from z1/z2's duty, which
  ramp-lock does not gate at all today (it gates `target_c`, not duty).
  **Ramp-lock was never protecting z0 from coupling overshoot — it protects
  the schedule from silently advancing past a zone that physically cannot
  keep up.** A deliberately-offset per-zone target does not remove that
  protection; it just requires ramp-lock to compare each zone against its
  *own* target instead of the shared one, which is a strictly narrower,
  more accurate test of the same "can this zone keep up with what it was
  asked to do" question — it stops conflating "z0 is 1.5 °C behind its own,
  deliberately slower schedule" with "z0 is lagging."
- The remaining open risk: if a per-zone offset is allowed to grow without
  bound (option (c) below — fully independent trajectories), ramp-lock's
  cross-zone meaning erodes entirely, because "the slowest zone sets the
  pace" stops being a well-formed sentence when there is no single pace.
  That is a real loss, and is exactly why option (c) is not recommended
  (§3).

**Resolution:** ramp-lock survives, redefined from "every zone vs. one
shared target" to "every zone vs. its *own* target, each target still gated
by the same 25 °C band against that zone's own actual reading." This
preserves the exact safety property (no zone silently exceeds its own
achievable rate) while permitting the deliberate offset this investigation
wants. It does **not** preserve "the group cannot progress past a stuck
zone" verbatim if targets fully decouple — see option (b) below for the
design that keeps that property intact by construction.

### 2.2 Dwell/segment advance

Segment advance today is one boolean (`target_c == seg->target_c`) checked
once. Per-zone targets make this per-zone: segment N is "reached" for the
group only when every active zone's own target equals its own segment
target. This is a mechanical redefinition, not a lost guarantee — dwell
timing already treats `segment_target_c` as per-zone-relevant
(§1.2, dwell-credit).

### 2.3 Feasibility

No change needed to the check's physics; only the caller needs to pass each
zone its own `target_c` where it currently passes the one shared
`seg->target_c`. Lowest-risk item in the whole set.

### 2.4 `thermal_guard` `setpoint_c` and the "fake setpoint" precedent

The fix is mechanical (`.setpoint_c = s_exec.zones[zi].target_c` instead of
`s_exec.target_c`) but the **process** risk is exactly the one the project's
own memory names: a guard reading the wrong setpoint value stays green while
silently checking the wrong thing. This item is called out explicitly as a
required, host-testable regression check in §4 — not because the fix is
hard, but because this exact class of bug has shipped before undetected by
an equal test count (`project_split_module_missing_name_class.md`,
`project_autotune_feeds_fake_setpoint.md`).

### 2.5 SaftyFW / wire protocol

No change required at all. See §1.6 — the frame already carries a per-zone
field and SaftyFW already reduces it with `max()`, which is the correct,
conservative reduction for a genuinely per-zone setpoint. This is the
biggest positive surprise of this study: the piece everyone would guess is
hardest (crossing the safety-critical link) is already done correctly and
needs zero change.

## 3. Design options, ranked

### (a) Per-zone offset/scale applied to the shared ramp

Each zone's effective target = shared `target_c` plus a small per-zone
offset (or the shared ramp scaled by a per-zone multiplier), re-clamped so
the offset only ever *lags* a zone, never leads it past the group target.

- **Fixes:** exactly the mechanism this investigation identified — z0 can
  arrive later/slower than its siblings on the final approach to a dwell.
- **Risks:** the offset must be bounded well inside ramp-lock's 25 °C band
  or it silently changes ramp-lock's meaning (§2.1); needs a clear rule for
  what happens at segment end (does the offset zone's dwell start late, or
  does it snap to the group's dwell start with the offset "spent"?).
- **Size of change:** moderate. `target_c` becomes an array (or the scalar
  stays and an array of offsets is added next to it — smaller diff, same
  shape as the already-shipped per-zone `ease_off_window_mult`). Ramp-lock's
  comparison changes from one line to one per zone. Feasibility passes
  each zone its own effective target. `safety_link_frames.c:466` changes
  from `pstat.target_c` to `pstat.zones[i].target_c`.
- **Validation:** host-testable in full — the offset arithmetic, ramp-lock's
  redefined per-zone comparison, segment-advance logic, and the guard
  `setpoint_c` feed are all pure state-machine logic with existing host-test
  infrastructure (`firmware/KilnFW/App/test/`). Kiln time only needed to
  measure whether the resulting slower z0 approach actually reduces
  overshoot by the predicted amount (the mechanism report's own open
  question, not a new one this design creates).

### (b) Per-zone ramp-rate limit — only ever slows a zone, never speeds it, same destination

A single shared *destination* `target_c` stays (satisfies segment advance
and feasibility exactly as today), but each zone's own PID setpoint this
tick is capped by a per-zone max approach rate that can only be tighter
than the segment's programmed rate, never looser. z0 gets its own tighter
cap; z1/z2 keep the segment rate. Ramp-lock is unchanged in structure — it
still compares each zone's own (now individually rate-limited) setpoint
against its own actual, and "reached" is still "setpoint == shared target,"
just reached later for the capped zone.

- **Fixes:** the same overshoot mechanism as (a), with a narrower, more
  auditable knob (one number: max °C/min for z0's final approach).
- **Risks:** smallest of the three code-changing options — the shared
  target destination is untouched, so §1.2's segment-advance and §1.5's
  ceiling refusal need zero redefinition; only the setpoint-stepping tick
  (`profile_executor.c:605-633`) gains a per-zone clamp, and ramp-lock keeps
  comparing against the one shared value it always has (a rate-limited zone
  is, by construction, the zone most likely to need the lock, and the lock
  still catches it exactly as today).
- **Size of change:** smallest of the three — no new per-zone target state,
  no redefinition of "reached," just one extra per-zone clamp term feeding
  the existing tick. Closest in shape to the already-shipped per-zone
  `ease_off_window_mult`.
- **Validation:** fully host-testable (clamp math, interaction with
  ramp-lock, interaction with dwell timing) before any kiln time; kiln time
  only for the overshoot-reduction measurement itself.
- **Recommended shape, pending owner sign-off on §5.**

### (c) Fully independent per-zone trajectories

Each zone gets its own complete ramp/dwell schedule, decoupled from the
segment table's single shared timeline.

- **Fixes:** the overshoot mechanism, plus opens the door to arbitrary
  future per-zone profile shaping.
- **Risks:** **loses** ramp-lock's cross-zone meaning as described in §2.1's
  last paragraph — "the slowest zone sets the pace" stops being
  well-defined once there is no single pace to fall behind on. Segment
  advance, dwell-entry, and feasibility all need ground-up redesign, not
  redefinition. This is the profile-format and executor rewrite the
  mechanism report's own cost note was warning about, at its largest scope.
- **Size of change:** large — new profile format fields, new executor state
  machine, new feasibility semantics, re-derivation of every guarantee in
  §1 from scratch rather than incremental redefinition.
- **Not recommended** for this problem: the overshoot this is chasing is a
  single final-approach rate difference on one zone, not a need for
  arbitrarily different schedules. Using (c) to fix it is solving a much
  bigger problem than the one in evidence.

### (d) Leave the trajectory alone; attack the overshoot at the controller

Per-zone feedforward compensation scaled by z0's own `model_k_dc`/
`model_dead_time_s` (mechanism report §2c / X5 item 4), or another
control-law change that does not touch `target_c` at all.

- **Fixes:** potentially the same overshoot, without touching the executor's
  shared-scalar model or any of the guarantees in §1 at all.
- **Risks:** the mechanism report already flagged this as
  architecture-level and *lower priority* than the trajectory fix — it
  compensates for z0's higher gain/longer dead time, which the report
  frames as an *amplifying* factor, not the trigger (§2c). Also carries its
  own history of failed attempts in this exact area (ease-off window,
  climb-decay-into-dwell, both withdrawn per `PID_EXPANSION_PLAN.md` §4 and
  the mechanism report's own X2-X3).
- **Size of change:** unknown until designed; likely smaller than (a)/(b)
  since it touches only the feedforward calculation, not the executor's
  state model — but with no direct-actuator confidence like (a)/(b) have
  (the ramp-rate→overshoot correlation is the mechanism report's
  best-supported finding, r=0.76 sign-robust; a feedforward compensation
  scaled by k_dc/dead-time has no equivalent direct evidence yet, only the
  plausibility argument in §2c).
- **Not recommended first**, per the mechanism report's own ranking (X5 item
  4, "lowest priority... until (1) and (3) are exhausted"). Nothing in this
  study changes that ranking — it is reproduced here for completeness
  against this task's explicit instruction to weigh it.

## 4. Recommendation and validation plan

**Recommend option (b)** — a per-zone maximum approach rate that only ever
tightens the segment's shared rate, with the shared `target_c` destination,
segment-advance, and feasibility logic left untouched. It is the smallest
change that reaches the actuator the mechanism report already validated
(commanded approach rate, §X5 item 1-2), it leaves ramp-lock's comparison
*code* and meaning completely intact (§2.1's resolution is satisfied by
construction, not by redefinition) — though, per §6 below, an aggressive
enough cap can still trip that unchanged comparison group-wide, it just
cannot redefine what it means to trip it — and it composes cleanly
with the already-shipped per-zone `ease_off_window_mult` pattern the team
has already reviewed and shipped once.

### 4.1 What can be host-tested (before any kiln time)

- The per-zone rate clamp itself: given a segment rate and a tighter z0 cap,
  the stepped setpoint never exceeds the cap, and z1/z2 are bit-identical to
  today (regression: existing `test_closed_loop.c`-style captures re-run
  unchanged for z1/z2).
- Ramp-lock interaction: construct a scenario where the clamp makes z0
  "lag" its own capped schedule and confirm the *existing* ramp-lock
  behaviour (freeze `target_c`/`segment_elapsed_s`) still triggers
  correctly and for the right zone (`ramp_lock_lagging_mask`).
- Segment-advance/dwell timing: confirm the group's segment only advances
  once z0's setpoint (now later, under its own cap) reaches the shared
  target, not before — this is the main behavioural change users would
  notice, and it must be an explicit, asserted test rather than an implicit
  side effect.
- `thermal_guard` `setpoint_c` regression (§2.4): a negative test proving
  the guard's fed setpoint changes when the clamp changes z0's actual
  commanded value — per `feedback_negative_test_every_check.md`, prove this
  check can fail before trusting it passes. This is the single most
  important host test in this whole plan, because it is exactly the failure
  mode (`project_autotune_feeds_fake_setpoint.md`) that has shipped
  silently before.
- Feasibility: confirm `profile_segment_feasibility()` is unaffected (it
  should be, since (b) never changes `seg->target_c`) — a one-line assertion
  that feasibility verdicts are identical with and without the clamp
  configured.
- `safety_link_frames.c`: with (b), the shared destination `target_c` is
  unchanged, so **no wire-protocol change is required at all** for this
  option — `z->setpoint_c = pstat.target_c` stays correct as written,
  because the clamp only affects the *rate of approach*, not the
  *destination* each zone is ultimately driving toward. This is a further
  point in (b)'s favour over (a): it needs none of §2.5's link-side
  changes.

### 4.2 What needs kiln time, and how to A/B it under `AB_EXPERIMENT_CHECKLIST.md`

Following the checklist explicitly, since it was itself rewritten out of
this same investigation's earlier false starts:

1. **Find every gate above the parameter** (§1 of the checklist): the new
   per-zone rate-cap config must be traced from preset → `zones_config` →
   the tick that applies it, confirming (a) it is read every tick the
   segment is ramping, not just at segment start, and (b) nothing upstream
   (commissioning state, `zone_active`, ramp-lock's own freeze) can leave
   the cap configured but unreachable. This is exactly the class of bug the
   checklist's own worked example (`control_mode: 2` vs `3`) describes, and
   the same class the mechanism report's X3 found in the withdrawn
   ease-off-window proposal (a knob that changes *when* a term tapers but
   never reaches the quantity the campaign wants to move) — so this gate
   check must include, as its own explicit item, **is z0's own duty (not
   just its feedforward term) different at the transition under a tighter
   cap** — the exact test that would have caught the ease-off knob's failure
   earlier.
2. **Live reachability proof** (§2 of the checklist): with a profile
   running, apply the capped preset and confirm — via `bd_*` fields
   (`bd_ff_rate_pretaper`/`posttaper` are not the right field here per X3;
   the direct measurement is z0's own `actual_c` slope over the final
   segment, sampled live) — that z0's measured approach rate is actually
   lower under the cap, *before* costing a full campaign. This directly
   answers the question the withdrawn ease-off proposal got wrong by
   skipping it (X1/X3: that knob's one direct actuator test was null).
3. **A/B design**, reusing the already-worked-out power arithmetic from the
   mechanism report (§X4): run on **stock profile 7** (`ab_new` variant,
   within-config SD 0.04-0.06 °C), 2 arms (A: no cap / board default; B: z0
   capped to roughly the segment rate minus ~1 °C/min) x 3 runs — >99% power
   at that SD for the predicted 0.5-1.8 °C effect, ~3.5-4.5 hours total kiln
   time, matching the mechanism report's own costed design.
4. **Decision rule** (§X4/W4 of the mechanism report, reused verbatim): z0's
   own ≥3 same-direction keys (overshoot, offset, normalised IAE at the
   final segment) as the primary endpoint, z1/z2 as pre-registered negative
   controls that must **not** move — a change leaking into the uncapped
   zones would falsify that the cap is doing what it claims and nothing
   more.
5. **Verify the probe field discriminates** (checklist requirement): before
   trusting any campaign result, confirm the chosen live-measurement field
   (z0's `actual_c` slope, or a new `bd_*` field if the clamp is
   implemented via a feedforward-adjacent path) actually differs between
   arms in a captured `.jsonl`, the same way X4 verified
   `bd_ff_rate_pretaper`/`posttaper` differ on real data before relying on
   them — do not repeat the mistake the withdrawn proposal made of trusting
   a metric no one had checked moves.

## 5. Owner calls — flagged explicitly, not decided here

- **Whether preserving ramp-lock's *exact current wording* ("the slowest
  zone sets the pace" against one shared value) matters more than the
  redefinition in §2.1**, even though this study concludes the redefinition
  preserves the underlying safety property. Reasonable people could weigh
  "provably equivalent protection, differently worded" against "keep the
  invariant literally unchanged" differently; recommendation (b) sidesteps
  this entirely by not touching ramp-lock's comparison at all, but if the
  owner ever wants option (a) instead, this trade becomes live.
- **No option evaluated here trades a safety property for tracking
  performance.** §1.6 in particular is a genuine, unexpected finding worth
  the owner's attention on its own: SaftyFW's S2 guard was already written
  to take a `max()` over per-zone setpoints, which means it does not need
  to be touched, weakened, or reasoned about specially for this change —
  the wire protocol and safety processor already support the exact shape
  of change under discussion. This is a "nothing needed here" situation
  reported, not a request for a safety exception.
- **Whether to spend the kiln time on option (b) at all before (a) is later
  wanted for other reasons.** This study's brief was gating the decision,
  not committing to build it; if the owner's priority shifts, (b) alone
  does not preclude adding (a)'s general per-zone offset later, since (b) is
  a strict subset of the state (b) would need anyway (a per-zone value next
  to the shared scalar).

## 6. Implementation status (2026-09-04)

**Option (b), as recommended in section 4, is IMPLEMENTED** —
`zone_cfg_t::approach_rate_cap_c_per_hr` (ZONES_CFG_VERSION 17→18), a
per-zone cap on how fast `zone_runtime_t::effective_target_c` may approach
the shared `s_exec.target_c`, computed once per tick in `profile_executor.c`
immediately before the per-zone control-mode pass. 0 (uncapped, the default)
makes every consumer bit-identical to reading `s_exec.target_c` directly, for
every zone, exactly as this section anticipated. See `PROFILES.md`'s new
"Per-zone approach-rate cap" subsection for the full mechanism, and
`PID_EXPANSION_PLAN.md` sec 3.6d for the landing note.

What this pass confirmed against the plan above, each independently
verified rather than assumed:

- **§1.6/§2.5 (SaftyFW S2 needs no change)**: confirmed by re-reading
  `snapshots.h:144-193`/`safety_guards.c:190-194,825-831` — `SaftyFW/**` was
  not touched, and does not need to be: the wire's `setpoint_c` per zone is
  still `pstat.target_c` (the shared destination) for every zone, since (b)
  never makes the wire value per-zone. S2's `max()` reduction was never
  exercised differently by this change.
- **§2.4 (`thermal_guard` "fake setpoint" decision, made deliberately)**: a
  capped zone's `thermal_guard_input_t.setpoint_c` is fed
  `zone_commanded_setpoint_c(z, zi)` — `effective_target_c` when capped,
  `s_exec.target_c` when not — not `s_exec.target_c` unconditionally. This
  re-checks the cap at the guard-feed site itself (rather than trusting a
  field some caller may not have populated) specifically so every existing
  host test that calls `pid_family_zone_tick()`/`zone_feedforward()` directly
  with a hand-built `zone_runtime_t` (never running `profile_executor.c`'s
  own per-tick cap-update loop) still gets exactly today's behaviour with no
  test changes required — only a test that deliberately configures a
  non-zero cap needs to also seed `effective_target_c`.
- **Ramp-lock, segment-advance, feasibility — code untouched, behaviour is
  NOT immune.** `profile_executor.c`'s ramp-lock loop and the
  `s_exec.target_c == seg->target_c` segment-advance check are byte-identical
  to before this pass, and `profile_feasibility.c` was not edited at all —
  but "the code is unchanged" does not mean "a configured cap cannot
  disturb what that code does," and an earlier draft of this study stated it
  that way. Ramp-lock compares `actual_c` (never `effective_target_c`)
  against the shared `target_c` with a 25 °C band
  (`EXEC_RAMP_LOCK_BAND_C`); a cap slow enough to let a zone's `actual_c`
  fall more than 25 °C behind the shared target trips `lock_ok = false` for
  every zone, freezing the whole group's schedule until the capped zone
  catches back up — self-limiting and deadlock-free, but a real, group-wide
  consequence of a per-zone setting. Segment-advance keys on the shared
  `target_c`, not `effective_target_c`, so a capped zone can still be short
  of arrival when the group advances or enters a dwell (bounded by the same
  25 °C band). And feeding `thermal_guard_input_t.setpoint_c` from
  `effective_target_c` measurably reduces guard 1's sensitivity on a capped
  zone (see `PROFILES.md`'s "Per-zone approach-rate cap" section for the
  full mechanism and numbers). The accurate claim is "cannot disturb
  ramp-lock for caps that keep the zone inside the 25 °C band," not
  unconditional immunity.
- **Migration**: unlike `ease_off_window_mult`'s v16→v17 hop, there was no
  prior global scalar to carry forward — every zone of every pre-v18 blob
  lands on the 0 (uncapped) sentinel via `convert_versioned_blob_to_current()`'s
  own entry `memset`, verified by a dedicated v17→v18 migration test
  (`test_nvs_load_from_v17_blob_defaults_approach_rate_cap_to_uncapped`,
  `test_zones_http.c`) that also pins a REAL, non-default per-zone
  `ease_off_window_mult` A/B arm surviving the same hop unchanged.
- **Default is behaviour-identical**: proven, not assumed — every consumer
  (`zone_taper_climb_rate()`, `zone_feedforward()`, `pid_update_terms()`, the
  BANGBANG hysteresis compare, the cooling-limited diagnostic, the guard feed)
  resolves to `s_exec.target_c` verbatim whenever a zone's cap reads 0, via
  the shared `zone_commanded_setpoint_c()` helper — there is exactly one
  place this resolution happens, not one per call site that could drift.

**What still needs a flash and a kiln A/B** (unchanged from section 4.2's
plan, not run by this pass): the live reachability proof (does a capped z0's
measured `actual_c` slope actually drop under a configured cap), the 2-arm
A/B on stock profile 7, and the decision rule against z1/z2 as pre-registered
negative controls. **Not flashed** — a live A/B campaign
(`fuzzy_ab_20260904d`) was running at the time this landed, and the
ZONES_CFG_VERSION 17→18 bump (stacked on top of `ease_off_window_mult`'s own
still-unflashed 16→17 migration) means the eventual flash must run the real
migration chain against that board's actual, non-default live config, not a
fresh commission — deliberately deferred to after the campaign rather than
verified during it.

## Files

- This report: `firmware/KilnFW/docs/PER_ZONE_TARGET_DESIGN_STUDY.md`.
- Linked from `PID_EXPANSION_PLAN.md` §3.6d.
- Built from: `firmware/KilnFW/App/drivers/control/profile_executor.c`,
  `profile_executor_internal.h`, `profile_feasibility.c`,
  `thermal_guard.c`, `autotune_engine.c`, `safety_link_frames.c` (read only);
  `firmware/CommonFW/docs/LINK_PROTOCOL.md`; `firmware/SaftyFW/src/snapshots.h`,
  `safety_guards.c` (read only, per the constraint that SaftyFW source is
  off-limits to edit — nothing here proposes touching it, precisely because
  §1.6/§2.5 found it does not need to change).
- Depends on: `logs/coupling/z0_dwell_overshoot_mechanism_20260904_report.md`
  §X5 (the shortlist this study gates item 1 of), `PID_EXPANSION_PLAN.md`
  §3.6b (reachability audit discipline), `firmware/KilnFW/docs/AB_EXPERIMENT_CHECKLIST.md`.
