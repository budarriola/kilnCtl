# Can the fuzzy layer also help keep the ramp on track? (2026-09-13)

Question from the owner: the fuzzy layer (`pid_fuzzy.c`) exists to reduce
over/undershoot; PID alone is expected to settle correctly. Is ramp tracking
a second objective the same layer can serve?

**Bottom line up front:** no, not with its current inputs. The rule table's
response to a sustained ramp-lag signature only pushes `Kp`, and `Kp` is not
the gain that governs steady-state ramp-following error in this loop
structure — `Ki` is, and the two ramp-lag cells leave `Ki` untouched. The
mechanisms this codebase already has (feedforward climb term, and the unused
setpoint-weight `b`) are the right tools for this problem; the fuzzy layer
adds nothing to it and should not be extended to try.

## 1. What the rule table actually does on a ramp

A steady, well-tracked ramp reads as **persistent error, ~zero error rate**
on this layer's two inputs (`pid_fuzzy.c`'s own header comment already
establishes this: real ramp rates land near zero on the rate axis because the
signal is `-d(measurement)/dt`, not `d(setpoint)/dt`, and even a brisk
300 °C/hr ramp is only 0.083 °C/s in magnitude against a 0.5 °C/s default
band). That lands in one of two symmetric cells: (error=NEG, rate=STEADY) or
(error=POS, rate=STEADY) — rows 0 and 2, column 1 of `RULE_TABLE`
(`firmware/KilnFW/App/drivers/control/pid_fuzzy.c:149-162`):

```
/* error = NEG (large, overshoot) */
{1.0f, 0.0f, 0.0f},  /* rate STEADY: steady overshoot -> push harder */
...
/* error = POS (large, undershoot) */
{1.0f, 0.0f, 0.0f},   /* rate STEADY: steady approach -> push harder */
```

Both cells are `{kp: +1, ki: 0, kd: 0}` — confirmed against the real function
(not by hand) in `docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`
(commit `ed854ac5`, its per-cell table unaffected by the reachability
correction in `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`, commit
`8a12521b`): cells #2 and #8 both show `ki x = 1.0/1.0` (no change) and `kp x
= 1.125/1.25` at strength 25/50. So the layer's entire response to sustained
ramp lag is "raise Kp up to +25% (at strength 50), leave Ki and Kd exactly
where they were."

**Is that the right lever?** For a PI(D) loop tracking a ramp reference
against a plant with no free integrator of its own (this is a FOPDT plant —
`k`, `tau`, dead time, no integrator), the classical result is that the
*steady-state ramp-following error* is set by the loop's velocity constant
`Kv = lim_{s->0} s·C(s)·P(s)`. With `C(s) = Kp + Ki/s`, the `Kp` term
contributes `s·Kp·P(s) -> 0` as `s->0`, so only the `Ki/s` term survives:
`Kv = Ki·P(0)`, and the steady ramp error is `ramp_rate / Kv = ramp_rate /
(Ki·P(0))`. **This depends on `Ki`, not `Kp`.** Raising `Kp` while holding
`Ki` fixed does not reduce the steady-state lag a ramp settles into — it
mainly changes the *transient* approach to that steady lag and, on a dead-time
plant, buys a little proportional tightening at the cost of reduced phase
margin, not a lower asymptotic ramp error.

So: the (error, STEADY) cells fire on exactly the signature a lagging ramp
produces, but the response they encode (`Kp` up, `Ki` unchanged) is tuned for
a *different* problem — a large, not-yet-recovering error near a setpoint
change (step-response overshoot/undershoot), where raising `Kp` genuinely
helps close the gap faster. Applied to a ramp, it is not the wrong sign, but
it is the **wrong lever**: at best a small, transient nudge; it does not
touch the term that actually sets ramp-following error. This is a
structural mismatch, not a tuning-constant problem — no `strength_pct` or
band re-scaling fixes it, because the cell would have to redirect its `Ki`
consequent, which changes what the cell does for the (more common,
already-characterized) overshoot case it was designed for.

## 2. Can error+error-rate fuzzy inference represent ramp lag at all?

Only weakly, and only through the error axis. A perfectly-tracked ramp and a
ramp lagging by a constant offset `e_ss` differ almost entirely in **steady
error**, not in its derivative — both have error rate near zero once the lag
has settled to its asymptotic value (`d(error)/dt -> 0` as the loop reaches
whatever steady lag the `Kv` above imposes). The rate axis in this
implementation additionally measures `-d(measurement)/dt` rather than
`d(error)/dt` — it has no access to `d(setpoint)/dt` at all (`pid_fuzzy.c`
lines 14-24 document this explicitly: "this signal ignores dSP/dt"). So the
inference system's rate input is not even measuring ramp-tracking quality;
it is measuring the plant's own climb rate, which is large and roughly
constant for the *entire* ramp regardless of whether tracking is good or bad.
The **only** signal in this system that varies with tracking quality during
a ramp is the error magnitude itself, landing in the ordinary large-error
cells — which is exactly why this reduces to "cell 2/8 fires and pushes
`Kp`," per §1. A genuine trajectory-tracking fuzzy scheme would need
`d(setpoint)/dt` (or equivalently the commanded ramp rate) as an input,
which this design does not have and was never built to have — it is a
disturbance-rejection design (PID_EXPANSION_PLAN.md's stated purpose), not a
trajectory-tracking design.

## 3. Comparison with what this codebase already has

### Feedforward climb term
`pid.c` already computes a feedforward output split into `ff_hold` (steady
state duty to hold the current setpoint) and a `climb` component (duty needed
to sustain the commanded ramp rate against the plant's own thermal mass),
added directly to `u` (`pid.c:138,202`, `.ff = ff_hold + climb` conceptually
per the surrounding comments) — this is precisely the standard fix for ramp
lag: feedforward on the setpoint's own derivative (the ramp rate) rather than
reacting to the error it would otherwise cause. This is the textbook answer
to "how do I track a ramp without steady lag," and it is uncoupled
(per-zone, not the coupled multi-zone form) after `project_feedforward_climb_uncoupled`
and the explicit **rejection** of a lead-compensation redesign
(`project_coupling_lead_comp_rejected`: the design reduces to the reverted
uncoupled formula at `t=0`, so it was not adopted). The integral floor was
also specifically reworked (`e690f8a6`, "Floor the PID integral at -ff_hold,
not -ff_u, so a ramp keeps its climb term") so the integrator cannot cancel
out the climb feedforward it's supposed to be providing — i.e. this project
has already done real, hardware-validated engineering on exactly this
problem, through the feedforward path, not the fuzzy layer.

**Scope note:** a separate opus-level pass has been dispatched specifically to
re-examine whether the ramp ideas below were reverted for the right reasons.
This document does not adjudicate that question — it only records what was
tried and the stated reason for the outcome, for that pass to use.

Two adjacent ramp-related attempts and one adjacent change, found in git log
around the climb term:
- Decaying the climb feedforward at dwell entry over dead time instead of
  stepping it to 0 (`a77db88c`, `b8b192db`) — later reverted, with the
  revert commit's stated reason (`f9d84453`, "Revert the dwell-entry climb
  decay: hardware says it made overshoot worse") on all zones, and per that
  commit message, guard 2 was also tripped.
- Scaling the climb term down for zone 0 specifically (`a7c9a882`, "The
  climb term drove zone 0 ten times harder than it needed") — this commit
  was kept (not reverted); cited here only to show the climb term has other,
  separate, non-reverted tuning history.
- The integral floor change (`e690f8a6`, "Floor the PID integral at
  -ff_hold, not -ff_u, so a ramp keeps its climb term") — also kept; changes
  what the integrator is allowed to cancel, not the climb term's own shape.

### Setpoint weighting (`b`)
`pid.c:34,131`: `p_term = kp * (b*setpoint - measurement)`. The mechanism
exists, matching the classical two-degree-of-freedom PID literature (Hägglund
& Åström-style `b`/`c` weighting, and the fuzzy-tuned variant in Visioli,
"Fuzzy logic based set-point weight tuning of PID controllers," IEEE
Trans. Systems, Man, and Cybernetics, 1999,
https://ieeexplore.ieee.org/document/798062/ — I could not retrieve the full
text through WebFetch, only the search-result abstract snippet, which
describes a fuzzy inference system that sets `b` from error and its
derivative specifically to trade off overshoot against rise time on
**setpoint steps**; mark this as abstract-only, not independently verified
here).

In this codebase, `b` is currently **fixed at 1.0** (classic PID, no
weighting): `PID_SETPOINT_WEIGHT_B` is `#define`d to `1.0f`
(`firmware/KilnFW/App/drivers/control/profile_executor_internal.h:251`) and
wired straight into every zone's PID config
(`firmware/KilnFW/App/drivers/control/profile_executor_run.c:569`). It is
not tuned, not exposed to config, and not adaptive.

Setpoint weighting, however, is the wrong tool for *this* objective too, by
the same argument as §1: `b` scales the setpoint's contribution to the
**proportional** term only. For a steady ramp, once the loop has settled into
whatever asymptotic lag `Kv = Ki·P(0)` imposes, `b` changes the size of the
transient at the *start* of the ramp (or at a setpoint step) but does not
change the asymptotic ramp-following error, which is set by `Ki` alone, not by
how the P-term's setpoint contribution is weighted. This matches the general
two-degree-of-freedom PID literature description found this session (search
result summarizing setpoint weighting's role): `b`/`c` factors are described
as overshoot/step-response shaping tools, and the standard remedy quoted for
"the most demanding setpoints have the form of ramps" is precisely
feedforward against the ramp's own derivative, not setpoint weighting on the
P-term. So `b` is a real, present, currently-inert-at-default mechanism for
*step overshoot* shaping (see §4), not a ramp-tracking mechanism — it is not
a substitute for the feedforward climb term this project already uses for
ramp tracking, and is not proposed as one here.

### Summary comparison

| Mechanism | Present in this codebase? | Targets ramp lag? | Governing parameter |
|---|---|---|---|
| Feedforward climb term | Yes, tuned, hardware-validated | Yes — the correct tool | `climb` duty term, per-zone `model_k_dc`/`model_tau_s` |
| Setpoint weighting `b` | Yes, present but fixed at 1.0 (inert) | No — shapes step transients, not ramp steady-state | `Ki` still governs ramp error regardless of `b` |
| Fuzzy layer (error, STEADY rate) cells | Yes, live | No — pushes `Kp`, the wrong lever for ramp `Kv` | Would need to touch `Ki`, and would collide with the overshoot role these cells already play |

**Honest answer to the framing question:** the mechanism this project already
has (feedforward climb, tuned per-zone, with the integral floor fixed to
protect it) is the right tool for ramp tracking, and the fuzzy layer adds
little to nothing for that objective given its current inputs (error and
`-d(measurement)/dt`, no access to the commanded ramp rate).

## 4. Does anything found here also help the primary objective — overshoot?

Two things, both worth flagging even though they don't touch ramp tracking:

- **Tuning `b` away from 1.0** is a live, unexplored knob for the *actual*
  stated objective (overshoot/undershoot at dwell entry and setpoint steps),
  independent of the fuzzy layer, already wired into `pid.c`'s math, currently
  just hardcoded off. A fixed `b` in the classical 0.2-0.8 range (per the
  general PID literature convention quoted in search results: "b=1 means
  proportional control using the error... in general the setpoint weight is
  reduced to prevent large transients") reduces the P-term's kick at a setpoint
  step/dwell transition without touching disturbance rejection, since `b`
  only affects the *reference* path, not the loop's response to load
  disturbances or plant errors. This is a candidate for the actual objective,
  not for ramp tracking, and is called out here only because the owner's
  question surfaced it as a currently-inert mechanism.
- The furnace-control literature surveyed (GA-Fuzzy-PID for vacuum annealing
  furnace, PMC, https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10686503/;
  2-DOF PIDA for electric furnace, PLOS ONE,
  https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0334594
  and PMC, https://www.ncbi.nlm.nih.gov/pmc/articles/PMC12520383/ — both
  found via search, summaries only, not independently fetched in full)
  converges on the same architectural answer this project already reached
  independently: separate the servo (setpoint-following) and regulatory
  (disturbance-rejection) paths rather than asking one adaptive gain scheme to
  do both. That is structurally what the feedforward climb term (servo path)
  plus the fixed PID/fuzzy disturbance-rejection loop (regulatory path)
  already is here — the fuzzy layer already stays out of the servo path
  today except through the two (error, STEADY) cells discussed in §1, which is
  the one place it does leak into ramp-following via a `Kp` nudge that (per
  §1) doesn't actually help. No new fuzzy mechanism found in the literature
  targets overshoot in a way this project hasn't already tried (see the prior
  literature review below) or already ruled out via the reverted dwell-entry
  climb-decay experiment.

## 5. Literature: what it says, filtered for ramp vs. step

Search terms used: "fuzzy PID ramp tracking", "fuzzy gain scheduling setpoint
tracking furnace", "fuzzy PI temperature ramp rate control kiln", "fuzzy
logic controller reduce overshoot temperature furnace", "fuzzy self-tuning
PID trajectory tracking", "type-2 fuzzy temperature control", plus targeted
follow-ups on setpoint weighting and feedforward. All results below are from
WebSearch result snippets/summaries unless a WebFetch of the full page is
noted; I was not able to retrieve full text for any paper this session (the
one WebFetch attempt, on the IEEE Visioli setpoint-weighting page, returned
an empty page body — IEEE Xplore gates full text — so that citation is
title/abstract-only from search results, not independently verified).

- **Genetic Algorithm–Optimized PID for thermal control**,
  https://www.iieta.org/download/file/fid/191653 — GA-tuned PID vs plain
  PID; addresses step/settling performance, not explicitly ramp tracking.
  Abstract-only.
- **"Comparative analysis of PID and fuzzy logic controller: furnace
  temperature control"**, https://www.academia.edu/73894177 — states
  "processes requiring... Ramp & Soak control are especially difficult to
  handle through conventional PID," framing fuzzy control as a response to
  ramp-and-soak difficulty in general terms, but the mechanism described
  (per the search summary) is fuzzy control's steady-state behavior and
  feedforward's unsteady-state behavior treated as **complementary, separate
  paths** — "fuzzy control is good at steady-state control and feed forward
  control is good at unsteady-state control" — which is consistent with §3's
  conclusion that feedforward, not fuzzy gain adjustment, is the tool for the
  ramp (unsteady-state, tracking) portion. Abstract-only.
- **Design of Heating Furnace Temperature Control Based on Fuzzy-PID**,
  https://www.researchgate.net/publication/300856596 — fuzzy-PID for
  furnace temperature; search summary describes overshoot/rise-time/settling
  improvements, does not isolate a ramp-tracking claim distinct from
  step-response. Abstract-only.
- **GA-Fuzzy-PID for vacuum annealing furnace**,
  https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10686503/ — combines GA-tuned
  fuzzy-PID with feedforward, explicitly assigning feedforward to the
  "unsteady-state"/ramp portion and fuzzy-PID to steady-state — same
  division of labor noted above. Abstract-only (PMC listing).
- **Visioli, "Fuzzy logic based set-point weight tuning of PID controllers,"
  IEEE SMC, 1999**, https://ieeexplore.ieee.org/document/798062/ — per
  search-result summary: a fuzzy system sets the `b` weight from error and
  its derivative specifically to trade off overshoot vs. rise time on
  **setpoint following** (steps), described as reducing "both the overshoot
  and the rise time in set-point following." This is a step-response paper,
  not a ramp-tracking paper, by the summary's own framing — supports §3's
  read that fuzzy-tuned setpoint weighting targets steps, not ramps.
  Full text not retrieved (IEEE Xplore paywall/empty WebFetch).
- **"Fuzzy rule-based set point weighting for fuzzy PID controller,"
  Discover Applied Sciences (Springer)**,
  https://link.springer.com/article/10.1007/s42452-021-04626-0 — a more
  recent variant of the same idea (fuzzy-adapted `b`), same step-response
  framing per search summary. Abstract-only.
- **"Comparative study of a learning fuzzy PID controller and a self-tuning
  controller," ScienceDirect**,
  https://www.sciencedirect.com/science/article/abs/pii/S0019057800000562 —
  per search summary, reports self-organising fuzzy PID "followed the
  specified path closer and smoother than the self-tuning controller" for a
  **path-tracking** (robotic/servo) experiment — this is the one result that
  most directly claims a trajectory-tracking win for a fuzzy scheme, but it
  is a different plant class (fast robotic/mechanical servo, not a
  40-260 s-time-constant, 26-40 s dead-time thermal plant) and a different
  fuzzy architecture (self-organising master tuner, not this project's
  static rule-table). I cannot extend this result's applicability to
  kilnCtl's plant without more than an abstract; flagging as suggestive, not
  transferable. Abstract-only.
- **Interval type-2 fuzzy broad learning system for MSW incineration
  furnace**, https://www.sciencedirect.com/science/article/abs/pii/S0957417425011522
  — targets nonlinearity/uncertainty-driven overshoot, not ramp tracking, per
  search summary. Abstract-only.
- **2-DOF PIDA with GCRA optimization for electric furnace**,
  https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0334594
  and https://www.ncbi.nlm.nih.gov/pmc/articles/PMC12520383/ — per search
  summary, explicitly separates feedforward paths (P and D channels) from
  the integral/acceleration terms specifically "to achieve rapid, low
  overshoot responses without sacrificing steady-state accuracy" — another
  instance of the same servo/regulatory separation, using feedforward + 2DOF
  structure rather than fuzzy adaptation, for essentially the same problem
  class this project has. Abstract-only.
- No source found (across all searches run) claims a fuzzy error+error-rate
  inference scheme was used specifically to reduce steady ramp-following lag,
  as distinct from step overshoot/rise-time — every hit that engages ramps
  at all (ramp-and-soak framing, GA-Fuzzy-PID's feedforward split) assigns
  the ramp/trajectory portion to feedforward, and fuzzy adaptation to the
  step/steady-state portion. **This absence is itself the answer to question
  1**: the literature does not support fuzzy gain adaptation as a ramp/
  trajectory-tracking tool separate from feedforward; where the two problems
  are addressed together, feedforward is consistently the ramp-side
  component and fuzzy/PID adaptation is consistently the step/steady-state
  component, matching this project's own existing split (feedforward climb
  vs. fuzzy-adjusted PID gains) rather than motivating a change to it.

## 6. Ranked recommendation

1. **Leave the fuzzy layer's rule table and inputs as they are for ramp
   tracking.** No literature source and no argument constructed here supports
   redirecting the (error, STEADY-rate) cells' `Ki` consequent to attack ramp
   lag — doing so would trade away their current, more defensible role
   (a small `Kp` nudge under sustained overshoot/undershoot) for a change
   that would also need to not regress that role, and no evidence here
   justifies the churn. This is a "don't touch it" recommendation, not a
   design.
2. **If ramp tracking is to be improved further, the feedforward climb term
   is the mechanism that structurally targets it** (§3), and it already has
   an active tuning history (`a7c9a882`'s per-zone scale-down, the `e690f8a6`
   integral-floor fix, and the dwell-entry-decay attempt and its revert,
   `a77db88c`/`b8b192db`/`f9d84453`). Whether that specific prior attempt was
   reverted for the right reason, and whether a variant of it is worth
   retrying, is explicitly out of scope for this document — a separate pass
   is examining exactly that question. This document's contribution is
   narrower: identifying the climb term (not the fuzzy layer) as the
   mechanism that addresses ramp lag structurally, per §1-§3.
3. **Consider tuning `b` away from 1.0, but for overshoot, not ramp
   tracking** — it is a real, present, currently-inert mechanism (§4) whose
   established literature role (Visioli 1999 and its Springer follow-up) is
   step-response overshoot/rise-time shaping, which matches this project's
   *actual* stated objective for the fuzzy layer. This is a separate,
   independent experiment from the fuzzy layer and does not require touching
   `pid_fuzzy.c` at all — only `PID_SETPOINT_WEIGHT_B`'s value (currently a
   compile-time constant, `profile_executor_internal.h:251`).
4. **What would test any of this:** single-zone simulation only, per the
   standing constraint that the multi-zone coupling model is refuted
   (`project_sim_firmware_coupling_model_mismatch`,
   `project_z0_coupling_is_shape_not_scale`) and two replacement attempts have
   failed — a single-zone FOPDT sim (already used for the reverted
   dwell-entry-decay and lead-compensation experiments) is the right harness
   for a `b` sweep: run a representative ramp-into-dwell profile at several
   fixed `b` values (e.g. 0.4, 0.6, 0.8, 1.0) and score **dwell-entry peak
   temperature** (the correct overshoot metric per standing guidance, not
   IAE/MAE) plus `FIRING_SUBSCORE_LAG_S` (`firmware/KilnFW/App/drivers/control/firing_score.c`)
   as the ramp-tracking side-effect check, to confirm `b` indeed does not
   move the lag subscore materially (predicted here from the `Kv` argument in
   §1/§3) while it does move the overshoot metric. Differences smaller than
   0.5 °C should be reported, not chased, per standing project guidance.
   This does not require a kiln firing and does not require touching
   `pid_fuzzy.c`, `adaptive_tune_ki.c`, or any file owned by concurrent work.

## Sources

- Genetic Algorithm–Optimized PID Control for Thermal, https://www.iieta.org/download/file/fid/191653
- Comparative analysis of PID and fuzzy logic controller: furnace temperature control, https://www.academia.edu/73894177/Comparative_analysis_of_PID_and_fuzzy_logic_controller_A_case_of_furnace_temperature_control
- Design of Heating Furnace Temperature Control System Based on Fuzzy-PID Controller, https://www.researchgate.net/publication/300856596_Design_of_Heating_Furnace_Temperature_Control_System_Based_on_Fuzzy-PID_Controller
- Design of vacuum annealing furnace temperature control system based on GA-Fuzzy-PID algorithm, https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10686503/
- Fuzzy logic based set-point weight tuning of PID controllers (Visioli, IEEE SMC 1999), https://ieeexplore.ieee.org/document/798062/
- Fuzzy rule-based set point weighting for fuzzy PID controller, https://link.springer.com/article/10.1007/s42452-021-04626-0
- Comparative study of a learning fuzzy PID controller and a self-tuning controller, https://www.sciencedirect.com/science/article/abs/pii/S0019057800000562
- Furnace temperature control based on interval type-2 fuzzy broad learning system, https://www.sciencedirect.com/science/article/abs/pii/S0957417425011522
- A Novel 2-DOF PIDA control strategy with GCRA-based parameter optimization for electric furnace temperature control, https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0334594 and https://www.ncbi.nlm.nih.gov/pmc/articles/PMC12520383/
- Two Degree-of-Freedom PID Control for Setpoint Tracking (MathWorks), https://www.mathworks.com/help/simulink/slref/two-degree-of-freedom-pid-control-for-setpoint-tracking.html
- Setpoint Weighting (20-sim library docs), https://www.20sim.com/webhelp/library_signal_control_pid_control_setpointweighting.php

## Cross-references (this project's own prior work, cited above)

- `docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md` (`ed854ac5`)
- `docs/audits/review_sim_fuzzy_commits_2026-09-13.md` (`8a12521b`)
- `docs/research/multizone_thermal_modelling_literature_2026-09-11.md` (`47cd0f28`)
- `firmware/KilnFW/App/drivers/control/pid_fuzzy.c`, `pid.c`,
  `profile_executor_internal.h`, `profile_executor_run.c`,
  `firing_score.c`
- Commits: `e690f8a6` (integral floor at -ff_hold), `a77db88c`/`b8b192db`
  (dwell-entry climb decay), `f9d84453` (revert of same), `a7c9a882`
  (zone-0 climb scale-down), `cf3b5adb` (guard 1 climbing-window floor,
  cited only for context on the climb term's surrounding machinery)
