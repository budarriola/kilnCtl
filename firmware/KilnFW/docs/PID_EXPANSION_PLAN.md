# PID Expansion Plan — control-algorithm choice per zone/firing

> **Status:** Phases 1, 2, 3 and 4 are landed and wired end to end — `pid_fuzzy`
> is called from `profile_executor.c` every tick a zone runs in PID_FUZZY
> mode, and the mode is selectable from `zones_page.html`. **Not** landed:
> Phase 3b (coupling feedforward — nothing persists the matrix and no
> coupling term exists in the control loop yet) and parts of Phase 5 (the
> zone-settings-inheritance UI has no negative tests yet; backup export of
> the four new fields has no test). See §4 for exact status per item.
> Companion to `PID_CONTROL.md` (what is built today) and
> `COMMISSIONING_UX.md` (the UI-design conventions this plan follows: ask
> only what cannot be derived, show provenance, never silently overwrite a
> hand-tuned value).
>
> §4 tracks progress with checkboxes: `[x]` is in the tree, `[ ]` is
> outstanding. Phase 0 is the foundation that predates this plan.

## 0. The problem

Every firing loads a different mass/arrangement of pots. That changes the
kiln's thermal time-constant (`tau`) and gain (`K`) — the two numbers a PID
loop is tuned against — every single time. A fixed, hand-tuned PID is tuned
for one load and wrong for the next. `PID_CONTROL.md` already documents this
codebase's answer to *identification*: `pid_autotune.c` fits a first-order-
plus-dead-time (FOPDT) model (`K`, `tau`, `L`) from a step-response test and
proposes SIMC gains from it, accepted only by an explicit
`POST /api/autotune/accept`. (The separate relay-feedback path proposes ZN /
Tyreus-Luyben gains from `{Ku, Tu}` and deliberately writes **no** model —
the FOPDT path refuses ZN/TL and the relay path refuses to write `{K, tau, L}`,
because neither rule set is derivable from the other's data. §1a checks that
split against the literature.) What this plan adds is a considered choice of
*which control algorithm* that identified model should feed, presented as an
operator-facing option next to the existing Kp/Ki/Kd fields on
`zones_page.html`, backed by a literature read of ten papers.

The papers themselves are **cited, not vendored** — `/docs/research/` is
gitignored, so the filenames in §1's table name a local working copy rather
than something in the repo. `CREDITS.md` carries the citation for each paper
that informed a recommendation, with a URL where one is known.

## 1. What the ten papers say

| # | Paper (file) | Algorithm | Problem addressed | Reported result |
|---|---|---|---|---|
| 1 | *Berner, PhD thesis* (`ThesisJosefinBerner.pdf`) | Relay-feedback autotuning with normalized time delay; decentralized MIMO identification | Automatic identification of process dynamics, including coupled/interacting loops, without a manual step test | Improved relay-autotune identification accuracy over the classical Åström–Hägglund method; decentralized relay experiments converge and separate coupled loops (Fig. 2) |
| 2 | Nichols Philips et al., *Application of Auto Tuner Fuzzy PID Controller* (`Application_of_Auto_Tuner_Fuzzy_PID_Cont.pdf`) | Cascade PID with fuzzy logic continuously re-tuning each PID term | Furnace temperature control that must stay good when the process transfer function itself changes (i.e. the plant is not fixed) | Fuzzy cascade controller has better rise/overshoot/undershoot/settling time and adapts to a *changed* process model better than a fixed cascade PID (simulated, MATLAB/Simulink) |
| 3 | *Design and simulation of self-tuning PID* (`Design_and_simulation_of_self_tuning_PID.pdf`) | Self-tuning PID-type fuzzy adaptive controller for a two-zone HVAC system | Multi-zone temperature control with different zone properties and a variable flow rate | Self-tuning fuzzy PID beat both classical PID and fuzzy-PD on settling time and steady-state error across two zones |
| 4 | *Fractional-Order PID Controllers for Temperature [Systems]* (`Fractional_Order_PID_Controllers_for_Tem.pdf`) | Fractional-order PID (FOPID), review | Heating/temperature systems with external disturbance, model uncertainty and non-linearity | Review concludes FOPID gives better robustness, stability and flexibility than integer-order PID across ambulance, induction-heating and bioreactor case studies it surveys; notes classical PID is still more flexible on raw time-spec response |
| 5 | *General-type industrial temperature system[s]* (`General_type_industrial_temperature_syst.pdf`) | Fuzzy fractional-order PID (FFuzzy PID) — gains updated online from fractional-order fuzzy rules (Mittag-Leffler membership functions) | Temperature systems with model uncertainty, noise, and **random delay** | Better dynamic performance and robustness to internal/external disturbance than fixed FOPID, verified only in simulation |
| 6 | Tan et al., *Hybrid System based Fuzzy-PID Control Sche[me]* (`Hybrid_System_based_Fuzzy_PID_Control_Sc.pdf`) | Q-learning + genetic-algorithm hybrid to auto-optimize a fuzzy-PID's membership functions | Removing the need for an experienced operator to hand-tune fuzzy membership functions by trial and error, for an exothermic (runaway-prone) batch reactor | Lower undershoot/overshoot than a conventionally-tuned fuzzy-PID even under an introduced disturbance |
| 7 | *Implementation of Fuzzy PID Controller o[n]* (`Implementation_of_Fuzzy_PID_Controller_o.pdf`) | Fuzzy PID over a system-identified ARX model (MATLAB System Identification Toolbox) of a real PT326 heating rig | Removing trial-and-error PID tuning for a physical heating process | Lower RMSE, better rise/settling time than conventional PID, validated against a real identified plant model (not just a textbook transfer function) |
| 8 | Güven, *Comprehensive Optimization of PID Controller Parameters for DC Motor Speed* (`Optim Control Appl Methods - 2024...pdf`) | Metaheuristic optimizers (Grey Wolf, JAYA, Golden-Jackal, Jellyfish/modified-Jellyfish) searching PID gains offline | DC motor speed control (not thermal) — global gain optimization against a simulated plant | Modified Jellyfish (mJS) gives the smallest overshoot and best settling time (1.18 s) among the algorithms compared, confirmed by Friedman/Wilcoxon/Kruskal/ANOVA significance tests; GWO was unstable on one plant |
| 9 | *Research on temperature control with num[erical methods]* (`Research_on_temperature_control_with_num.pdf`) | Comparison of two-position (hysteresis), self-tuned PID, and manually-tuned PID (Ziegler-Nichols step response, Cohen-Coon, and Ziegler-Nichols tuning-rule methods) | Which classical tuning rule is best for an indirect-heat resistance furnace (electric, 1 kW/230 V) | Cohen-Coon tuning gave the best experimental temperature control of the three PID tuning methods tried; both beat plain hysteresis control |
| 10 | *Tuning Optimization of Hybrid controller* (`Tuning_Optimization_of_Hybrid_controller.pdf`) | Hybrid PI + feed-forward controller, tuning-optimized | Shell-and-tube heat-exchanger outlet temperature control under external disturbance | 81% improvement in overshoot and 76% improvement in settling time versus a classical PI controller, zero steady-state error |

## 1a. What the literature says about the methods already implemented here

This section checks the six identification/tuning methods `pid_autotune.c`,
`autotune_engine.c` and `pid.c` already implement (per `PID_CONTROL.md`)
against the broader control-engineering literature, separately from the ten
papers in §1 (which argue for or against adding a *new* algorithm on top).
Citations here use `[Wn]` to keep them out of the `[1]`-`[10]` paper numbering.

**FOPDT step-response fitting, two-point 28.3%/63.2% method.** This is
Smith's method [W2] — one of several two-point procedures for reading a
time-constant and dead-time off an S-shaped open-loop step response, the
others being (35%/85%) and (33%/70%) pairs. The literature treats the exact
percentage pair as a minor choice, not a correctness question: using more
than two points (28.3/50/63.2/70) gives redundancy against a noisy trace, but
the two-point form this codebase uses is standard and well characterized, not
an ad hoc shortcut. Its known weakness is exactly the one `PID_CONTROL.md`
already calls out — the fit is only as good as the underlying assumption
that the plant really is first-order-plus-dead-time, and a step test forces
the plant through a large, slow excursion to get it.

**Relay-feedback identification (Åström–Hägglund).** Originated in
Åström & Hägglund's 1984 paper generating sustained relay oscillation as an
alternative to continuous-cycling (classical Ziegler-Nichols) identification
[W3][W4]. It recovers the ultimate gain/period pair (`Ku`, `Tu`) from the
relay's oscillation amplitude and hysteresis band — the exact formula
`PID_CONTROL.md` documents. The literature's consistent praise is that it is
"one of the simplest and most robust auto-tuning techniques," in wide
industrial use for decades [W3], because it identifies the process's
critical point with one bounded, self-limiting test instead of a slow open
step. Its known trade-off against step/FOPDT identification: it only yields
one frequency-response point (`Ku`, `Tu`), not a full `{K, tau, L}` model —
which is exactly why this codebase's own comment ("one frequency-response
point does not determine a FOPDT model") declines to write a plant model
from a relay result, matching how the technique is used elsewhere.

**SIMC tuning rule (Skogestad).** SIMC ("Skogestad IMC") derives PID gains
from a FOPDT model via a single tuning parameter, the closed-loop time
constant `lambda`, and the literature calls out its "smooth" tuning intent —
trading a controllable amount of speed for robustness — as its main
differentiator from Ziegler-Nichols-style rules, which were designed around
quarter-amplitude decay (i.e., a controller that is expected to ring) [W1].
The `lambda = 3*L` choice this codebase treats as "robust" for a kiln is the
same territory SIMC's own literature describes as favoring smooth, non-
oscillatory response over aggressive disturbance rejection — the right side
of that trade for a slow, high-dead-time thermal plant where any overshoot
risks the ware. The searches found no source specifically validating
`lambda = 3*L` as a named constant (Skogestad's own default is `lambda = tau`
or `lambda = theta` in different treatments), so that specific multiplier
should be read as this codebase's own conservative choice within SIMC's
framework, not a cited external result.

**Ziegler-Nichols and Tyreus-Luyben, from `{Ku, Tu}`.** Both are documented,
long-standing rules for converting relay/continuous-cycling data into PID
gains. The comparison literature is consistent: Tyreus-Luyben is a
deliberately less aggressive modification of Ziegler-Nichols, trading
response speed for reduced overshoot and better robustness margins, while
classical Ziegler-Nichols targets quarter-amplitude decay and is
correspondingly more oscillatory [W5]. `PID_CONTROL.md`'s note that "ZN
targets quarter-amplitude decay... which on a kiln at 1200°C costs the
firing" is exactly what the literature would predict — ZN is the wrong
default for a plant where oscillation is expensive, and offering
Tyreus-Luyben alongside it (as the code already does, non-default) is the
literature-consistent way to expose the trade-off rather than hide it.

**Model-based feedforward duty term.** Feedforward-plus-PID for FOPDT
processes is a standard structure in the literature: a feedforward term
computed from the identified model handles the known, predictable part of
the control effort (here, the duty needed to hold or ramp to a setpoint),
leaving feedback to correct only the model's error [W6]. This matches
`PID_CONTROL.md`'s own framing ("puts the actual curve on the desired curve
instead of a fixed offset below it, leaving the PID to correct only model
error") closely. The literature's standard caution applies directly here
too: a feedforward term is only as trustworthy as the model it is computed
from, which is why gating it on "off unless that zone has an identified
model" and clamping the sum rather than the feedforward term alone (as this
codebase does) is the conservative, literature-consistent choice rather than
a shortcut.

**Relative Gain Array (RGA) for cross-zone coupling.** RGA is the classical
method for choosing input-output pairings and quantifying loop interaction
in multivariable control, computed here exactly as the textbook formula
prescribes — `RGA = K .* (K^-1)^T` from a steady-state gain matrix [W7][W8].
Its best-documented limitation is precisely the one relevant to a kiln: RGA
is a **steady-state** measure computed from static gains, so on a genuinely
nonlinear plant (as a radiatively-coupled multi-zone kiln is, since
radiative loss scales with T^4) a single RGA computed at one operating point
does not necessarily hold at another — the literature's answer is either
computing RGA separately per operating region, or extensions like dynamic
RGA (using transfer functions instead of static gains) or the nonlinear
block RGA variants built for exactly this case [W8]. `PID_CONTROL.md`
already reflects this correctly: the matrix is left unpopulated and unused
until real bench data exists rather than approximated, and the code refuses
an incomplete or singular matrix instead of guessing — consistent with the
literature's caution that an RGA computed from a bad or unrepresentative
gain estimate is worse than no RGA at all.

**Where this checked out, and where it didn't.** Five of the six methods are
described in `PID_CONTROL.md` in a way that matches the literature closely,
including the reasoning behind non-default choices (relay identification not
producing a plant model, ZN not being the default rule, RGA staying
unpopulated). The one item worth flagging: no source found here specifically
names `lambda = 3*L` as a standard SIMC default — Skogestad's published
defaults use `lambda = tau` (fast) or `lambda = theta`/dead-time-based
variants depending on the source, so `3*L` should be treated as this
codebase's own conservative parameter choice inside the SIMC framework, not
as a directly cited external constant, and `PID_CONTROL.md`/this plan should
not imply otherwise.

**Web references**

- [W1] The SIMC Method for Smooth PID Controller Tuning (Skogestad) — https://skoge.folk.ntnu.no/publications/2012/skogestad-improved-simc-pid/PIDbook-chapter5.pdf
- [W2] Classic Methods for Identification of First Order Plus Dead Time (FOPDT) Systems — https://towardsai.net/p/artificial-intelligence/classic-methods-for-identification-of-first-order-plus-dead-time-fopdt-systems
- [W3] Relay feedback auto-tuning of process controllers — a tutorial review — https://www.researchgate.net/publication/222514888_Relay_feedback_auto-tuning_of_process_controllers_-_a_tutorial_review
- [W4] Åström-Hägglund relay feedback test (overview figure/summary) — https://www.researchgate.net/figure/Astrom-Hagglund-relay-feedback-test_fig1_352973090
- [W5] Mastering Tyreus-Luyben Tuning — https://www.numberanalytics.com/blog/tyreus-luyben-tuning-guide
- [W6] Feedforward Control (Dynamics and Control) — https://apmonitor.com/pdc/index.php/Main/FeedforwardControl
- [W7] Relative gain array — Wikipedia — https://en.wikipedia.org/wiki/Relative_gain_array
- [W8] Relative Gain Array — an overview (ScienceDirect Topics) — https://www.sciencedirect.com/topics/engineering/relative-gain-array

## 2. Recommendation

**Recommend two algorithms, layered rather than competing, both already
close to what `PID_CONTROL.md` has built:**

### 2a. Primary: keep and lean harder on step/relay-test autotuning + feedforward PID (papers [1], [9], [10])

This codebase's `pid_autotune.c`/`autotune_engine.c` already does the
*identification* step [1] argues for and [9] tunes against: a step test fits
`{K, tau, L}` fresh, per zone, per firing — which is precisely how "unknown
thermal mass at firing start" gets solved. (Note the division of labour
precisely: [1] is about relay-feedback *identification*, and the relay path
here yields `{Ku, Tu}` only, never a model — see §0 and §1a; [9] compares
*tuning rules* applied to an already-identified furnace model, so it validates
the rule menu, not the identification method. Neither paper validates the
other's half.) It needs no sensor beyond the thermocouples already on every
zone, and it does not need online re-identification, because the *load* is
fixed once the kiln is closed — it only needs re-identifying between firings.

**Caveat this recommendation depends on, stated plainly:** "the plant is
constant during a firing" is true of the *load*, not of the *plant gain*. A
kiln's dominant heat-loss term is radiative and scales with `T^4`, so the
effective `K` at 200 °C and at 1250 °C are not the same number, and a single
FOPDT fit taken during a low-temperature step test will under-predict the duty
needed near cone temperature. This is a real limitation of the primary
recommendation, it is orthogonal to the load-mass problem, and it is the
strongest argument for the §2b layer — more so than cross-zone coupling is.
`PID_CONTROL.md` already names the same gap ("one temperature band per zone,
no gain scheduling").

[9]'s finding that Cohen-Coon step-response tuning beat plain Ziegler-Nichols
on a real resistance furnace argues for exposing tuning-rule choice (the code
already computes SIMC from FOPDT and ZN/Tyreus-Luyben from relay data —
Cohen-Coon is a small, well-defined addition on the FOPDT path, see §4).
**But note the tension:** Cohen-Coon descends from the same quarter-amplitude-
decay design target as Ziegler-Nichols and is the more aggressive rule of the
two on offer here, which sits awkwardly against §1a's argument for SIMC
(smooth, non-oscillatory) on a plant where overshoot costs the ware. [9]'s
result is one furnace, judged on tracking performance, not on overshoot risk
to a load. Cohen-Coon should therefore ship as a *selectable third rule with
SIMC remaining the default*, and the choice between them settled by bench data
on this kiln — not by promoting Cohen-Coon on the strength of [9] alone.

[10]'s 81%/76%
overshoot/settling-time improvement from adding a feed-forward term over plain
PI matches this codebase's existing model-based feedforward
(`u_ff` in `PID_CONTROL.md` §"Feedforward") almost exactly — the plan here is
to make sure every operator who accepts an autotune result also gets the
feedforward term switched on (it already is, automatically, once a model is
accepted), and to surface that fact on the UI so it is not a hidden win.

**Why not adaptive/self-tuning-in-the-loop control (continuously re-estimating
`K`/`tau` while firing) as the primary recommendation:** the papers that show
its value ([2], [3], [6]) do so on plants whose dynamics genuinely change
*during* a run (a furnace whose transfer function itself changes, a batch
reactor's exothermic runaway). A kiln loaded once and closed does not — its
thermal mass is fixed for the whole firing. Re-identifying every tick adds
real engineering risk (an in-loop estimator can diverge, especially near a
kiln's dead-time-heavy dynamics) for a problem this kiln does not have. The
"unknown at firing start, constant during firing" framing calls for a
good *identification-before-firing* step, not continuous re-identification.

### 2b. Secondary, opt-in: Fuzzy-PID with fixed rule table (papers [2], [7]; multi-zone evidence from [3])

`PID_CONTROL.md` already flags a real, open gap: "one temperature band per
zone (no gain scheduling)" and a cross-zone-coupling matrix (RGA) that is
built but never populated because no thermal-cycle hardware has run yet.

What each cited paper actually supports, kept distinct rather than pooled:
[2] shows a fuzzy layer holding performance when the *process transfer
function itself changes* — the closest published analogue to this kiln's
`T^4` gain variation across a firing, and the primary justification for this
mode. [7] shows a fuzzy PID beating a conventional one on a **real**
identified heating rig (not just a textbook transfer function), which is what
makes the approach credible rather than simulation-only. [3] is the only
multi-zone result in the set, and it is the weakest support of the three here:
its two zones are independently ducted, so it shows "per-zone fuzzy adjustment
works in a multi-zone installation", not "fuzzy adjustment solves zone
coupling". Coupling is handled separately in §2c and should not be claimed as
a [2]/[7] result.

A zone whose element ages and drifts, and the temperature-dependent gain
above, are the case [2] and [7] argue for: a small fuzzy-rule table nudging Kp/Ki/Kd around the
autotune-fitted base gains, using nothing but the same error/error-rate
signals the PID loop already computes. It is implementable in plain C: a
9-25-rule Mamdani table over triangular membership functions and a lookup
plus linear interpolation is a few hundred lines, no floating-point library
beyond what `pid.c` already uses, and it needs zero additional sensors. It
should ship **as a second selectable mode, not a replacement** for classic
PID — the papers uniformly compare it *against* a classic PID baseline and
show an improvement, they do not show classic PID becoming *wrong*.

### 2c. Cross-zone thermal coupling

This kiln has multiple zones that are not thermally independent — heat from
one zone's element bleeds into its neighbors, so a controller tuned per zone
in isolation can fight its neighbor or hunt. `PID_CONTROL.md` already has a
coupling matrix (RGA — relative gain array) built but unpopulated, waiting on
real thermal-cycle hardware data.

None of the ten papers directly solve *N interacting heating zones in one
vessel*. The closest are [1] (decentralized relay-feedback identification
that explicitly separates coupled loops during autotune — this is an
**identification-time** answer: it tells you how strongly zone A's relay test
disturbs zone B's reading, which is exactly what the RGA matrix wants to be
populated with) and [3] (self-tuning fuzzy PID across a two-zone system, but
its zones are independently ducted, not radiatively coupled the way kiln
zones are — its result supports "per-zone fuzzy adjustment is viable," not
"here is how to decouple two kiln zones").

**Recommended extension (this project's own; structure is textbook, the
kiln-specific application is not attributed to any paper):** treat a neighbor
zone's temperature as a **measured disturbance fed to the feedforward term**,
not as an adjustment to the feedback gains.

That placement is the whole point, and an earlier draft of this plan got it
wrong by proposing a neighbor-driven bias on `Ki` instead. Two reasons the
gain-bias form is unsound:

1. A neighbor's heat bleed *already shows up* in this zone's own measured
   error — that is what "coupled" means. Adding a second path that reacts to
   the same physical event through the integral gain double-counts it, and
   raising `Ki` in response to a disturbance is the classic route to integral
   windup on a plant with this much dead time.
2. Measured disturbances are precisely the case feedforward exists for [W6],
   and it is the structure [10] measured an 81%/76% overshoot/settling-time
   improvement from on a heat exchanger — a *thermal* plant with an external
   disturbance, which is the closest published analogue in this set. The
   feedback gains should stay a function of this zone's own error, as in every
   fuzzy-PID paper cited in §2b.

Concretely: the existing feedforward term (computed today from `{K, tau}`
alone) gains an additive coupling contribution `-c_ij * (T_j - T_j_setpoint)`
for the strongest-coupled neighbor `j`, where `c_ij` is a per-pair coupling
coefficient measured by the *existing* autotune machinery — `autotune_engine.c`
already samples every configured zone during a step test specifically to fill
a row of the coupling matrix, so the measurement path exists and is unused. A
neighbor running hot subtracts duty from this zone; a neighbor running cold
adds it; a neighbor on target changes nothing. This is a bounded, sign-checked
scalar on a term the code already computes and already clamps, not a new
control loop, and it degrades to exactly today's behavior when `c_ij = 0`, a
zone has no measured coupled neighbor, or no model is identified (feedforward
is already gated off in that last case).

Full MIMO decoupling (inverting the RGA gain matrix to cancel all cross terms
at once) stays deferred: §1a's [W8] limitation applies directly — an RGA is a
steady-state measure and this plant's gains move with `T^4`, so a decoupler
designed against a single-operating-point matrix would be inverting a number
that is only correct at one temperature. One measured feedforward coefficient
per adjacent pair degrades gracefully when it is wrong; a matrix inversion
does not.

**Explicitly not recommended:**

- **Fractional-order PID / fuzzy-FOPID** ([4], [5]) — real robustness gains
  in the literature, but fractional-order controllers require approximating
  a non-integer derivative/integral (an IIR filter bank, e.g. Oustaloup), add
  two fractional-order exponents (conventionally written `lambda` and `mu` —
  no relation to SIMC's closed-loop time constant `lambda` in §1a; the symbol
  collision is the literature's, not this document's) that nobody on this
  project has a settled way to pick, and the papers' own headline case studies (ambulance climate
  control, bioreactors) are not thermally analogous to a kiln's very slow,
  very high dead-time dynamics. The complexity is not justified by [4]'s own
  admission that "the standard PID controller is more flexible to time
  specification values."
- **Metaheuristic offline gain search (GA/Q-learning/Jellyfish/etc.)**
  ([6], [8]) — [8] is not even a thermal system (DC motor speed) and both
  [6] and [8] run their optimizer offline, against a simulated plant, for
  many generations/iterations — exactly the kind of workload an ESP32-S3
  cannot run online, and this project has no simulated kiln plant accurate
  enough (per `PID_CONTROL.md`'s own "untested against a real kiln" caveats
  on the FOPDT fit) to trust an optimizer's convergence against it. [6]'s
  actual *useful* contribution — using a search to tune fuzzy membership
  functions instead of hand-picking them — is worth revisiting only once
  §2b's fuzzy mode has real fielded data to search against.

## 3. UI plan: per-zone control-algorithm dropdown on Thermocouples & Zones

### 3.1 Where it goes

`App/drivers/zones_page.html` now has, per zone, a control-mode select
including the fuzzy option (`modeHtml`, line ~548: `<option value="3">PID —
fuzzy-adjusted</option>`) and a fuzzy tuning panel (`.fuzzyPanel`, lines
~599-606: rule-strength table plus the "Adjustment strength" number input,
default 30) alongside the classic PID Kp/Ki/Kd block (lines ~592-598), plus
the read-only fitted-model display and the page-level "PID Autotune" /
relay-feedback / coupling-matrix sections.
This plan **extends the existing mode select** rather than adding a
second control, so there is exactly one place an operator decides "how does
this zone's heat get commanded":

```
Control mode:  [ Off ▾ ]
               [ Bang-bang (simple on/off) ]
               [ PID — classic ]
               [ PID — fuzzy-adjusted ]  ← NEW
```

Selecting a PID variant (classic or fuzzy) reveals a tuning panel below the
dropdown, swapped by JS on `change` exactly the way `renderTcType()` and the
other zone-row builders already conditionally render blocks in this file —
no new page, no new endpoint round-trip to switch which panel is visible.

### 3.2 Dropdown option copy (operator-facing, no jargon)

| Option | One-line description shown under it |
|---|---|
| **Off** | Never turns this zone's heater on. Use for a zone with nothing loaded this firing. |
| **Bang-bang (simple on/off)** | Turns the heater fully on below your target and fully off above it, like a household oven. Simple and reliable, but the temperature will wobble a few degrees around the target. |
| **PID — classic** *(default once tuned)* | Smoothly adjusts how hard the heater runs instead of just on/off, for a steadier climb and fewer overshoots. Needs tuning numbers for this kiln and load — use the Autotune button below, or enter numbers from a previous firing with a similar load. |
| **PID — fuzzy-adjusted** | Same smooth control as classic PID, but nudges its own settings up or down while firing, so one set of tuning numbers works across the whole temperature range instead of only near where it was measured. Best for long firings that climb a long way from where Autotune ran, or a zone whose elements have aged since it was last tuned. Still needs an initial Autotune first — this mode adjusts around that starting point, it does not replace it. |

(A future `PID — autotune pending` badge/state is out of scope here; the
existing "not identified yet" read-only text already covers it.)

### 3.3 Panel shown per selection

- **Off / Bang-bang** — no tuning panel (bang-bang already shows only its
  existing hysteresis note, unchanged).
- **PID — classic** — exactly what exists today: Kp/Ki/Kd number inputs,
  the read-only fitted-model line (`K`, `tau`, `L`, or "not identified yet"),
  and the page-level Autotune section's Start/Accept/Abort flow, scoped to
  this zone via the existing `populateAutotuneZones()` zone picker.
- **PID — fuzzy-adjusted** *(new)* — the same Kp/Ki/Kd fields, now labeled
  "base gains (from Autotune)" since the fuzzy layer adjusts around them
  rather than replacing them, plus:
  - A read-only 3x3 rule-strength summary — for each of {error large-negative
    / near-zero / large-positive} × {error falling / steady / rising}, the
    direction (↑/–/↓) each of Kp/Ki/Kd is nudged, rendered as a small table,
    not raw membership-function numbers. Kiln operators are not fuzzy-logic
    engineers; the value of showing this at all is "prove it isn't a black
    box," not "let them retune it."
  - One number input, **Adjustment strength** (0-100%, default a
    conservative value e.g. 30%), scaling how far the fuzzy layer is allowed
    to move Kp/Ki/Kd away from the base gains — the one knob worth exposing,
    because "how aggressively should this deviate from what Autotune
    measured" is a real, answerable question for an operator who has seen a
    zone overshoot or lag.
  - The same read-only fitted-model line and Autotune section as classic PID
    (fuzzy mode does not change how the base gains are obtained).

### 3.4 Why per-zone, not per-firing

`zones_page.html` already scopes control mode, Kp/Ki/Kd, and the fitted model
per zone (`zone_cfg_t`), because zones can legitimately need different modes
— a bisque zone that just needs to hold a soak might run bang-bang while a
glaze zone on a tight ramp runs PID. The new dropdown follows that existing
grain rather than adding a firing-wide setting that would fight it.

### 3.5 Zone settings default to "same as another zone", not to a blank form

**The problem this solves.** Every zone row currently renders the full
settings stack below its "Thermocouple type wired to channel *i*" line:
control mode, calibration offset, Kp/Ki/Kd, max ramp, min rise rate, max/min
temp, the guard parameters and the timing profile. On a 3-zone kiln that is
three near-identical copies of a long form, and in practice the zones of one
kiln want the *same* numbers — a potter tuning zone 0 and then hand-copying a
dozen fields into zones 1 and 2 is being asked to do a transcription job the
page should do, and every hand-copy is a chance to fumble one digit that
later reads as a zone-specific quirk.

**Required behavior.** Only the first zone shows the settings stack expanded
by default. Each subsequent zone shows, in place of that stack, one dropdown:

```
Thermocouple type + settings:  [ Same as zone 0 ▾ ]
                  [ Same as zone 2 ]        <- any other *enabled* zone
                  [ Custom settings for this zone ]
```

- Default selection for zones 1..N is **"Same as zone 0"**.
- The list offers every other enabled zone, not just zone 0 — a 4-zone kiln
  where zones 2 and 3 are a matched pair should be able to say so.
- Choosing "Custom settings for this zone" expands the full stack for that
  zone, pre-filled with the values it was inheriting, so "custom" starts from
  what was already in effect rather than from blanks or zeros.
- A zone in "same as" mode shows the inherited values **read-only** beneath
  the dropdown, not hidden entirely. `COMMISSIONING_UX.md`'s show-provenance
  rule applies: an operator must be able to see what a zone will actually do
  without changing a control to find out.

**Scope of the inherited block: thermocouple type downward.** The dropdown
covers `tc_type` as well as everything below it. In practice a kiln is wired
with one thermocouple type throughout — all Type K, all Type S — so making
the operator set it once per channel is the same transcription chore the rest
of this section exists to remove.

One implementation subtlety this creates, worth stating so it is not
discovered later: `tc_type` is the one inherited field that is **per-channel,
not per-zone**. The existing comment at `zones_page.html:551` is explicit
("tc_type here means *channel i*, not *zone i*"), and `zone_cfg_t::tc_type`
is documented the same way in `zones_http.c`. So "copy zone 0's settings to
zone 1" means, for this field, *write zone 0's thermocouple type onto the
channel zone 1 reads* — which is the intended and useful behavior, but it is
a write to a different addressing space than the other copied fields. Two
consequences to handle rather than trip over:

- **CORRECTED 2026-08-30 — the first version of this bullet was wrong and
  caused a real bug.** It said "apply the type to every channel in that
  zone's mask", full stop. Implemented literally, that changed the meaning of
  an existing control: the select is labelled "Thermocouple type wired to
  channel *i*" and previously posted `z<i>_tctype` straight to channel *i*.
  Fanning it out over `thermo_mask` means that whenever
  `thermo_mask != (1 << i)` — the normal case after any channel remap — an
  operator setting "channel 2" silently re-types channel 0, while channel 2
  echoes back unchanged so the edit appears to vanish on reload. A zone with
  `thermo_mask == 0` (explicitly legal) loses the edit entirely. The
  consequence is a wrongly-linearized thermocouple on a live kiln.

  The correct rule: **for a zone with its own custom settings, the write is
  the identity it always was** — zone *i*'s select sets channel *i*'s type.
  The mask fan-out applies **only** to a zone that is inheriting from a
  *different* terminal zone, which is the only case where "copy that zone's
  thermocouple type onto the channels this zone actually reads" is what the
  operator asked for. If the fan-out semantics are wanted for custom zones
  too, the select must be relabelled and derived from the mask first — the
  label and the write have to agree.
- Two zones sharing a channel must not be able to give it conflicting types
  through their separate dropdowns. Last-write-wins is acceptable, but the
  page must show the resulting type on both zones rather than letting one
  zone display a type its channel does not actually have.

**Still NOT covered:** the zone name, relay mask, thermocouple mask and CT
mask. Those are wiring — unique per zone by definition, and copying them
between zones would describe a kiln that does not exist.

**Implementation notes.**
- Storage: this is a UI-level convenience over the existing per-zone config,
  *not* a new inheritance layer in the firmware. On save, an inheriting zone
  writes the resolved values into its own `zone_cfg_t` exactly as if they had
  been typed — the control loop keeps reading one flat per-zone config and
  knows nothing about "same as". The only new persisted state is a small
  per-zone `settings_source` (u8: 0xFF = custom, else the zone index copied
  from), kept solely so the page can re-open showing the same dropdown state.
- Consequence to handle deliberately: editing zone 0 after zones 1-2 have
  been saved as "same as zone 0" must re-resolve and re-save those zones too,
  or they silently drift from the zone they claim to follow. Do this on save
  in `renderZones()`'s submit path and say so in the UI ("also updates zones
  1, 2"), rather than resolving at load time — a zone whose stored numbers
  disagree with its claimed source is exactly the kind of split state that
  makes a later bug unreadable.
- Guard against a cycle (zone 1 "same as" zone 2 while zone 2 is "same as"
  zone 1) and against pointing at a disabled zone; both should collapse to
  custom rather than resolve to nothing.

### 3.6 Cross-zone coupling is not a dropdown option

§2c's coupling compensation is deliberately **not** a fourth mode or a
checkbox. It rides on the feedforward term, which is already automatic
whenever a zone has an identified model, and its coefficient is measured by
autotune rather than chosen by an operator. Asking a potter "should zone 2
compensate for zone 1?" is asking a question they have no way to answer, and
`COMMISSIONING_UX.md`'s own rule is to ask only what cannot be derived. The
correct surfacing is read-only provenance — the coupling-matrix display that
already exists on the page, showing the measured coefficient once real data
populates it, so the compensation is visible without being a decision.

## 4. Implementation phases (plan only — no code in this pass)

**Checkbox key.** `[x]` = built and in the tree today (verified against the
code, not assumed). `[ ]` = outstanding. Phase 0 is the pre-existing
foundation; Phases 1 and 3 have since been implemented against this plan, so
a `[x]` outside Phase 0 means this plan's own work has landed and passed host
tests — not that it is wired up or reachable by an operator. Nothing is
selectable in the UI until Phases 2, 4 and 5 land.

### Phase 0 — what already exists (no work required)

- [x] FOPDT `{K, tau, L}` fit from a step-response trace, two-point
      28.3%/63.2% method — `pid_autotune_fit_fopdt()`
- [x] SIMC gain computation from the fitted model (`lambda = 3*L` default) —
      `pid_autotune_tune_from_fopdt()`
- [x] Relay-feedback `{Ku, Tu}` identification, with ZN / Tyreus-Luyben rules
      on that path only — `pid_autotune_fit_relay()` /
      `pid_autotune_tune_from_relay()`
- [x] Autotune state machine, step and relay methods, guard coverage, 4h
      budget, explicit-accept-only gain write — `autotune_engine.c`
- [x] Model persisted per zone alongside gains (`zones_config_set_model()`)
- [x] Model-based feedforward duty term, gated off when no model identified
- [x] Per-zone control-mode select (Off / Bang-bang / PID) and Kp/Ki/Kd
      inputs — `zones_page.html:491-492`, `:561-563`
- [x] RGA coupling-matrix math and the per-zone sampling during a step test
      that is meant to populate it — `pid_autotune.c`, `autotune_engine.c`
- [x] **Coupling matrix populated with real data** — full 3x3 measured on
      the bench rig 2026-08-30 (step method, duty 0.4). Measured cells
      (`K` in °C/duty, `tau` and `L` in s):

      | | j=0 | j=1 | j=2 |
      |---|---|---|---|
      | **i=0** | K=32.648, tau=163.7, L=40.2 | K=5.863, tau=181.4, L=65.5 | K=2.812, tau=163.9, L=84.7 |
      | **i=1** | K=10.887, tau=123.2, L=56.7 | K=20.969, tau=113.5, L=39.3 | K=3.332, tau=125.6, L=65.4 |
      | **i=2** | K=7.723, tau=123.1, L=93.2 | K=8.625, tau=127.8, L=83.7 | K=23.641, tau=124.7, L=40.8 |

      Dead time rises monotonically with distance from the driven zone in
      every row (row 2: 40.8 self, 83.7 to zone 1, 93.2 to zone 0), which is
      the check that the fits track real heat transport rather than baseline
      drift. Each run was started only after the rig settled to < 0.8 °C
      spread, because a step test on a falling baseline fits a cross-gain as
      *negative*.

      **The matrix is not symmetric in any pair.** 0<->1 is 5.863 vs 10.887
      (1.86x), 0<->2 is 2.812 vs 7.723 (2.75x), 1<->2 is 3.332 vs 8.625
      (2.59x) — every higher-index zone drives more into its neighbours than
      it receives back. Normalized to self-gain, zone 1 sends 52% of its
      self-gain into zone 0; zone 2 spreads roughly evenly (33% into zone 0,
      37% into zone 1) — much of the raw asymmetry is explained by zone 0's
      larger self-gain (32.648 vs 20.969 / 23.641). §2c's feedforward must
      use `c_ij` indexed in the correct direction, not a single shared
      coefficient per zone pair.

      Zone 2's two cross-couplings (7.723 into zone 0, 8.625 into zone 1) are
      within 12% of each other — a schema that stores only one neighbour per
      zone would discard a coefficient of nearly equal weight, not a
      negligible one. See Phase 3b finding 3 below, which this data makes
      concrete.
- [x] **RGA computed from real data** — first RGA on this hardware,
      2026-08-30, now 3x3 over all zones: `det = 13700`,

      ```
      [ 1.1131  -0.0992  -0.0140 ]
      [-0.0909   1.1484  -0.0575 ]
      [-0.0222  -0.0492   1.0715 ]
      ```

      Diagonal 1.07-1.15, off-diagonals small and negative, rows sum to 1:
      weak interaction, and the diagonal pairing (zone *i* controlled by
      heater *i*) is confirmed correct for all three zones — no repairing
      needed. This strengthens the existing recommendation against full MIMO
      decoupling rather than changing it. §1a's [W8] caveat stands
      unchanged: this is a steady-state RGA taken near ambient on a
      0..80 °C rig, so it does not settle the `T^4` regime a real firing
      reaches.
- [x] **Zones 1 and 2 configured** — all three zones have now been driven and
      fitted: zone 1 (K=20.969, tau=113.5, L=39.3; SIMC proposal kp=0.03443,
      ki=0.00030, kd=0.67673, *not* accepted) and zone 2 (K=23.641,
      tau=124.7, L=40.8, step method at duty 0.4, done 2026-08-30). Zone 0 is
      `mode 2` (PID) with hand-entered gains (Kp=2.0, Ki=0.1, Kd=1.0), not
      autotuned ones. Proposed gains for zones 1 and 2 were reviewed, not
      accepted.
- [x] **An autotune run completed on real hardware** — done 2026-08-30.
      Zone 0 (K=32.648, tau=163.7, L=40.2), zone 1 (K=20.969, tau=113.5,
      L=39.3) and zone 2 (K=23.641, tau=124.7, L=40.8), all step method at
      duty 0.4. `PID_CONTROL.md`'s "neither method has ever completed on
      hardware" is now stale.
      Proposed gains were reviewed, not accepted; every recommendation in §2 rests
      on it. Note the rig's zones are range-limited to 0..80 °C, so a bench
      autotune identifies the plant near ambient — useful for validating the
      *mechanism*, but it does not settle §2a's `T^4` gain-variation caveat,
      which needs a real firing to observe.

### Phase 1 — Cohen-Coon tuning rule (small, de-risks nothing new)

`pid_autotune.c` computes SIMC from the fitted FOPDT model, and ZN /
Tyreus-Luyben from relay `{Ku, Tu}` data — the two rule sets are on separate
paths and each refuses the other's inputs (§0). [9]'s finding motivates adding
Cohen-Coon as a second rule on the **FOPDT** path specifically, since it takes
the same `{K, tau, L}` inputs SIMC does. Read with §2a's caveat: Cohen-Coon is
the more aggressive of the two and must not displace SIMC as the default.

- [x] Add `pid_autotune_tune_from_fopdt()` Cohen-Coon branch (same inputs, new
      published formula). `AUTOTUNE_RULE_COHEN_COON` appended as value 3;
      SIMC remains 0/default.
- [x] Host test: Cohen-Coon output on a known `{K, tau, L}` matches the
      published formula (hand-computed against `{K=400, tau=1000, L=30}`),
      SIMC unchanged, ZN/TL still refused on this path.
- [x] Assert Cohen-Coon really is the more aggressive rule (`cc.kp > simc.kp`)
      — the plan claims it, so it is now proven rather than asserted in prose.
- [x] Degenerate-`L` handling: Cohen-Coon divides by `L`, and unlike SIMC has
      no substitute that preserves the rule's meaning (the whole rule is
      parameterized by `L/tau`). Below
      `AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S` (0.5 s), or `tau <= 0`, or
      `K == 0`, it **refuses** — all-zero gains — rather than clamping `L` to
      an epsilon and emitting an arbitrarily large Kc that looks like a real
      answer.
- [x] **Refusal is now machine-readable.** `autotune_gains_t` carries
      `autotune_refusal_t refusal` plus `char refusal_reason[96]`, mirroring
      `fopdt_model_t`'s existing `valid`/`invalid_reason` pattern. Every
      refusal path in *both* `pid_autotune_tune_from_fopdt()` and
      `pid_autotune_tune_from_relay()` sets a distinct code, and the strings
      name the offending value ("Cohen-Coon needs dead time >= 0.50 s; this
      fit has L = 0.210 s"). Tests assert the codes are pairwise distinct —
      a single "some reason was set" check would have missed the whole point.
- [x] **Negative plant gain refused on both FOPDT rules** (found in review):
      `K < 0` — a step test begun while the kiln was still cooling, or a
      relay wired to the wrong zone's thermocouple — used to yield negative
      Kp/Ki/Kd. Cohen-Coon's guard tested `== 0`; SIMC had no gain check at
      all. Both now refuse.
- [x] **Refusal reason surfaced in the UI.** `zones_page.html:1513-1524`
      reads `s.refusal`/`s.refusal_reason` and displays "Tune refused (...)"
      when a rule returns no gains, falling back to the bare code if the
      reason string is empty.
- [x] **Cohen-Coon is now reachable.** `autotune_engine_run()`
      (`autotune_engine.c:1201-1214`) takes a `rule` parameter and refuses
      anything but SIMC/Cohen-Coon on the step path; `s_at.step_rule` (set
      `:1217`) is what `autotune_engine.c:353` passes to
      `pid_autotune_tune_from_fopdt()`. `dashboard_http.c:2051-2061` parses an
      optional `rule` field on `POST /api/autotune/start`'s step-test path,
      defaulting to SIMC and refusing `zn`/`tl` at the door.
- [x] **`/api/autotune` serves `refusal`/`refusal_reason`.**
      `autotune_status_get_handler()` (`dashboard_http.c:1791`
      `autotune_refusal_name()`, JSON body `:1823-1845`) emits `"refusal"` and
      `"refusal_reason"` alongside `proposed_gains`, so a refused rule no
      longer presents as three silent zeros.
- [x] Radio option on the autotune section of `zones_page.html` — landed with
      the two firmware gaps above; not separately re-verified line-by-line in
      this pass, but the `rule` field it posts is consumed end to end.
- [ ] `docs/PID_CONTROL.md`: comparison table — **deferred until bench data
      exists**, per this repo's "measured on the bench" convention. Do not
      write a comparison from the literature alone.

No new data structures, no new wire format. Ships independently of everything
below.

### Phase 2 — data structures — LANDED (with two post-review repairs)

Two bugs found in the opus review of this phase would each, on their own,
have destroyed a commissioned board's configuration on the first boot after
the update. Both are fixed; recorded here because the failure mode is the
kind that gets reintroduced:

1. **The frozen `zone_cfg_v9_t` was actually v8's shape** — it interleaved the
   `uint8_t` members, where real v9 groups them all at the tail. Each isolated
   `uint8_t` pads to the next float's alignment, so the struct measured 124
   bytes against the real 116; `expected_len_for_version(9)` could then never
   match a stored blob, `decode_zones_blob()` would call every v9 board's
   config corrupt, and the board would boot to factory zone defaults — cal
   offsets, PID gains, guard thresholds, temperature limits and relay/TC
   wiring all silently gone. Fixed, and pinned with
   `_Static_assert(sizeof(zone_cfg_v9_t) == 116)` so a future edit fails the
   build instead of a user's kiln. The migration test did not catch it because
   it staged a blob of `sizeof(zones_cfg_v9_t)` — self-consistent with the
   wrong layout.
2. **`ZONES_CONFIG_BLOB_MAX_SIZE` 512 → 640 silently deleted every saved kiln
   config.** That macro is not only a runtime ceiling: it sizes
   `kiln_cfg_entry_t::blob`, a member of the *persisted*
   `kiln_cfg_store_blob_t`. Widening it changed `sizeof`, and
   `nvs_load_store()`'s `len != sizeof(loaded)` check treats any other size as
   corruption and resets to an empty store — losing every named kiln config
   and `active_id`. Fixed by bumping `KILN_CFG_STORE_VERSION` to 2, freezing
   `kiln_cfg_store_blob_v1_t`, and migrating v1 → v2 (copy each entry, blob
   into the wider array, remainder zeroed) before the exact-size check runs.

Remaining items from that review, not yet done:

- [x] Optional-field probe fixed. `zone_field_present()` now treats only a
      genuinely absent key (`-1`) as omitted; present-but-empty (`0`) and
      over-long (`-2`) are refused. The older guard/timing fields keep their
      pre-existing bare `> 0` probe — deliberately not changed as a side
      effect of this phase.
- [x] `settings_source` self-reference refused (the degenerate cycle Phase 5
      would otherwise have to unwind). CUSTOM and other-zone still accepted.
- [x] `coupling_neighbor_zone` integrality checked, so a fractional index is
      refused rather than silently `(int)`-truncated at Phase 3b's use site.
      **Note for whoever writes the next test here:** the first version of
      this check's test used `2.7`, which the *range* check (0..2, since
      `THERMO_CHANNEL_COUNT` is 3) already rejects — it passed identically
      with the integrality check deleted. It now uses `1.5`, in range, so
      only integrality can reject it. Caught by the deliberate-break pass,
      which is the fourth near-miss of this kind in this repo.
- [~] **Backup/restore of the four new fields — import done and well tested,
      export untested.** Import: `backup_http.c:280-320` (field parsing),
      `:1140-1220` (validation), `:1352-1370` (commit); tests in
      `test_backup_import.c:916-1015` cover the v3 round-trip, v2-body
      defaulting (`settings_source` lands at `ZONE_SETTINGS_SOURCE_CUSTOM`
      0xFF, never 0 — the same trap that nearly destroyed commissioned
      configs in the v9->v10 NVS migration), the fuzzy-strength range
      refusal, the coupling-neighbor integrality refusal, and the
      self-reference refusal. Export: `backup_http.c` emits all four keys
      (`"fuzzy_strength_pct"`/`"coupling_coeff"`/`"coupling_neighbor_zone"`/
      `"settings_source"`, same block as `:280-320`), but **no test exists
      for the export path** — there is no `test_backup_export.c` and no
      export-shaped test in the tree, so the emitted JSON shape is unverified.
      Gap to close: an export test asserting the four keys appear with the
      correct values for a known zone config.
- [x] Stale `512` references corrected in `kiln_cfg_store.h`, `zones_http.c`
      and `zones_http.h`.

Original phase description follows.

### Phase 2 — data structures — DONE (built differently than planned in one place)

- [x] `zone_control_mode_t` (`zones_http.h:637`) gained `ZONE_CONTROL_MODE_PID_FUZZY = 3`,
  appended at the tail as planned. Parse ceiling enforced at
  `zones_http.c:4047` (0-3); test coverage `test_zones_http.c:1722-1746`.
- [x] New per-zone fields landed in blob v10 (`zones_http.c:378-381`,
  `_Static_assert(sizeof(zone_cfg_v9_t) == 116)` at `:911`, v9->v10 migration
  `:1005`/`:1303-1306`):
  - [x] `fuzzy_strength_pct` — stored as a **float**, not the originally
    planned u8, and defaults to **0** (no-op / classic-PID-identical), not
    30. 30 is only `zones_page.html:590`'s UI seed value when a zone has
    never set it.
  - [x] `coupling_coeff` + `coupling_neighbor_zone` per zone (§2c) — landed as
    planned, one scalar + one neighbor index per zone, 0 the safe default.
    See Phase 3b below for why this single-neighbor shape cannot hold the
    full coupling data set.
  - [x] The rule table stayed a firmware-wide constant, not persisted per
    zone, exactly as planned (`pid_fuzzy.c`'s `RULE_TABLE`).
- [x] **Built differently than this section originally planned, and this is
  the correct design, not a shortfall:** `pid_state_t` did **not** gain a
  fuzzy flag/pointer. Instead, `profile_executor.c`'s per-zone tick computes
  a per-tick `pid_cfg_t` copy — `pid_fuzzy_prepare_gains()`
  (`profile_executor.c:1620-1660`) calls `pid_fuzzy_adjust()` to produce
  adjusted `kp`/`ki`/`kd`, then `pid.c`'s `pid_rescale_integral_for_new_ki()`
  (`pid.c:25`) bump-transfers the integral term across the resulting Ki
  change before `pid_family_zone_tick()` (`:1487`) runs the same
  `pid_update_terms()` body classic PID uses, just fed the adjusted config.
  This keeps `pid.c` itself unaware fuzzy mode exists — no branch inside the
  PID module, no parallel control loop — and solves the bump-transfer hazard
  (formerly listed at line ~797) as a side effect of the same call, which a
  bare flag/pointer on `pid_state_t` would not have done on its own.

### Phase 3 — control-loop change (fuzzy layer) — DONE

- [x] `pid_fuzzy.c`: `pid_fuzzy_adjust()` (`pid_fuzzy.c:135-226`) implemented
  exactly as planned — Mamdani product AND over the 3x3 {error}×{error rate}
  rule table, weighted-average defuzzification, `strength_pct == 0` returns
  the (sanitized) base gains bit-for-bit. Signature takes this zone's own
  error/rate only, no neighbor input, per §2c. Host tests in `test_pid_fuzzy.c`.
- [x] `profile_executor.c`'s per-zone tick calls it. `ZONE_CONTROL_MODE_PID_FUZZY`
  (`profile_executor.c:2007-2016`) calls `pid_fuzzy_prepare_gains()`
  (`:1620-1660`) to get adjusted gains, then runs the same
  `pid_family_zone_tick()` (`:1482-1485`) the classic-PID case uses.
  Feedforward is untouched by this phase, as planned.

**Three wiring hazards found in review (2026-08-30). All three resolved before
shipping:**

- [x] **`error_rate_c_per_s` producer — resolved.** It has one:
      `profile_executor.c:1625-1626` feeds `z->pid_state.d_filtered`
      (`pid.c`'s existing low-pass-filtered derivative-on-measurement, negated
      once inside `pid.c`'s `raw_d` computation), not a raw per-tick finite
      difference. The `dSP/dt` question is settled in writing at
      `pid_fuzzy.c:12-33`: the setpoint's own ramp rate is deliberately
      **excluded** — `d_filtered` reflects only measurement-vs-time, so during
      a profile ramp the axis measures tracking error's rate, not the
      commanded ramp itself, once `RATE_BAND_C_PER_S` was rescaled (next item).
- [x] **`RATE_BAND_C_PER_S` — resolved by rescaling 0.05 → 0.5.**
      (`pid_fuzzy.c:45`, rationale at `:12-43`.) At 0.5 °C/s (30 °C/min,
      ~6x the fastest ramp this kiln's profiles command), an ordinary firing's
      ramp now sits inside the STEADY bucket for its whole duration, so the
      axis measures disturbances, not the profile. `test_pid_fuzzy.c:191,206`
      assert a normal ramp stays in the STEADY bucket.
- [x] **Bump transfer on `Ki` change — resolved.** `pid.c:25`
      `pid_rescale_integral_for_new_ki()` (a new function, not
      `pid_seed_bumpless()`) rescales `integral` so `ki*integral` — the I
      term's actual contribution — holds constant across a Ki move. Called
      every tick from `profile_executor.c:1650`; tests in
      `test_pid.c:134-196` include a negative test proving an un-rescaled Ki
      cut really does step duty, so the positive assertion is not vacuous.
- [x] `autotune_engine.c` needs **no change**: it always identifies/writes base
  gains regardless of which control mode a zone is currently in, matching
  how it already behaves for bang-bang zones (autotune is always available,
  its acceptance just changes what number a mode later reads).

### Phase 3b — cross-zone coupling feedforward (§2c) — NOT STARTED, schema question open

Sequenced after, not blocked by, the coupling measurement. The rig can
produce the data (see Phase 0) — the ordering rule is only that the
coefficient must be **measured before it is trusted**, never guessed. Build
the persist path and the tests first; they are what make the measurement
usable. Do not ship the feedforward contribution enabled by default until a
real matrix exists for the installation.

**Findings from this pass (2026-08-30), verified against the current tree:**

1. **Nothing persists the matrix.** `autotune_engine.c:358-380` fills
   `s_at.coupling` in RAM only during a step test. `zones_config_set_coupling()`
   is never called from `autotune_engine.c` — grep confirms its only callers
   are `zones_http.c` (the setter itself) and `backup_http.c:1359` (backup
   import). So a measured matrix dies at reboot unless an operator manually
   re-enters it through backup import; the autotune-to-storage path does not
   exist yet.
2. **No coupling term exists in the feedforward.** `zone_feedforward()`
   (`profile_executor.c:468-482`) computes `hold + climb` only.
   `coupling_coeff` has zero references anywhere in `profile_executor.c`.
3. **The schema cannot hold the data as currently shaped.** `zone_cfg_t` has
   ONE `coupling_coeff` + ONE `coupling_neighbor_zone` per zone. Asymmetry IS
   representable today — the coefficient is stored per *source* zone, so
   c(1->0)=10.887 and c(0->1)=5.863 coexist in the measured Phase 0 data — but
   **multiplicity is not**: zone 1 cannot store c(1->2)=3.332 alongside
   c(1->0)=10.887 in the current one-neighbor-per-zone shape. Discarding N-2
   neighbors is a real limitation once a kiln has 3+ zones. Fixing it means a
   directed NxN set — `float coupling_coeff[MAX31856_CHANNEL_COUNT]` per zone
   — which changes `sizeof(zone_cfg_t)` and therefore needs the same
   discipline that just prevented two config-destroying bugs this session
   (§Phase 2's post-review repairs): a frozen `zone_cfg_v10_t`, a static
   assert pinning its size, `ZONES_CFG_VERSION` 10->11, a migration function,
   and possibly a `KILN_CFG_STORE_VERSION` bump if the wider blob no longer
   fits the existing ceiling. This is the same change class that caused two
   config-destroying bugs on 2026-08-30 (the v9 struct layout bug and the
   `ZONES_CONFIG_BLOB_MAX_SIZE` widening bug, both under Phase 2) — treat any
   future edit here with that history in mind.
4. **Sign convention question, currently unresolved.** `coupling_coeff` is
   validated non-negative both at the door (`zones_http.c:2712`,
   `zones_config_set_coupling()`) and in blob validation (`zones_http.c:3398`).
   §2c's design is `-c_ij*(T_j - sp_j)` — the *term* carries its own sign via
   `(T_j - sp_j)`, and `c_ij` itself is meant to be a non-negative magnitude,
   so the current non-negative validation matches the design as written in
   §2c and is very likely intentional, not a bug — but this has not been
   explicitly confirmed against an implementation, since none exists yet.
   Flag this for whoever writes Phase 3b's feedforward code: confirm the sign
   lives entirely in `(T_j - sp_j)` before assuming the validator needs
   loosening.

- [x] Configure zones 1 and 2 and run a step test with cross-zone sampling,
      to produce the first real coupling matrix (Phase 0) — full 3x3 matrix
      and RGA recorded above (2026-08-30).

- [ ] Decide and design the schema change in finding 3 above before writing
      any persistence code — this is the highest-risk step in this phase.
- [ ] `autotune_engine.c`: persist the per-pair coupling coefficient the step
      test already measures, instead of only reporting it — this is the
      measurement path that exists but currently feeds nothing.
- [ ] Feedforward term gains the additive `-c_ij * (T_j - T_j_setpoint)`
      contribution for the strongest-coupled neighbor, inside the existing
      clamp on the summed duty (not a separate clamp).
- [ ] Host test proving `c_ij = 0` reproduces today's feedforward output
      bit-for-bit (the negative test for this feature).
- [ ] Host test for sign correctness: neighbor hot subtracts duty, neighbor
      cold adds it. A sign error here actively drives zones apart, so this is
      the one test that must exist before this ships anywhere near hardware.

### Phase 4 — HTTP endpoints — DONE

- [x] `POST /api/zones`: `parse_zone_fields()` accepts `z%u_mode` up to the
  fuzzy value (`zones_http.c:4045-4047`) and `z%u_fuzzy_strength`
  (0-100, `zones_http.c:4290-4323` alongside `z%u_coupling_coeff`/
  `z%u_coupling_neighbor`, same range-refusal-at-the-door pattern as
  `z%u_minon`, plus preserves the previous value when the field is absent).
- [x] Negative test: `test_zones_http.c:1775-1795`
  (`test_post_fuzzy_strength_out_of_range_refused_not_clamped`) proves 101
  and -1 are refused outright, with positive controls at the 0/100 boundary.
- [x] `GET /api/zones`: echoes `mode`, `fuzzy_strength_pct`, `coupling_coeff`
  and `coupling_neighbor_zone` per zone (`zones_http.c:3692-3706`). Round-trip
  test `test_zones_http.c:1939-1956`.
- [x] No new autotune endpoint — `/api/autotune/*` stays exactly as documented
  in `PID_CONTROL.md`; fuzzy mode consumes its output, it does not change
  its contract.
- [ ] Optional, later: `GET /api/zones` could also report the *live* per-tick
  fuzzy-adjusted gains (as `/api/control`'s `pid_terms_t` already reports
  `ff` alongside P/I/D) so the tuning panel's rule-strength table can show
  which cell is currently active, not just the static table — a nice-to-have
  once the base mechanism is proven, not a Phase 4 requirement.

### Phase 5 — UI changes

- [x] `zones_page.html`: the mode `<select>` (`modeHtml`, line ~548) has the
  fourth option, and the fuzzy tuning panel (`.fuzzyPanel`, lines ~599-606:
  rule-strength table + "Adjustment strength" input) sits alongside the PID
  Kp/Ki/Kd block (lines ~592-598). Both `.kp`-style class selectors and the
  `z%u_kp`-style wire fields carry the new field.
- [~] **Zone settings inheritance (§3.5) — implemented, negative tests
  missing.** The per-zone "Same as zone N / Custom" dropdown and read-only
  inherited display (`zones_page.html:819-1000`), re-resolution on save
  (`:1309`, `:1361-1365`), and the `settings_source` field in the config blob
  (`zones_http.c:2739-2760`) are all in the tree. But the three negative
  tests §3.5 demands do **not** exist: no host test for a save/reload
  inheritance round-trip, no test that a cycle collapses to custom, no test
  that a channel shared by two zones reads back the same type on both —
  `settings_source` does not appear anywhere in `test_zones_http.c` in an
  inheritance-behavior test (it appears only in migration-default and
  POST-parse-range tests, e.g. `test_zones_http.c:1869-1920`). Gap to close:
  add the three negative tests §3.5 specifies.
- [x] **N/A — the web dashboard does not mirror this control.** Checked
  before assuming: `App/drivers/app.js` and `App/drivers/main_page.html`
  contain no `control_mode` or fuzzy references at all — `zones_page.html`
  is the only page carrying zone config edit today, so there is nothing to
  keep in sync.
- [ ] `docs/PID_CONTROL.md`: still needs a new "Fuzzy adjustment" section,
  in the same style as the existing "Feedforward" section — formula, what
  it is off by default when (`strength_pct=0` or no base model identified),
  and bench measurements once real hardware produces any. Not written yet.

### Phase 6 — host tests (required before any of the above is considered done)

Following this repo's own "negative-test every check" rule
(`docs/GUARD_TEST_MATRIX.md`'s convention, referenced repeatedly in
`PID_CONTROL.md`):

- [x] `strength_pct=0` reproduces the base gains bit-for-bit — `test_pid_fuzzy.c:17-43`
      (unit level) and `test_closed_loop.c:216-239` (integration level, through
      the actual `fuzzy_tick()` per-tick wiring, not just `pid_fuzzy_adjust()`
      in isolation).
- [x] Each rule-table corner (large positive/negative error × rising/falling)
      nudges in the intended direction — `test_pid_fuzzy.c:52-87`.
- [x] Closed-loop test mirroring `PID_CONTROL.md`'s existing "4-simulated-hour
      closed-loop run" — `test_closed_loop.c:340-356`: no false guard trip,
      settles within 10 °C of setpoint after 4 simulated hours in fuzzy mode.
      **Caveat:** `test_closed_loop.c`'s `fuzzy_tick()` (used by the tests
      above) is a hand-written mirror of `pid_fuzzy_prepare_gains()`'s logic,
      local to that test file — so a future edit to the real
      `pid_fuzzy_prepare_gains()`/`pid_fuzzy_adjust()` would not be caught
      there alone. A real guard against exactly that drift was since added:
      `test_profile_executor_prestart.c:1683-1711`
      (`test_fuzzy_prepare_gains_matches_pid_fuzzy_adjust_directly`) calls
      the actual production `pid_fuzzy_prepare_gains()` and diffs its output
      against a direct `pid_fuzzy_adjust()` call — confirmed present in the
      tree, not just claimed.
- [ ] The two §2c coupling tests from Phase 3b (zero-coefficient parity, and
      sign correctness) — **not started**, blocked on Phase 3b's feedforward
      code not existing yet (see Phase 3b above).
- [x] Negative tests exist proving several of the checks above can actually
      fail — e.g. `test_pid.c`'s "sanity: an UN-rescaled Ki cut... really does
      step duty" and `test_closed_loop.c`'s strength_pct=100 divergence check
      — following this repo's rule that a check must be provably falsifiable.

**Definition of done for this plan as a whole:** every box above checked,
plus at least one autotune run completed on the real rig (Phase 0), without
which none of §2's recommendations have been validated against anything but
simulation and literature. Note the bench rig's 0..80 °C range means even a
successful run validates the *mechanism* rather than the kiln-temperature
behavior — §2a's `T^4` caveat stays open until a real firing.
