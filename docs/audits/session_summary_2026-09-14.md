# Session summary, 2026-09-14

Successor to `docs/audits/session_summary_2026-09-13.md` (which is itself
successor to `docs/audits/session_summary_2026-09-11.md` — read both first).
Pointer document only, same convention: see the individual audits under
`docs/audits/` for full detail. This entry covers what landed after the
2026-09-13 summary was written, folds in results that were "in flight" as of
that sweep's ROADMAP.md header, and records an owner decision. No board was
flashed, no heating run was performed, no controller behaviour was changed
while writing this document.

## Verification performed

`git cat-file -t` confirmed every hash cited below is a real commit in this
repository: `2c49465a`, `c002ceaf`, `2edbb6eb`, `d41da85f`, `9a9afb25`,
`560cffe0`, `97288659`, `36f88d62`, `e78fbc5b`, `0dbd7c6d`, `a57ca6f9`,
`375c9258`, `1570a65a`. Each commit's diff/message was read directly before
being cited, not taken on trust.

## OWNER DECISION — autotune-derived fuzzy bands ship

**Decision, not a proposal.** The owner chose to keep `2c49465a`'s
dimensionless derivation — `rate_band = model_k_dc / model_tau_s`,
`error_band = model_k_dc * 0.5` — sourced from each zone's own autotune
model, replacing `pid_fuzzy.c`'s previous absolute constants
(`ERROR_BAND_C_DEFAULT`/`RATE_BAND_C_PER_S_DEFAULT`, 20.0 °C / 0.5 °C/s,
sized by desk reasoning about "a mid-size kiln"). The absolute constants are
retained **only** as an explicitly-logged fallback path for a zone that has
never been autotuned. Rationale: this satisfies the standing requirement
that nothing ship guessed for, or tuned to, a kiln other than the installed
one, at no material cost — per `docs/FUZZY_CONTROLLER_PLAN.md` §2(i), the
band normalisation is "a real correctness improvement regardless of
anything else," and it is orthogonal to finding (A) (the layer's lack of
demonstrated benefit, restated below), which concerns the rule table and
strength, not the axis units. **Owner requirement "bands must come from the
installed kiln's own measurement, not a guessed constant" is now MET** for
any zone that has been autotuned.

This does not change the recommendation in `docs/FUZZY_CONTROLLER_PLAN.md`
§4.1 (adopt (v), expect to land on (iii)+`adaptive_tune`, dispose of the
fuzzy layer under (iv) later) — band-unit correctness and rule-table/
strength-authority design are separate axes, and the plan already treated
adopting (i)'s band derivation as compatible with any downstream verdict on
the rest of the layer.

## The fuzzy layer has no demonstrated benefit — restated plainly

`docs/audits/fuzzy_overshoot_measurement_2026-09-13.md`, including its
appended `1570a65a` review, together refute the headline this sweep
inherited "in flight." Do not cite the original headline (fuzzy beats an
equivalent flat retune by ~0.42 °C overshoot and 40–80 s settle time)
without also citing the review that overturns it:

- The document's "equivalent fixed retune" arm (kp ×0.75 / ki ×1.25 / kd
  ×0.75) is fuzzy_50's **centre-cell maximum** — reachable only when error
  and rate are both exactly zero — not its time-average. Fuzzy_50's actual
  ramp-phase mean is kp ×0.8684 / ki ×1.1618 / kd ×0.8382, roughly half the
  comparison arm's perturbation where it counts.
- A fifth arm, a plain static gain rescale at fuzzy_50's own measured
  ramp-phase average with `strength_pct=0` (no inference in the path at
  all), reproduces fuzzy_50 on all four of the owner's objectives inside
  the stated materiality bars (0.164 vs 0.148 °C overshoot, −0.253 vs
  −0.179 °C undershoot, 0.119 vs 0.130 °C steady RMS, 195 vs 200 s settle).
  The overshoot/undershoot penalty tracks gain-change magnitude
  **monotonically** across all five arms with no discontinuity at the
  inference/no-inference boundary.
- The claimed 0.42 °C overshoot advantage came entirely from dwell 2's
  ramp-*down* residual (the measurement was still above target when the
  dwell began) — the real delta, at the two dwells where overshoot is
  actually overshoot, is **+0.15 °C** (fuzzy worse, not better).
  Sub-materiality either way per the standing 0.5 °C threshold, but the
  headline number was mislabelled.
- The settle-time claim inverts at a principled 0.5 °C band (the owner's
  own materiality scale, vs. the document's chosen 2.0 °C band): the retune
  arm settles 205 s **slower** than the control at 0.5 °C, and fuzzy_50
  also inverts (40 s slower) at 0.25 °C.

**Net: the fuzzy layer remains indistinguishable from a gain change of the
same magnitude.** This is not a new conclusion — it is finding (A) from the
2026-09-13 pass, now measured directly on the owner's four-part objective
rather than IAE/MAE, and it survives adversarial review.

**`docs/FUZZY_CONTROLLER_PLAN.md` §2(iv) updated accordingly.** The
2026-09-13 update text in that section, which reported the "for" case for
deleting the layer as *weakened* by the (since-retracted) IAE comparison,
is **withdrawn** — that weakening depended on the ~7.8% IAE headline, which
`1570a65a` retracted for the same reason above (mismatched comparison-arm
strength). Option (iv)'s "for" case stands as originally written; the
five-option structure in §2 is otherwise unchanged — this is a correction to
one option's argument, not a re-ranking.

## Ramp tracking closed against the fuzzy layer — verified, unchanged

`c002ceaf` (already reflected in `docs/FUZZY_CONTROLLER_PLAN.md`
§0.0.1/§4.3): the two ramp-lag rule cells are `{kp +1, ki 0, kd 0}`, but
steady-state ramp-following error is set by `Kv = Ki·P(0)` — a lever the
rule table never touches — and the fuzzy layer has no access to
`d(setpoint)/dt` at all (only instantaneous error and rate-of-actual). This
finding is closed, not open; the feedforward climb term is the correct
mechanism for this objective, not any change to the rule table.

## Scorecard defect found and fixed

`firing_score.c` had no settle-time instrument and clamped undershoot to
zero — both accept-permissive gaps, meaning both classes of true regression
could pass silently. Sequence, each verified against its own commit:

- `2edbb6eb` — re-examined seven reverted ramp/overshoot decisions against
  the four-part objective; found the scorecard defect (analysis only, no
  code touched, no board flashed).
- `d41da85f` — instrumented settle-time and undershoot in `firing_score.c`,
  raising `FIRING_SUBSCORE_COUNT` 3 → 6.
- `9a9afb25` — adversarial review found that raising the enum count **silently
  enrolled** the three new, unvalidated axes into `firing_compare`'s verdict
  (it looped to `FIRING_SUBSCORE_COUNT`), shifting the historical corpus's
  REJECT count 21→26 and INSUFFICIENT 615→610 while ACCEPT held at 24 only by
  cancellation.
- `560cffe0` — fixed the enrolment: voting is now an explicit mask, with
  `_Static_assert`s making an unclassified axis a compile error and a signed
  axis unable to vote (undershoot and the settle-time-vs-target delta are
  signed/diagnostic, not votable as written). A1 restored to the pinned
  **24/21/615**.

**Settle band is 2.0 °C**, chosen from measurement across 33 real dwell-zone
instances — this is a corpus-derived constant, not the 0.5 °C figure from
`fuzzy_overshoot_measurement_2026-09-13.md`'s Finding 2 above, which is a
narrower critique of that document's own bench-scale harness band, not of
`firing_score.c`'s corpus-fitted one. Do not conflate the two 2.0 °C-vs-0.5 °C
discussions — they are different instruments measuring different things.

## Ratchet fixes and observability

- `97288659` — anchored the `adaptive_tune` K_dc ratchet's plausibility
  bound to the zone's original autotune baseline rather than the live
  adapted value (closes the "bound relative to persisted state" class,
  `project_bound_relative_to_persisted_state.md`).
- `36f88d62` — adversarial review of `97288659` found an ordinary whole-page
  zones config save silently zeroed the new anchor field (every POST field
  with no explicit key built from a caller-zeroed scratch struct); fixed.
- `e78fbc5b` — closed a live Ki ratchet loop under `PID_FUZZY`:
  `adaptive_tune_ki.c` diagnosed corrections from the *effective* closed-loop
  trace but wrote them against the *reference* (pre-fuzzy) gain, producing an
  effective-vs-reference divergence that compounded ~1.2x per run and would
  have hit the 5x plausibility bound after 9 runs. Fixed by withholding the
  Ki correction while fuzzy is active. **Consequence: Ki adaptation and the
  fuzzy layer are now mutually exclusive by design** — this is a real
  constraint on any future combination of the two, not an oversight.
- `0dbd7c6d` — exposed `autotune_baseline_k_dc` (the `97288659` anchor,
  `ZONES_CFG_VERSION` 25→26, protected from whole-page-zero by `36f88d62`)
  on `GET /api/zones`, closing the hole where the anchor's own value was
  unobservable from outside the firmware.

## Open items — no outcome asserted

Recorded honestly as open, not resolved, not trending either way absent
further evidence:

- **Coupling sign reversal** — unexplained; no literature reports one.
- **Level-scheduled coupling class** — failed twice; the written
  do-not-retry-without-explaining gate from the 2026-09-13 sweep stands
  unchanged.
- **Pico heat-start reboots** — closed-pending-recurrence, not
  root-caused.
- **`ease_off_window_mult` hardware A/B** (`a57ca6f9`) — z0=1.5x/z1=2.0x/
  z2=3.0x on one profile-7 firing was **INCONCLUSIVE**, confounded by a
  within-run zone confound (the three zones were not run as independent
  arms). The shipped default of 2.0 stands unchallenged by this experiment.
- **RP2040 fault-hook chain** — still never observed end-to-end on real
  hardware; host tests and the mirror-drift check exercise it, not an
  actual triggered fault on the bench Pico.
- **Zones JSON headroom** — confirmed **161 bytes** free (7199/7360) against
  the response's cap; plan at
  `docs/audits/zones_json_headroom_plan_2026-09-14.md` (`375c9258`).
  **Enlarging the buffer is forbidden** — the plan is about reducing what
  goes into the existing cap, not raising the cap.

## Two structural items parked deliberately

- The fuzzy guard in `adaptive_tune_ki.c` (`e78fbc5b`) reads
  `control_mode`/`fuzzy_strength_pct` **at refine time**, not a snapshot
  taken at capture time — today this is safe only because another module's
  interlocks happen to prevent the two from disagreeing mid-cycle, not
  because the guard itself is time-consistent.
- `adaptive_tune_ki.c`'s guard fails **open** if
  `zones_config_get_control_mode()` returns false (i.e., on a config-read
  failure it behaves as though fuzzy is off and applies the correction
  anyway) — a defensible default for availability, but worth naming
  explicitly rather than leaving implicit.

Neither is fixed in this sweep; both are recorded so a future session does
not have to rediscover them.

## UNVERIFIED ON HARDWARE

Restated because it bears directly on how much weight every fuzzy-related
and ratchet-related finding above should carry: **all of the following
remain dormant on the live board as of this sweep**, unchanged from
2026-09-11 and 2026-09-13 —

- `fuzzy_strength_pct = 0.0` on all three zones — by `pid_fuzzy_adjust()`'s
  own documented contract this reproduces base PID bit-for-bit, so **none**
  of this sweep's fuzzy findings (the band-derivation decision, the
  overshoot-measurement refutation, or the ramp-tracking closure) has ever
  been observed on real hardware. All are host-test/sim results only.
- `approach_rate_cap_c_per_hr = 0.0` on all three zones — the paired-input
  fix referenced in earlier sweeps only changes behaviour once this is
  non-zero.
- `adaptive_tune enabled=False` on all three zones
  (`observations_lifetime=0`) — the K_dc ratchet fix (`97288659`), the
  POST-wipe fix (`36f88d62`), and the effective-vs-reference Ki fix
  (`e78fbc5b`) are all validated only by host tests/sim; none has run a
  single real adaptation cycle on the board.
- The RP2040 fault-hook diagnostic chain has still never been observed
  end-to-end on hardware (see "Open items" above).

None of this is a defect in the work itself — it states what evidence
exists, so a later sweep does not read "fixed" or "measured" as "confirmed
on the board."

## Check suite

`tools/run_all_checks.ps1` reported **94/94** at the time of this sweep (see
this document's own commit for the exact run). Attribute any regression seen
in a later run to whichever commit's `git log` timestamp precedes it, not to
this document.
