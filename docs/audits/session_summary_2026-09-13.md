# Session summary, 2026-09-13

Successor to `docs/audits/session_summary_2026-09-11.md` — read that one
first for everything landed through 2026-09-11. Pointer document only, same
convention: see the individual audits under `docs/audits/` for full detail.
This entry covers what landed after that summary was written and brings
`ROADMAP.md`/`docs/FUZZY_CONTROLLER_PLAN.md` up to date with it.

**Superseded in part by `docs/audits/session_summary_2026-09-14.md`**, which
folds in this sweep's "in flight" items (the fuzzy overshoot measurement and
its review, the scorecard fix sequence) and records an owner decision on the
autotune-derived fuzzy bands — read that document for the current state of
anything this one marked open.

## Verification performed

- `git cat-file -t` confirmed `c9ce6b7c`, `ed854ac5`, `ba230bca` and
  `8a12521b` are all real commits in this repository before citing them
  anywhere.
- Diffs of all three original commits read as claimed in their commit
  messages (checked directly, not taken on trust): `c9ce6b7c` only touches
  `docs/audits/coupling_level_schedule_adjudication_2026-09-11.md` (no
  production code); `ed854ac5` adds a new host-test executable and touches
  `build_host_tests.ps1`'s registration, `pid_fuzzy.c` unchanged (empty
  `git diff` after its own negative test, per the commit message); `ba230bca`
  adds one audit doc only.
- **`8a12521b`, a same-day adversarial review of all three plus `fbdc5bd0`,
  overturned or narrowed two of the three findings below after this document
  was first drafted** (`docs/audits/review_sim_fuzzy_commits_2026-09-13.md`).
  This document has been corrected in place to match; see each item's own
  paragraph for what changed and why.

## Landed since the 2026-09-11 summary, by area

**Coupling model** — `c9ce6b7c` ran the flat-coupling-scale discriminator
section 9 of `coupling_level_schedule_adjudication_2026-09-11.md` proposed but
had not run, reporting a flat 1.173x multiplier as matching the first
level-schedule attempt's headline A1 count (38/660) but not its cluster shape,
and called the result PARTIAL. **`8a12521b`'s review found this PARTIAL
verdict itself overstated**: against the pinned rate (24/660), sampling
statistics put {18, 20, 24} in one indistinguishable cluster (<=1.25 sd apart)
and 38 only marginally elevated (~1.8 sd, p~=0.07, not established as real);
the per-subscore cluster-shape argument (splits of 2-vs-11, 22-vs-27) is
below resolution the same way. The factor-1.0 control did reproduce the pin
exactly, so the experiment's mechanics are trustworthy — its conclusion is
not. Correct reading: **inconclusive**, not PARTIAL. The pin stays at
24/660; the level-scheduled coupling class's "explain the sign reversal
before a third attempt" gate is unchanged.

**Fuzzy controller** — `docs/FUZZY_CONTROLLER_PLAN.md`'s Stage 0 ((v-a), the
offline nine-cell probe) ran: `ed854ac5` recorded the centre cell's exact
effect — the only cell ever observed on real hardware, 100% of a
2178-sample mode-3 capture — as kp x0.75/ki x1.25/kd x0.75 at strength 50
(x0.875/x1.125/x0.875 at 25); **this multiplier finding stands.** Its
separate headline, "6 of 9 rule cells unreachable" (built on a ~0.083 degC/s
"measured plant maximum"), does **not** stand: `8a12521b`'s review traced
that figure to an unmeasured profile-rate conversion (300 degC/hr / 3600),
contradicted by the same module's own recorded real peak of 0.110 degC/s —
**do not cite a reachability count for this probe.** A follow-on comparison,
not originally scoped as part of Stage 0, then asked whether the centre-cell
effect is "real" or just a disguised fixed retune: `ba230bca` originally
reported fuzzy-on beating an equivalent always-on flat retune by ~7.8% IAE
(0.22 degC MAE). **That result has been retracted by the same review**: the
fuzzy-ON arms were measured against a stale executable built minutes earlier
while `ed854ac5`'s own negative test had the centre cell's rule sign
inverted — the source was hand-restored with an empty `git diff`, but the
poisoned binary was never rebuilt before being measured. Rebuilt from
tracked source, fuzzy vs. an equivalent flat retune is a **0.011 degC MAE**
gap at strength 50 and **reverses sign** (flat retune slightly better) at
strength 25 — indistinguishable from a fixed multiplier on this scenario.
Net effect on `docs/FUZZY_CONTROLLER_PLAN.md`'s five-option analysis:
**neither 2026-09-13 result changes the standing position** — option (iv)
does not gain a new argument and does not lose one either; nothing measured
on this bench clears the 0.5 degC materiality line in either direction on
any option, and the deciding evidence for this design space cannot come from
this bench fixture, since its plant is capped ~40 degC above ambient with a
real measured peak ramp rate well under the rate band the off-centre cells
need. A process finding from the same review, worth carrying forward: a
negative test that hand-restores source and proves an empty `git diff` can
still leave a **built artifact** poisoned — a full rebuild must follow any
negative test before anything downstream is measured against it (same class
as `project_idf_build_silently_noops_under_msys` and
`project_move_item_force_not_atomic`).

**Other work in progress, not describable here** — three new audit docs
(dimensionless fuzzy bands, adaptive_tune confidence/authority design, gain
scheduling design) are being written by a concurrent pass as of this sweep.
Their conclusions are unknown to this document and are not cited, described,
or guessed at here — check them directly once that pass completes.

## Still OPEN

- Everything listed OPEN in the 2026-09-11 summary that is not explicitly
  closed above remains open (in particular: `36f88d62`'s zones-save-zeroes-
  anchor finding needs its regression-test confirmation, and the fuzzy
  controller's architecture decision is not yet made).
- The level-scheduled coupling class's "explain the sign reversal before
  retrying" gate — unchanged by `c9ce6b7c`, which tested a different
  (non-scheduled) hypothesis and, on review, is inconclusive rather than a
  resolution.
- `docs/FUZZY_CONTROLLER_PLAN.md` Stage 2 (the (iii)-vs-(ii)-vs-(iv) decision
  point) — Stage 0 is done, Stage 1 ((v-b), fixing the shared-setpoint finding
  D) has not been reported done, and Stage 2 has not been reached.
- The coupling sign reversal at 62-75 C remains **unexplained**; no
  literature source found reports one. Do not treat `c9ce6b7c` as progress
  toward explaining it — it tested a magnitude/shape question, not the sign
  question.

## UNVERIFIED ON HARDWARE

Unchanged from 2026-09-11, restated because it bears directly on how much
weight either fuzzy result above should carry: **all of the following remain
dormant on the live board**, so nothing landed since 2026-09-11 changes what
is actually running on the physical kiln today.

- `approach_rate_cap_c_per_hr = 0.0` on all three zones — the paired-setpoint
  fix (`7d76d8fc`) only changes behaviour once this is non-zero.
- `fuzzy_strength_pct = 0.0` on all three zones — by `pid_fuzzy_adjust()`'s
  own documented contract this reproduces base PID bit-for-bit, so **neither
  of this sweep's fuzzy findings has ever been observed on real hardware** —
  both `ed854ac5` and `ba230bca` are host-test/sim results only.
- `adaptive_tune enabled=False` on all three zones (`observations_lifetime=0`)
  — the K_dc ratchet fix (`97288659`) and the POST-wipe fix (`36f88d62`) are
  both validated only by host tests/sim.
- The RP2040 fault-hook diagnostic chain (`8267fab2`) has still never been
  observed end-to-end on hardware — host tests and the mirror-drift check
  exercise it, not an actual triggered fault on the bench Pico.

None of this is a defect in the work itself — it is a statement about what
evidence exists, so that a future sweep does not read "fixed" or "measured"
as "confirmed on the board."

## Check suite

`tools/run_all_checks.ps1` reported **94/94** at the time of this sweep (see
this document's own commit for the exact run). Attribute any regression seen
in a later run to whichever commit's `git log` timestamp precedes it, not to
this document.
