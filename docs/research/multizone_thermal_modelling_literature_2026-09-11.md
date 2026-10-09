# Multi-zone thermal coupling: literature review for kilnCtl's refuted linear model

Date: 2026-09-11. Scope: find a replacement for the additive, duty-driven linear
coupling term in `firmware/KilnFW/App/test/sim_plant.c` (`G*u = b`, `G = diag(k) +
coupling_coeff`), refuted on hardware (see project memory
`project_z0_coupling_is_shape_not_scale.md` and the constraints below). This
review is filtered against those hardware constraints, not written as a general
survey. **Purpose note:** per the coordinator's mid-task instruction, this
review is explicitly aimed at what could be coded into `sim_plant.c`'s
fixed-step Monte-Carlo simulator, so each shortlisted class below carries a
discrete-time update, a parameter count keyed against data already captured on
this bench, a numerical-stability note, and a single-zone-regression check.

## The constraints (recap, used as a hard filter)

1. 3 zones stacked vertically, z0 top / z1 middle / z2 bottom, resistive/PWM,
   FOPDT per zone (k=42.7/32.4/33.8 °C/duty, tau=256/259/247 s, dead=40/31/26 s).
2. Single-column sweep (one zone driven, duty 0.237–0.920) is **linear**:
   flat ΔT/duty into the neighbour (22.7/22.5/21.5/21.9, <5% spread, no trend).
3. Joint drive **breaks superposition**: linear model over-predicts by ~33%
   at low level (17.6–17.8 vs 13.28 °C actual).
4. The residual **reverses sign with level**: same model under-predicts by
   9–31% across nine zone/plateau cells at 62–75 °C.
5. Refuted already: per-source superlinear (T^4/3) buoyancy term; any
   `k*P_total^gamma` (gamma>1) loss term — refuted because a single-column run
   reached 1.48x the joint case's total power with zero effect.
6. Surviving signature: deficit depends on **how power is split** across
   zones, not on the total.
7. Bench scale: ~4 W, 120 V, ΔT ≤ ~40 °C above ambient — convection/conduction
   regime, not the radiation-dominated regime of a real >1000 °C kiln.

I could not find any single published model built for exactly this
configuration (three stacked resistive zones in a small enclosed bench
fixture). Nothing below is a drop-in match; everything is adapted-with-caveats,
and I say explicitly where I'm inferring rather than quoting.

## Ranked shortlist

### 1. Shared-node (single lumped cavity/air-mass) RC network — top candidate

**Idea.** Instead of pairwise zone-to-zone coupling coefficients, each zone
exchanges heat only with one shared internal-air (or enclosure) node, and that
node exchanges heat with ambient. Zone-to-zone interaction is then an
*emergent* property of two zones both being coupled to the same third state,
not a direct term between them.

**Literature.** RC/lumped-parameter thermal-network models with shared nodes
are the standard architecture in multi-zone building thermal modelling:
- Sonta, Simmons, et al. (topology-learning RC networks for multi-zone
  buildings), *"Data-driven identification of a thermal network in multi-zone
  building"*, arXiv:1810.07400 (2018). https://arxiv.org/abs/1810.07400 —
  fetched this one directly (see PDF cached this session). It confirms the
  RC-network architecture (`T(k+1) = A·T(k) + B·u(k)`, zones as capacitor
  nodes, resistances as edges) and that a **naive topology assumes all-to-all
  connectivity, which then gets pruned** ("spurious two-hop neighbor links
  must be pruned during topology learning") — i.e., the paper's own
  identification procedure independently rediscovers that pairwise
  zone-to-zone links are often actually two hops through a shared node, which
  is exactly the restructuring proposed here.
- General LPTM (lumped parameter thermal model) surveys: e.g. Cen et al.,
  *"Lumped Parameter Thermal Network Modeling and Thermal Optimization Design
  of an Aerial Camera"*, PMC/NCBI, 2024, https://www.ncbi.nlm.nih.gov/pmc/articles/PMC11207309/
  — general RC-node methodology, not specific to sign reversal (cited for the
  node/edge formalism only, from-abstract-only for anything beyond that).
- Multi-zone oven patents (not peer-reviewed, cited only as engineering
  precedent for the "shared cavity, leakage between zones" framing): US
  Patent (thermal management system for multizone oven),
  https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10890336 —
  explicitly states "absolute isolation between cavities is not required and
  substantial leakage can be managed by active feedback control of cavity
  temperature," i.e. commercial multi-zone ovens are engineered around exactly
  this shared-air-mass coupling path rather than pairwise element-to-element
  radiation/conduction.

**Does it explain the sign reversal?** Partially, and only qualitatively — no
source found demonstrates a sign reversal from this architecture on real data.
The mechanism I can construct from the topology (not sourced, my own
derivation): a shared cavity-air node has its own heat capacity and its own
loss path to ambient. At low duty split unevenly (e.g. two zones on, one off),
the driven zones pump heat into the cavity node while the off zone still cools
that node toward ambient — a net *drain* effect on the neighbour that a
zone-pair linear coefficient (fit only at one operating point) cannot capture,
and its relative size shifts with the split. At higher average level, the
cavity node itself sits well above ambient and stops being a heat sink for the
neighbour, flipping the effective coupling from "loss" to "gain." This is
consistent with constraint 6 (depends on split, not total) but I have **not**
found a paper that measures this reversal in a lumped 1-node model
experimentally — flag this as reasoned-but-unverified, not literature-backed.

**Discrete-time update (for sim_plant.c).**
State: existing per-zone FOPDT states `T_z0, T_z1, T_z2` (or their lag-filter
internal states) **plus one new scalar state** `T_cav` (cavity/shared-air
temperature). Per-zone dynamics unchanged (still FOPDT driven by that zone's
own duty). Add:
```
T_cav[k+1] = T_cav[k] + dt/tau_cav * ( sum_i h_i*(T_i[k] - T_cav[k]) - (T_cav[k]-T_amb) )
T_i_effective_input[k] += h_i * (T_cav[k] - T_i[k]) * dt   // added to each zone's own energy balance
```
i.e. each zone's FOPDT gets one extra additive heat-flow term proportional to
`(T_cav - T_zone)`, and the cavity itself is a single first-order lag driven
by the (duty-weighted) sum of all three zones minus its own loss to ambient.

**Parameters added.** 4 new scalars: `tau_cav`, `h_z0`, `h_z1`, `h_z2` (coupling
conductance per zone into the shared node). Everything else (k, tau, dead per
zone) is unchanged.

**Identifiable from data already captured?** Partially. The single-column z2
sweep (0.237/0.530/0.547/0.920) constrains `h_z2` and, combined with the
known z2 FOPDT k/tau, the single-zone response gives one equation per duty
level (4 points) — enough to bound `h_z2*tau_cav` jointly but not separate
them without an assumed `tau_cav`. The matched-ΔT0 joint plateau (duties
0.163/0.210/0.247) and the 62–75 °C `cplval75` plateaus give two operating
points (low and high) with different apparent coupling sign — that is
*exactly* the two-point contrast needed to fit `tau_cav` (whose relative size
against zone tau's controls how "saturated" the cavity is at each level,
which is what would produce the sign flip in my qualitative story above). **No
new hardware run is strictly required to get a first fit**, but the fit is
under-determined (4 unknowns from ~2 independent operating regimes plus the
linear single-column check) — a confirmatory single extra joint run at a
third level would resolve the ambiguity, not a hard requirement to proceed.

**Numerical stability.** Trivial — one extra first-order lag state, explicit
Euler update, no iteration, no stiffness beyond what the existing FOPDT states
already have (same order of tau). Safe inside a 220-run fixed-step Monte
Carlo.

**Single-zone regression check.** With only one zone driven, `h_i` for the
other two zones is still nonzero (they're always thermally coupled to the
cavity), so this does NOT trivially reduce to "coupling off." However, because
that path's effect on the *driven* zone's own temperature is a second-order
loop (driven zone → cavity → back to driven zone) and the single-column data
is what calibrates `h_z2`, the model must be constrained during fitting so
that the same `h_z2` reproduces the measured flat, linear single-column
ΔT/duty curve. This is a real regression risk: unlike a pure "off-diagonal
term only" design, a shared node structurally touches the driven zone's own
balance too. **Concrete mitigation:** fit `h_i` jointly against BOTH the
single-column sweep (to pin down self-loop magnitude, target: ~0 net change
to the driven zone's own response) and the joint plateaus (to pin down cross
effect) — if no parameter set satisfies both, this candidate must be rejected
or restricted to a "one-way" cavity model where the driven zone injects heat
into the cavity but does not itself receive back-conduction (asymmetric `h`),
which weakens the physical story but preserves single-zone correctness by
construction.

### 2. Bilinear (input × state) coupling term

**Idea.** Add a term proportional to the product of one zone's duty and
another zone's (or the cavity's) temperature: `T_dot_i += sum_j N_ij * u_j *
T_i` or `* T_j`. This is standard in building-climate bilinear models, where
airflow/duty inputs multiply temperature states (physically: the "gain" of an
actuator scales with the state it's acting on, e.g. heat injected scales with
inlet-outlet ΔT which itself is state-dependent).

**Literature.**
- Bilinear state-space models for building thermal zones are well established;
  see the building-MPC literature located this session: *"Tractable and
  Robust Modeling of Building Flexibility Using Coarse Data,"* arXiv:1802.06165
  (2018), https://arxiv.org/abs/1802.06165, and *"A Bilinear Approach to Model
  Predictive Control for Thermal Conditioning of Adaptive Buildings,"*
  ResearchGate, https://www.researchgate.net/publication/343151731 — **both
  from-abstract-only**: WebFetch on 1802.06165 returned only the abstract and
  could not surface the governing equation text; I did not obtain the exact
  equation from either paper and am not asserting one.
- The generic bilinear building-thermal form quoted widely in this literature
  (paraphrased structural form, not a verbatim equation from a specific paper
  I could confirm): `x_dot = A x + B u + sum_i N_i x u_i + E d`, where `u_i` is
  an HVAC/airflow input and `N_i x u_i` is the bilinear coupling term. I flag
  this form as representative-of-the-class rather than sourced to one specific
  paper's exact notation.

**Does it explain sign reversal?** Structurally, yes — a bilinear term's sign
and magnitude scale with the state it multiplies, so `N_ij * u_j * T_i` can
flip sign as `T_i` crosses some reference (e.g. if `N_ij*(T_i - T_ref)` is the
actual form, common in HVAC bilinear models where the reference is a duct
setpoint). This is the constraint-5 mechanism candidate that most directly
produces a level-dependent sign change from a small parameter set. However, I
found no thermal bilinear paper that reports an experimentally observed sign
reversal of exactly this kind (all HVAC papers found describe airflow-times-ΔT
bilinear terms for energy balance, which is convection accounting, not an
inter-zone crosstalk sign flip) — this is inference from the general bilinear
structure, not a specific literature confirmation.

**Discrete-time update.**
```
T_i[k+1] = T_i[k] + dt * ( (k_i*u_i[k] - T_i[k])/tau_i_delayed
           + sum_{j != i} N_ij * u_j[k] * (T_i[k] - T_ref) )
```
`T_ref` a single new global (or per-zone) reference constant; `N_ij` up to 6
off-diagonal coupling coefficients (3 zones, could restrict to
nearest-neighbour z0-z1, z1-z2 only → 4 coefficients, physically justified by
vertical stacking).

**Parameters.** 4–6 `N_ij` plus 1 `T_ref` = 5–7 new scalars — more than
candidate 1.

**Identifiable from existing data?** The matched-ΔT0 joint low-duty plateau
and the 62–75 °C high-duty plateaus give exactly two `(u, T)` regimes per
zone pair — enough to solve for `N_ij` and `T_ref` per pair (2 equations, 2
unknowns) *if* the single-column linear sweep also constrains one boundary
condition (N_ij should predict ~0 net cross-effect change across the single
column's own duty range, which the 4-point sweep can check). This is
plausible with existing data but tighter than candidate 1 — more parameters,
fewer effectively independent operating points actually captured (2 plateaus
vs 4 unknowns pair-wise), so parts of `N_ij`/`T_ref` may be only weakly
constrained without a 3rd joint level.

**Numerical stability.** Explicit Euler, no iteration — fine for the
Monte-Carlo loop, same order as candidate 1. One caution: `T_ref` badly
chosen could make `N_ij*(T_i-T_ref)` large for some Monte-Carlo-sampled
initial conditions, which is a robustness (not stiffness) risk — needs a
sanity clamp during simulation, not a solver change.

**Single-zone regression check.** Same structural problem as candidate 1: the
term `N_ij*u_j*(...)` is exactly zero whenever `u_j = 0`, so if only zone i is
driven, every term with an undriven neighbour's duty as multiplier vanishes
identically — **this candidate reduces exactly to today's single-zone
behaviour with u_j=0 for all off zones, by construction, no fitting needed to
guarantee it.** This is a real advantage over candidate 1 (which touches the
driven zone's own balance through the always-present cavity even at u_j=0).

### 3. Setpoint/level-dependent gain scheduling (RTP-derived)

**Idea.** Rather than adding a coupling *state*, let the coupling
*coefficients themselves* be scheduled functions of the average zone
temperature (or duty level), following the rapid-thermal-processing (RTP)
multi-lamp literature's established practice of gain-scheduling a coupling
gain matrix against wafer temperature setpoint.

**Literature.** Multi-zone RTP systems (semiconductor wafer heating, 3+
independently controlled lamp zones with cross-coupled thermocouple readings)
are the most directly analogous published system I found — same
architecture shape (N independently driven heat sources, N measured zone
temperatures, an explicit N×N gain/interaction matrix), even though the
physical regime (radiative lamps, vacuum/low-pressure chamber, seconds-scale
dynamics) is very different from this bench fixture. Key sources:
- Norman, Gyurcsik, et al./Schaper, Edgar, et al. survey line on RTP
  multivariable control; specifically found via search (**from-abstract-only,
  I could not obtain the primary papers' full text this session**): "Gain
  scheduling was employed to compensate process nonlinearity as the
  temperature set point changes (Schaper et al. 1994; Edgar & Breedijk 1994)"
  and "the conditioning of the [lamp-zone] control system... is usually poor"
  — summarized via ResearchGate/secondary sources found at
  https://www.researchgate.net/publication/234065612_Modeling_and_Control_of_Rapid_Thermal_Processing
  and https://ir.lib.nycu.edu.tw/bitstream/11536/29999/1/000166627500015.pdf
  (this second one, "Control system design for a rapid thermal processing
  system," is a full PDF I did not deep-read for equations this session —
  flagging as a good next-step primary source, not yet verified in detail).
- The physical reason cited across this literature for setpoint-dependent
  gain in RTP is that lamp-to-wafer coupling is *radiative* and scales ~T^4,
  so the linearized gain around any one operating point necessarily differs
  from the linearized gain at another — this explains gain *magnitude*
  scheduling with level, which is a well-established published mechanism, but
  I found no RTP source claiming a coupling-gain **sign** reversal
  specifically (RTP gain matrices are all reported strictly positive off any
  citation I could verify). **This mechanism is a good analogy for "why
  coupling strength depends on level" but not literature support for "why it
  reverses sign."**

**Discrete-time update.** Replace the constant `coupling_coeff` matrix entries
with `coupling_coeff_ij(T_avg)`, e.g. a simple piecewise-linear or one-knot
lookup: `c_ij(T_avg) = c_ij_lo + (c_ij_hi - c_ij_lo) * clamp((T_avg -
T_lo)/(T_hi - T_lo), 0, 1)`, keeping the existing additive `G*u=b` solve
structure, i.e. this is the smallest possible patch to the CURRENT code path
(no new state at all — just recompute `G` each step from current temperatures
instead of holding it constant).

**Parameters.** 2 numbers per off-diagonal entry (`c_ij_lo`, `c_ij_hi`) instead
of 1 — doubling today's coupling-matrix parameter count, or fewer if
restricted to nearest-neighbour pairs.

**Identifiable from existing data?** Yes, directly and cheaply: `c_ij_lo` from
the matched-ΔT0 low-duty joint plateau (13.28 °C actual vs. today's fit), and
`c_ij_hi` from the 62–75 °C `cplval75` plateaus — this is literally what the
two already-captured operating points are for. **No new data needed** to fit
this candidate; it is the cheapest of the three to parameterize.

**Numerical stability.** Best of the three — same linear solve `G*u=b`
already in the code, done once per step with a recomputed `G`; no new ODE
state, no iteration.

**Single-zone regression check.** `c_ij(T_avg)` interpolation still applies
even with one zone driven (T_avg still moves), so the off-diagonal term is
still nonzero unless duty in the other zone is exactly what zeroes it out in
today's model already. Concretely: today's off-diagonal terms are
"DUTY-driven" (`coupling_coeff * u_j`), and if that structure is kept (only
the *coefficient* is scheduled, the multiplication by `u_j` is unchanged),
then **`u_j=0` still zeroes the term exactly**, same guarantee as candidate 2.
This preserves single-zone correctness by construction and is the safest
candidate on this criterion.

**But:** this candidate can only produce a sign reversal if `c_ij_lo` and
`c_ij_hi` are fit with opposite signs — which is just curve-fitting the
observed reversal into two calibration points, not a from-first-principles
explanation of *why* it reverses. It is the cheapest-to-implement and
lowest-risk option, but it is descriptive/interpolative rather than
mechanistic, and it will not generalize to any third operating point that
wasn't part of its calibration set (unlike candidates 1–2 which have some
claim to extrapolating from a physical mechanism).

## Recommendation ranking (for the implementation pass)

1. **Ship candidate 3 (scheduled-gain matrix) first** — smallest code delta,
   uses only already-captured data, zero new numerical-stability risk,
   provably preserves single-zone linearity, and directly targets the sim's
   acceptance harness (see below). It is curve-fitting, not physics, but it
   is honest curve-fitting bounded by real measurements, and it's the one
   most likely to move the A1 bar without introducing new risk.
2. **Candidate 2 (bilinear term)** as a follow-up if the scheduled-gain patch
   under- or over-shoots at a third, not-yet-calibrated operating point —
   it has the cleanest "reduces to today's single-zone model when u_j=0"
   guarantee of the two state-adding options, and a plausible (if
   unconfirmed) mechanism for the sign flip.
3. **Candidate 1 (shared-cavity node)** is the most physically motivated but
   carries the real single-zone regression risk described above and needs
   careful joint fitting against the single-column sweep before it can be
   trusted; treat it as a research spike, not the first patch.

## Acceptance criterion

Per `docs/audits` convention, the existing acceptance harness is
`firmware/KilnFW/App/test/sim_iter_tune.c` /
`firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`. Its **A1 bar is
currently pinned at 24/660 false accepts (3.64%)**, attributed to the present
additive coupling model, against a documented design target of 2.0%. Any of
the three candidates above is a genuine coupling-model change and therefore
**expected to move that number** — the pin's own exit condition calls for
re-measuring A1 after such a change. Whichever candidate is implemented,
re-run `check_sim_iter_tune_bars.ps1` and report the new A1 rate as the
acceptance result; do not treat "the sim compiles and single-zone tests still
pass" as sufficient on its own.

## Does not apply, and why

- **Deep-learning multi-zone furnace prediction** (graph attention + GRU,
  ScienceDirect, https://www.sciencedirect.com/science/article/abs/pii/S0735193325010504,
  from-abstract-only): data-hungry, not interpretable as physical parameters,
  cannot be hand-identified from ~3 bench operating points, and gives no
  mechanistic account of sign reversal — wrong tool for a 220-run
  deterministic host-test Monte Carlo with a handful of calibration points.
- **CFD/FEA reflow-oven thermal-coupling models** (MpCCI/FLUENT+ABAQUS
  board-level coupling, ResearchGate 235311073): far too expensive
  computationally for a fixed-step Monte-Carlo host test, and built for
  radiation-dominated forced-convection tunnels at PCB-reflow temperatures
  (183–260 °C, forced air), not a ~40 °C-rise natural-convection bench box —
  regime mismatch on both axes (compute cost and dominant transport mode).
- **Radiation-dominated real-kiln models** (any treatment where
  Stefan-Boltzmann T^4 radiative exchange dominates): explicitly excluded by
  the scale caveat in the task — this fixture never exceeds ~40 °C above
  ambient, squarely in the convection/conduction regime, so any model whose
  coupling mechanism is fundamentally radiative (as in RTP lamp-to-wafer
  coupling, or a real >1000 °C kiln) needs its scaling re-derived from
  scratch before it could apply here, if it applies at all. RTP's *lesson*
  (gain depends on level) is borrowed above; RTP's *mechanism* (T^4 radiative
  transfer) is not — flagged so this distinction isn't lost.
- **Buoyancy/superlinear power-loss terms** — already refuted on hardware per
  the task's own constraints (T^4/3 buoyancy exponent; `k*P_total^gamma`
  loss); the natural-convection CFD literature I found on stacked heated
  cylinders/enclosures (ScienceDirect S2214157X25010238; Buoyancy Driven
  Natural Convection Flow… ResearchGate 257726656) confirms
  Rayleigh-number-dependent regime transitions are real and well studied, but
  every such source models a *fixed, unchanging enclosure geometry* with heat
  sources at fixed locations — none reports a *duty-split-dependent* sign
  reversal of the kind measured here, and re-deriving one from CFD first
  principles is out of scope for what could be validated against 2-3 bench
  operating points.
- **Building-topology RC identification requiring many sensors/rooms**
  (arXiv:1810.07400's own topology-learning algorithm): oriented at
  identifying *unknown* adjacency among many (5+) rooms from streaming sensor
  data; with only 3 known-adjacent zones and a handful of calibration points,
  the topology-learning machinery itself is unnecessary overhead — only its
  shared-node *architecture* (candidate 1 above) is worth borrowing, not its
  identification algorithm.

## On the sign reversal specifically

**No paper found in this search reports an experimentally measured,
level-dependent sign reversal of inter-zone thermal coupling in a system
resembling this one.** The RTP literature confirms coupling *magnitude*
depends on operating level (via radiative T^4 scaling, a mechanism that does
not directly apply at this fixture's scale) but not a sign flip. The bilinear
building-control literature offers a plausible mathematical *form* that can
produce a sign flip (state-dependent coupling terms crossing a reference
temperature) but I did not find a paper demonstrating this flip on real
thermal hardware — it is a structural possibility from the model class, not a
confirmed physical account. The shared-cavity-node story in candidate 1 is my
own reasoned inference from the topology, not sourced to any paper that
measured it. **This is a legitimate finding, not a gap in the search:** the
literature explains *that* multi-zone thermal coupling is commonly nonlinear
and level-dependent in magnitude, but I found nothing that explains *why it
would reverse sign* in a small resistive/convective enclosure. Candidate 3
(scheduled gain, fit directly to the two already-captured plateaus) sidesteps
needing that explanation by treating the reversal as an empirical calibration
target rather than a predicted consequence of a mechanism — which is the
honest and immediately implementable path given what is actually known.

## What data would most directly resolve this

A third joint-drive plateau at an intermediate level (between the ~13 °C
low-duty point and the 62–75 °C high-duty points) would let any of
candidates 1–3 be checked for whether the sign transition is a smooth
crossing (supports the bilinear/shared-node story) or an abrupt regime change
(would suggest something more like a convection-pattern flow reversal, per
the CFD literature on stacked heaters, though as noted that literature was not
found to model duty-split dependence). This is flagged as a "next hardware
run" suggestion per the task's request, not something this research task
performed or is authorized to schedule.
