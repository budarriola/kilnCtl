# Unfitted CT channels 0/1 vs S3's fallback margin — 2026-09-18

**Verdict: this is a real, live hazard — but the hazard is a nuisance trip
(S3 false-arming on two channels with nothing plugged in), not a missed
safety event.** The bench board's `ct_installed = 1` (channel 3 is
genuinely fitted) means the "no CT at all" gate (`current_sensing_disabled`)
is **off**, so S3 (`SAFETY_TRIP_LOAD_STUCK_ON`) evaluates the raw,
uncommissioned floor reading on channels 0 and 1 exactly as if they were
real sensors. Measured idle margin on those two channels today is **8-9
counts** out of a 25-count fallback threshold — not headroom built from a
deliberate design tolerance, just whatever the op-amp's unsubtracted DC
offset happens to sit at right now.

**Addendum, same day:** a parallel change is masking `current_any_present()`
per channel so an unfitted channel cannot contribute to it, alongside making
`current_sensing_commissioned` topology-aware. §4 below traces the analog
front end (`R90`'s documented role as "the DC return for the CT secondary")
and confirms, rather than merely assumes, that an unfitted channel's reading
is 100% offset/leakage artifact and 0% real current information — masking it
throws away no genuine detection capability for S3 or S9, and closes a
sharper regression that relaxing the commissioning gate would otherwise open
for S9 (see §4's last paragraph). One residual, non-eliminated risk is
named: masking is only as good as keeping the per-channel "fitted" answer
synchronized with physical reality.

## 1. Why the "not fitted" fact does not protect anything here

Two independent gates exist in this codebase for "don't trust the current
reading," and neither one is keyed to "is a CT physically clamped onto
*this specific conductor*":

- **`current_sensing_disabled`** (`cts_disabled` in
  `firmware/SaftyFW/src/tasks/safety_core.c` around line 1132) is `true`
  only when the config record explicitly answers `ct_installed = 0` —
  i.e., "this board has no current sensing hardware at all." This bench
  board has a real, fitted CT on channel 3, so `ct_installed = 1` and this
  gate is **false**. It is a board-wide fact, not a per-channel one, and it
  says nothing about channels 0/1 having no clamp on them.
- **`current_sensing_commissioned`** (`s_current_sensing_commissioned`,
  set around line 449 of the same file) requires **all three**
  `k_ct_v_per_a[ch] > 0.0f`. With channels 0/1 both at `k_ct_v_per_a = 0`,
  this is **false** board-wide.

`current_any_present()` (`firmware/SaftyFW/src/snapshots.h`) is an OR
across all three channels' `present[n]` flags, and
`current_presence_is_flowing()` (`firmware/SaftyFW/src/current_presence_policy.c`)
computes `present[n]` for an uncommissioned channel (`k_ct_v_per_a <= 0`)
as `delta_counts > CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS` (25,
`current_presence_policy.h` line 97), where `delta_counts = counts_avg -
zero_counts` and `zero_counts = 0` for both unfitted channels. So
`delta_counts` on channels 0/1 is simply the raw ADC reading — no offset is
ever subtracted, because there is no measured offset to subtract from a
channel with no sensor to run the zero-calibration routine against.

Net: **any one of the three channels crossing 25 counts sets
`any_current_present = true`**, board-wide, regardless of whether the
other two channels (including the genuinely fitted one) show anything.

## 2. Which guards downstream actually act on it

Traced every consumer of `any_current_present` in
`firmware/SaftyFW/src/safety_guards.c`:

| Guard | Gate present? | Live on this board? |
|---|---|---|
| **S3** `SAFETY_TRIP_LOAD_STUCK_ON` (line 858) | Only `current_sensing_disabled` (line 852) | **YES — live, unprotected.** `current_sensing_commissioned` is never checked here. |
| S4 (`relay_commanded_continuously && !any_current_present`, line 874) | `current_sensing_disabled` | WARN-only regardless (never trips); the *opposite* direction of concern (masks a dead element), already documented and accepted risk in `current_presence_policy.h`'s own header comment. |
| S6b `SAFETY_TRIP_LINK_DEAD` soft path (line 506) | None — checked directly | Live, but only matters while the link is already down past `link_timeout_s`; an unfitted-channel false positive here only removes a grace period that a real safety condition (link already silent) is already active for. Not the channel this report is about. |
| S9 (`trip_ineffective` escalation, lines 405-441) | Explicitly branches on `current_sensing_disabled` **and** `current_sensing_commissioned` (lines 407, 419) | **Protected.** An uncommissioned board never progresses the streak toward the unclearable latch — it raises `s9_uncommissioned_warn` instead (non-latching). This is precisely the gate the 2026-08-27 fix added, and the header comment for `current_presence_policy.h` explains why: S9's fallback-margin false positive would otherwise misdiagnose "current still flowing after de-energize" as a welded contactor, off a phantom floor reading, forever. |
| S11 (frozen-sensor arm-only use) | N/A — false positive only arms a check more eagerly | Documented as harmless by `current_presence_policy.h`'s own comment. |

**S3 is the one live, unprotected consumer.** Its condition
(`safety_guards.c:858`) is:

```c
} else if (in->any_current_present && !in->relay_commanded_recently) {
    state->s3_stuck_elapsed_s += in->dt_s;
    ...
    if (state->s3_stuck_elapsed_s >= stuck_th) {   /* stuck_on_time_s, default 20.0s */
        trip(state, SAFETY_TRIP_LOAD_STUCK_ON, ...);
```

Nothing else stands in the way once `any_current_present` reads true and no
relay has been commanded within `correlation_window_s` (default 150 s,
`firmware/SaftyFW/src/safety_guards.h` line 237): the accumulator sums
`dt_s` every tick the condition holds and trips unconditionally at
`stuck_on_time_s` (20 s default). This is a **sustained-condition** timer,
not an edge/streak debounce — a single noisy sample that clears on the next
tick resets the accumulator to zero (`safety_guards.c:868`), but a
*sustained* upward reading (the floor sitting above 25 counts continuously,
not merely spiking through it) satisfies the condition on every tick and
trips in exactly 20 s.

This is the exact scenario `firmware/SaftyFW/docs/CURRENT_SENSE.md`'s
"Known limitation" section and completion checklist already name and leave
open: `"§5 step 2 (one relay at a time) passed on all three channels —
gates S3/S4 — needs real hardware, not done."` This audit is a from-source
confirmation that the still-open gap is not merely theoretical on today's
bench board — the measured margin against it is small.

## 3. Is 8-9 counts real margin, or noise around a small number?

Today's captures (channel 1: mean 16.23 counts, std 0.418, range 16-17
counts over 30-60 s; channel 2: mean 17.00, std 0.000, range 17-17 counts,
per `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md`'s bench
data) show the **short-term noise** on these two channels is far below the
margin — sub-count. That is not the risk. The risk is `zero_counts = 0`
leaving the entire DC offset floor unsubtracted, and that floor is not
guaranteed to sit still:

- `CURRENT_SENSE.md` §5 states plainly: *"`zero_counts` is not zero and
  must not be assumed to be... the op-amp's offset appears as a small
  positive floor"* and *"A `zero_counts` that drifts is itself a fault... if
  it has moved by more than a few counts, report it."* The doc treats
  **a few counts of drift** as significant enough to flag as a fault
  condition worth alarming on for a *commissioned* channel. On an
  *uncommissioned* channel there is no alarm, no baseline, and no
  subtraction — a few counts of the same drift mechanism silently eats
  most of the 8-9 count margin.
- The offset floor's source is documented as **U8 (AD8542) input offset
  voltage plus D14 (Schottky) reverse-leakage current into the R77/C57
  hold node** (`CURRENT_SENSE.md` §1/§5). Both terms are temperature-
  dependent by nature — CMOS op-amp `Vos` drifts with die temperature, and
  Schottky reverse leakage roughly doubles every ~10°C rise, which raises
  the steady-state peak-hold floor (leakage current sets how high the R77
  droop settles against the recharge pulses). This board sits in an
  enclosure a few feet from a kiln that is, by design, the heat source this
  whole project exists to run — an hours-long firing is exactly the
  condition under which enclosure/ambient temperature at the safety board
  rises the most, and does so on a timescale (minutes to hours) that lines
  up with S3's 20 s sustained-condition trip far better than any
  microsecond/millisecond noise process would.
- This is a *systematic*, not random, effect. A slow monotonic rise in the
  floor is precisely the shape S3's sustained-accumulator (not a spike
  debounce) is built to catch on a *real* stuck relay — it is equally well
  shaped to catch a *thermally drifting phantom floor* on an unfitted
  channel, since the guard has no way to distinguish "real current that
  has been on continuously for 20 s" from "an offset that has been above
  threshold continuously for 20 s."
- No datasheet-derived quantitative temperature coefficient for the AD8542
  or D14 leakage is present anywhere in this repo's docs, and this audit
  does not fabricate one. The point stands qualitatively either way: an
  8-9 count (≈6.5-7.3 mV) margin is small against *any* nonzero thermal
  coefficient sustained over an hours-long firing, and the repo's own
  commissioning doc independently treats "a few counts of zero-offset
  drift" as fault-worthy on channels that DO get to run the zero-cal
  routine. Channels 0/1 never get that routine (there is no CT to
  auto-zero against), so whatever floor drift exists is invisible until it
  crosses 25 counts and trips S3 outright.

Effects here are measured in ADC counts (1 count ≈ 0.8 mV), not degrees C,
so the "ignore sub-0.5°C" convention does not apply and is not being
invoked to wave this away.

## 4. Does masking presence on an unfitted channel throw away real detection capability?

A parallel change is now underway making `current_sensing_commissioned`
topology-aware (so a kiln with one or two shared CTs need not calibrate all
three channels) and, in the same change, masking `current_any_present()` so
a channel with **no CT fitted** cannot contribute to it at all. That second
half is exactly the mechanism this report has been analyzing, so the
question worth answering precisely is: does an unfitted channel's counts
reading carry *any* genuine current information that masking would throw
away, or is it pure offset?

**Traced from the schematic (`CURRENT_SENSE.md` §1, `hardware/mainBoard/output/kiln.pdf` p.4):
a channel with no CT plugged into its jack (J13/J-equivalent) has no
physical path for a real current signal to reach that channel's ADC at
all.** The relevant facts, all from the circuit diagram and its accompanying
text:

- Each channel's op-amp input (`U8A`'s `−` pin, through `R43`) is driven
  **only** by whatever is plugged into that channel's own 3.5 mm jack. The
  three channels do not share an input node, a burden resistor, or a
  sense line — they are three electrically independent instances of the
  same circuit (`"channels 2 and 3 are identical (R78/R83/…, R84/R89/…)"`,
  §1), each fed by its own CT secondary.
- **`R90` (1 MΩ) is explicitly documented as "the DC return for the CT
  secondary. Without it the input node floats and the rectifier output
  wanders."** With no CT plugged in, R90 is doing exactly what it is there
  to do when nothing else is driving the node: holding the rectifier's
  input near ground, not near some indeterminate or externally-coupled
  voltage. There is no antenna effect, no capacitive pickup path, and no
  shared reference documented anywhere in this circuit that would let
  current flowing through a *different*, unrelated conductor (e.g. the
  kiln's actual heating elements, which are galvanically nowhere near this
  jack) influence this channel's reading.
- The nonzero floor this report measured (16-17 counts) is fully accounted
  for by the same two sources §5 already names for the *fitted* channels'
  zero-current floor: **`U8A`'s input offset voltage** (present regardless
  of what, if anything, is plugged into the jack — it is a property of the
  op-amp, not the signal) and **`D14`'s small reverse leakage current into
  the `R77`/`C57` hold node** (present whenever the board is powered,
  again independent of the jack). Nothing in the transfer function (§2) has
  a term that depends on any external current source once the CT itself is
  physically absent — the "signal" input to the rectifier is simply
  undriven.

**This confirms, rather than merely assumes, the claim the parallel fix
rests on: an unfitted channel's reading is 100% offset/leakage artifact and
0% current information, by construction of the analog front end, not by
firmware policy.** Masking `current_any_present()` for such a channel does
not remove a bias toward detecting real current — there is no real current
for it to ever detect. `current_presence_policy.h`'s stated rationale for
the loose fallback margin ("bias toward detecting current... the safe
side") is about *sensitivity on a channel that has a wire on it*; it was
never a rationale for treating a channel with no wire on it as informative,
and nothing in that header comment claims otherwise — it is silent on the
unfitted-channel case because, until this task, no code path distinguished
it from "wired but not yet commissioned."

**One residual risk, not eliminated by the physical argument above, is
provenance rather than physics:** masking is only as trustworthy as the
config fact that says "this channel has no CT fitted." If that fact is
wrong — an operator later clamps a real CT onto channel 0's jack without
also updating whatever field the new topology-aware gate reads to decide
"fitted" — the mask would then suppress a **real** presence signal on a
channel that is, at that point, no different from channel 3 today. This is
the same class of risk that already exists for the board-wide
`ct_installed` flag (§0.1's own "Answering *no* on a board that does have
CTs silently disarms S3, S4, S9 and S14" warning) — not a new failure mode
this change introduces, but worth naming explicitly since the fix
generalizes that existing single board-wide answer into one answer per
channel, multiplying the number of places the answer can go stale. The
mitigation is procedural (the commissioning-check discipline §5 already
prescribes: "Command **one** relay on... Confirm exactly one channel
responds"), not a firmware guarantee — this audit does not have visibility
into whatever mechanism the parallel change uses to keep the per-channel
"fitted" answer synchronized with physical reality, and that mechanism is
worth reviewing on its own once that change lands.

**Net for S3 and S9 specifically:** masking removes zero genuine detection
capability from either guard as long as the "unfitted" fact is accurate.
For S9 in particular (the guard the coordinator flagged as the reason this
must land in the same change as the commissioning-gate relaxation): without
masking, relaxing `current_sensing_commissioned` to tolerate a
one-or-two-CT topology would let S9's streak-and-latch progress on a board
where the *third* (never going to be fitted) channel's phantom floor
crosses 25 counts — an unfitted channel's offset noise reaching the
unclearable `TRIP_INEFFECTIVE` latch, which is a strictly worse outcome
than today's board-wide "all three or none" gate, since today's gate at
least fails toward the safe non-latching WARN on any uncommissioned board.
Masking closes exactly that regression.

## 5. What this does and does not put at risk

- **Not a missed-hazard direction.** S3's own design intent (per
  `current_presence_policy.h`'s safe-direction reasoning) is to bias toward
  false positives rather than false negatives, because a false negative on
  the welded-SSR guard is the dangerous failure mode. A false trip from an
  unfitted channel's floor is the *accepted* trade-off working as designed
  for a channel that IS supposed to be commissioned eventually — the gap is
  that this fallback was written for "not yet commissioned," and channels
  0/1 on this board are "never going to be commissioned, because nothing
  is plugged in," which is a different situation the code does not
  distinguish.
- **Real consequence if it fires:** S3 trips the whole board
  (`SAFETY_TRIP_LOAD_STUCK_ON` is a full safety trip, not a per-channel
  warning), halting whatever firing is in progress, off a channel with no
  physical current path at all. That is a nuisance/availability hazard —
  it can ruin a firing and requires an operator to notice, diagnose (a trip
  reason of "current present, nothing commanded" pointing at hardware that
  was never populated is actively misleading), and clear the trip — not a
  safety-in-the-dangerous-direction hazard. It does not weaken S9's
  weld-detection (protected, see table above) or any other guard's ability
  to catch a real fault.
- **Likelihood is not established by this audit.** Whether the floor
  actually drifts by 8+ counts over a real firing has not been measured —
  this is a source-and-documentation analysis of the mechanism and the
  margin, not a bench measurement of thermal drift over hours. The 8-9
  count number is today's snapshot at room temperature with the kiln cold.

## 6. Candidate remedies (not implemented — analysis only)

- **Add a per-channel "unfitted" concept distinct from board-wide
  `ct_installed`, and mask that channel out of `current_any_present()`.**
  This is the remedy identified as correct by §4 above, and is the one
  now being implemented in parallel (topology-aware
  `current_sensing_commissioned` plus per-channel presence masking). §4's
  analysis found no genuine detection capability lost by masking, given the
  circuit's own DC-return topology (`R90`) makes an unfitted channel's
  reading pure offset/leakage by construction — and found one residual,
  non-eliminated risk worth reviewing once that change lands: keeping the
  per-channel "fitted" answer synchronized with what is actually clamped
  onto each jack, the same provenance problem §0.1 already documents for
  the board-wide flag, now multiplied per channel. This audit did not
  design or implement that change and takes no position on its concrete
  config-schema shape.
- **Write a nonzero `zero_counts` for the unfitted channels anyway**, just
  to eat the margin. Rejected as a *recommendation*, not merely deferred:
  `zero_counts` is documented (`CURRENT_SENSE.md` §5) as "measured... with
  the CT fitted and no primary current" — it is a commissioning value with
  a specific physical meaning, "what does this exact sensor chain read at
  true zero." Writing one for a channel with **no sensor chain to measure**
  is not commissioning, it is fabricating a number that looks like
  calibration evidence but isn't — the auto-zero routine
  (`current_task_ct_auto_zero_begin()`) would in fact happily average the
  same DC-offset-floor samples this report is worried about and call the
  result "commissioned," which converts an honest "uncommissioned, treat
  with suspicion" state into a dishonest "commissioned, trust this" one
  while changing nothing about the physical absence of a sensor. That is
  arguably worse than the current state, not better: it would also flip
  `current_sensing_commissioned` true once all three channels have *a*
  `k_ct_v_per_a` and `zero_counts`, which changes S9's behavior for a
  channel that still has nothing plugged into it.
- **Raise the fallback margin** (`CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS`,
  currently 25) globally. Rejected as a global change: it weakens the
  fallback for every currently-uncommissioned-but-genuinely-wired
  installation (the case this constant was designed for, per
  `current_presence_policy.h`'s own comment), trading a real board's S3/S9
  sensitivity for a problem specific to unfitted channels on this one bench
  unit.
- **Do nothing, treat this as a known bench-only condition** and rely on an
  operator to notice a spurious S3 trip, check `docs/audits` (this file),
  and understand it points at channels 0/1 rather than a real stuck relay.
  Workable only as a stopgap given the trip reason string
  (`"current present with nothing commanded on"`) does not itself say
  *which* channel triggered it — worth checking whether the trip detail
  already names a channel; if not, that is a smaller, independent
  diagnosability gap worth closing regardless of which remedy above (if
  any) is chosen.

None of the above is implemented by this audit — this is a read-and-analyze
pass only, per task scope. No `.c`/`.h`/`.ps1`/`.kicad_*` file has been
modified.

## Files read

- `firmware/SaftyFW/src/current_presence_policy.c`,
  `firmware/SaftyFW/src/current_presence_policy.h`
- `firmware/SaftyFW/src/safety_guards.c`, `firmware/SaftyFW/src/safety_guards.h`
- `firmware/SaftyFW/src/snapshots.h`
- `firmware/SaftyFW/src/tasks/safety_core.c` (lines ~420-452, ~1105-1155)
- `firmware/SaftyFW/docs/CURRENT_SENSE.md`
- `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md` (bench idle-count data)
