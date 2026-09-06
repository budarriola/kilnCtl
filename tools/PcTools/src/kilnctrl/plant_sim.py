"""Calibrated FOPDT+coupled-PID plant simulator for kilnCtl's 3-zone profile
executor -- promoted from a session scratchpad's ``calibrated_sim.py`` /
``calibration_compare.py`` pair once the calibration held up across five
hardware captures (baseline / after / ifix / holdfix_clean / final -- see
``tools/PcTools/tests/fixtures/plant_sim/`` and the "TRUST" section below).

THE BUG THIS REPLACES (read before touching ``run_profile`` or
``segs_from_capture``): every simulator upstream of this one
(``ease_off_sim.py``, ``dwell_entry_sim.py``, ``pid_floor_sim.py`` and this
module's own direct ancestor ``calibrated_sim.py`` before its first commit)
drove itself off a HARDCODED ramp rate, ``rate_c_per_s = 10.0/60.0`` (10
C/min). The real profile the five captures ran ramps at 2-3.5 C/min --
3-5x slower. Because the coupled climb feedforward is linear in the
commanded rate (``climb = tau * rate / K``), that single wrong constant
overstated and saturated the climb term on every ramp, handed the PID a
large wrong-sign correction to fight for the whole ramp, and produced a
phantom 5-7 C mean ramp error that gain retuning could not fix (it wasn't a
gain problem). It made the old simulator reject a change hardware later
liked, and endorse one hardware measured as worse.

The fix: ``run_profile()`` takes ramp rate, segment timing and start
temperature as EXPLICIT arguments -- there is no default and no fallback
constant anywhere in this module. ``segs_from_capture()`` /
``run_profile_from_capture()`` derive all three directly from a capture's
own logged segment boundaries (``elapsed_s``/``target_c`` at each
``segment_index``/``dwelling`` change), so the sim is always driven by
exactly the profile hardware ran. ``tests/test_plant_sim.py::
test_regression_reproduces_after_capture`` pins this: it fails hard if
``run_profile``/``segs_from_capture`` regress to a hardcoded rate.

TWO KNOBS reproduce the *known* control differences between the five
firmware builds (see ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 2
and 4), so one plant identification has to explain all five runs rather
than being re-fit per capture:

  climb_mode='uncoupled'   baseline's defect: each zone computes its climb
                            term as though it heated alone
                            (tau*rate/K_dc, no coupling matrix) --
                            documented as "a roughly tenfold over-drive on
                            zone 0."
  climb_mode='coupled'     'after' onward: the real coupled Gaussian solve
                            (K_full^-1), same math the firmware ships.
  integral_floor='ff_u'    ifix build: integral floors at -ff_u (the WHOLE
                            feedforward, climb included) -- the
                            rejected/superseded approach.
  integral_floor='ff_hold' holdfix/final: floors at -ff_hold (hold
                            component only) -- the shipped fix.

TRUST -- what this simulator can and cannot be used for. Read this before
using it to argue for or against a control change; it is not a validated
oracle.

  TRUST it for: ramp-tracking magnitude and sign on any build using the
  coupled feedforward (climb_mode='coupled', i.e. 'after' onward).
  Residuals against the five captures run 0.0-1.6 C on windows where
  hardware's own run-to-run RMS is 0.7-2.8 C -- comparable to hardware's
  own noise floor. Aggregate over all 60 windows (5 captures x 2 ramp + 2
  dwell segments x 3 zones): mean|residual| = 1.00 C, RMS = 1.45 C, max =
  4.15 C (that max is the still-open baseline/uncoupled case below).

  DO NOT TRUST it for the exact saturation dynamics of the pre-coupled
  (climb_mode='uncoupled') baseline defect: the sim's plain per-zone
  formula gets the *direction* and *rough scale* of the "tenfold
  over-drive" right (it overshoots seg0's ramp by 2.5-4.2 C, its largest
  residuals in the whole calibration) but not the real bug's precise
  saturation curve. Treat 'uncoupled' results as qualitative only.

  DO NOT TRUST zone 2's second-dwell number in isolation on any coupled
  build: it runs 1.8-2.6 C cold vs hardware on every one of 'after',
  'ifix', 'holdfix_clean' and 'final', specifically on seg1's dwell,
  zone 2, and nowhere else this consistently. This is NOT a simulator
  defect -- it is the sim faithfully reproducing a known, still-open
  plant-identification error: PID_EXPANSION_PLAN.md sec 3.2 documents
  zone 2's coupled solve wanting 0.861 hold duty where the kiln actually
  needs under 0.74, i.e. the identified K_dc for zone 2 is too low. Read a
  zone-2 dwell result from this sim as "probably 1-2 C optimistic or
  pessimistic depending on sign" until sec 3.2's plant re-identification
  lands and this module is recalibrated against a fresh capture.

  DO NOT TRUST absolute values below the ~1 C residual noise floor above.
  The identified plant (K/tau/L below) came from a single 0-80 C bench-rig
  dataset; that identification, not this module's mechanics, is the
  limiting factor on precision.

  Bottom line: good for mechanism-level "will this help or hurt, roughly
  how much" comparisons on coupled-ff builds. Not good enough to sign off
  a specific gain change to the third decimal place, or to replace a
  firing for zone 2's dwell behavior specifically.

PWM window quantization (``heater_output.c``) IS modeled (added
2026-09-03g, see ``run_profile``'s ``pwm_window_ms``/``pwm_min_on_ms``/
``pwm_min_off_ms`` and ``_pwm_render``/``HEATER_DEFAULT_WINDOW_MS`` below).
Superseded reasoning: this was previously left unmodeled on the theory that
it "averages out under 10 s sampling and is not implicated by any of the
five captures' error shape" -- true for TRACKING error against those five
captures (default OFF in ``run_profile`` keeps them byte-identical), but
irrelevant to what the window actually damages, which is the RELAY, not
the temperature curve. PID_EXPANSION_PLAN.md sec 3.4's 2026-09-03f addendum
found the gain search's real defect was exactly this gap: an objective
scored purely on tracking error cannot penalize actuator chatter, so it
answers "more gain, always" regardless of noise. Default OFF in
``run_profile`` itself (``pwm_window_ms=0.0``); default ON (at firmware's
own 60 s window) in the gain-search entry points, same convention as the
noise/quantization model below.

Measurement noise/quantization IS modeled (added 2026-09-03, see
``run_profile``'s ``measurement_quantum_c``/``measurement_noise_std_c``/
``measurement_seed`` and the ``MAX31856_QUANTUM_C``/
``MEASURED_THERMO_NOISE_STD_C`` constants below) -- both default OFF
(``0.0``) in ``run_profile`` itself so the five-capture regression fixtures
stay byte-identical, but default ON in ``per_zone_gain_grid_search``/
``per_zone_gain_holdout_report`` (the gain-search entry points), because a
gain search run without them cannot see the ringing an ever-larger
kp/kd combination would cause on the real noisy sensor and pins its
"optimum" at whatever grid edge it is given (PID_EXPANSION_PLAN.md sec
3.4's 2026-09-03 addendum). ``MAX31856_QUANTUM_C`` is derived from
``firmware/KilnFW/App/drivers/hw/max31856_codec.h``, not assumed;
``MEASURED_THERMO_NOISE_STD_C`` is measured directly off rested/steady
dwell windows in the ``logs/coupling/cpl_z{0,1,2}_thermo.jsonl`` excitation
captures already used to identify K_full/tau above (see that constant's
own comment for the exact sample windows and method) -- both are cited
rather than guessed, per this module's five captures already being
hardware-quantized and compared against at their native cadence, never
resampled or smoothed to hide quantization on either side (see
``tests/fixtures/plant_sim/README.md`` and
``tests/test_plant_sim.py``'s module docstring for how the regression test
keeps that honest).

See ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` for the control-side
background this module reproduces, and ``log_analysis.py`` (whose
``parse_profile_exec_jsonl``/``split_runs``/``build_windows``/
``window_zone_stats`` this module reuses rather than re-implementing) for
the hardware-side windowing this simulator is compared against.
"""
from __future__ import annotations

import argparse
import dataclasses
import math
from typing import Optional, Sequence

import numpy as np

from . import log_analysis

# ---------------------------------------------------------------------------
# Identified plant (0-80 C bench rig, single dataset -- see module docstring's
# TRUST section: this identification, not the simulator mechanics, is now the
# limiting factor on absolute precision).
# ---------------------------------------------------------------------------

# Re-identified 2026-09-02 from three rested-start, fully-settled (2100 s
# dwell) single-zone excitation runs -- logs/coupling/cpl_z{0,1,2}_{mcp,thermo}.jsonl,
# parsed with coupling_pair_log.py -- and three passive cooldowns
# (logs/coupling/cooldown_z1.jsonl, cooldown_z2.jsonl,
# cooldown_after_coupid6.jsonl). See PID_EXPANSION_PLAN.md sec 3.2/3.4.
#
# K_full IS the coupling matrix in [affected][stepped] form directly (no
# wire_form/transpose step any more -- the excitation runs measure exactly
# this orientation: hold column i's duty, read every zone's settled rise).
# The prior matrix assumed much weaker/near-symmetric coupling; the
# excitation runs show it is strongly ASYMMETRIC (z1 raises z0 ~22 C, z0
# raises z1 only ~9 C -- K_full[0][1]=27.32 vs K_full[1][0]=14.30).
# Condition number 4.64 (was 5.27) -- plausibility check passes.
K_full = np.array([
    [38.13, 27.32, 21.72],
    [14.30, 35.90, 22.15],
    [ 8.33, 12.42, 35.32],
])
K_diag = np.diag(K_full).copy()

# tau from the three passive cooldowns (single clean decay window per zone
# -- rough, not calibration-grade, but the only tau evidence that postdates
# the original bench-rig identification). Materially longer than the old
# bench-rig tau (263.8/269.8/270.9): the bench rig likely under-measured
# tau, or heating/cooling tau genuinely differ on this plant -- unresolved,
# flagged in PID_EXPANSION_PLAN.md sec 3.4.
tau = np.array([469.0, 455.0, 345.0])

# Dead time: no new dead-time data postdates the original bench-rig
# identification (the relay run gives Tu=334.3s, an oscillation period, not
# L directly -- deriving L from it needs a model-specific relation not yet
# built). Left at the bench-rig value.
L = np.array([52.8, 43.5, 33.9])

_K_INV = np.linalg.inv(K_full)

# ---------------------------------------------------------------------------
# Per-path dead time / tau (added 2026-09-03) -- two findings the same night
# named this as the same missing piece: the load estimator's leading suspect
# (PID_EXPANSION_PLAN.md sec 3.8, negative R^2 as low as -11.6 on real
# captures against <1% error in simulation) and the zone-2 hardware A/B
# mechanism (sec 3.2's 2026-09-03 entry) both trace to ONE dead time/tau per
# RECEIVING zone being applied to every column of that zone's coupling row,
# own-zone and cross-zone alike. Sec 2 measured the two paths are NOT the
# same: diagonal (own-zone) dead time 34-53 s / tau 264-271 s vs off-diagonal
# (cross-zone) dead time 135-158 s / tau 620-730 s -- neighbour heat arrives
# 3-4x later than a zone's own element.
#
# L_PAIR[i][j] / TAU_PAIR[i][j]: dead time / tau on the path from stepped
# zone j to affected zone i.
#   diagonal (i==j):  MEASURED, same per-zone L/tau this module already used
#                      (bench-rig L above; tau is the recalibrated own-zone
#                      figure -- see the `tau` array's own comment).
#   off-diagonal:      ASSUMED. Sec 2 gives only an aggregate RANGE across
#                      all six cross-zone paths (135-158 s / 620-730 s), not
#                      a per-pair breakdown -- no capture in this repo
#                      isolates a single (i,j) cross-zone step response well
#                      enough to fit a per-cell number. The range midpoint
#                      (146.5 s / 675.0 s) is used uniformly for every
#                      off-diagonal cell. This is a real limitation, stated
#                      here rather than hidden behind six numbers that look
#                      more precise than the data supports.
OFFDIAG_L_S = 146.5    # ASSUMED: midpoint of sec 2's measured 135-158 s range
OFFDIAG_TAU_S = 675.0  # ASSUMED: midpoint of sec 2's measured 620-730 s range

L_PAIR = np.full((3, 3), OFFDIAG_L_S)
np.fill_diagonal(L_PAIR, L)  # MEASURED (bench-rig dead time, diagonal)

TAU_PAIR = np.full((3, 3), OFFDIAG_TAU_S)
np.fill_diagonal(TAU_PAIR, tau)  # MEASURED (cooldown tau, diagonal)

# The identified plant (K/tau/L above) comes from a 0-80 C bench rig -- see
# module docstring's TRUST section. Any run whose commanded target leaves
# that envelope is extrapolating beyond where the identification has ever
# been checked against hardware.
EXTRAPOLATION_BOUNDARY_C = 80.0


def is_extrapolation(max_target_c: float, boundary: float = EXTRAPOLATION_BOUNDARY_C) -> bool:
    return max_target_c > boundary


# ---------------------------------------------------------------------------
# High-temperature extension (cone 10 / ~1285 C), added 2026-09-02.
#
# Everything above is MEASURED, but only across roughly 0-80 C. A kiln
# firing runs to bisque (~1000 C), cone 6 (~1222 C) and cone 10 (~1285 C).
# Heat loss up there is not the same physics: conductive/convective loss is
# linear in (T - T_ambient) (what K/tau above capture), but radiative loss
# goes as T_kelvin^4 and comes to dominate. Total thermal conductance (and
# therefore both K and tau, which share the same denominator -- see
# ``loss_conductance_scale``'s docstring) is scaled by a small-signal
# Stefan-Boltzmann term above the calibration point, so the model changes
# CHARACTER above EXTRAPOLATION_BOUNDARY_C rather than extending a straight
# line indefinitely.
# ---------------------------------------------------------------------------

# T_REF_C: the excitation runs' dwell temperature -- MEASURED, this is where
# K_diag/tau above are anchored and where loss_conductance_scale() is
# defined to equal exactly 1.0 (the high-temperature extension is inert at
# and below the fitted range).
T_REF_C = 55.0

# RAD_LOSS_FRACTION_AT_REF: what share of TOTAL heat loss (conductive +
# radiative) is radiative at T_REF_C. ASSUMED -- there is no measurement of
# this split at any temperature in this dataset; every capture available
# stays low enough (<=80 C, ~330 K) that radiative loss is negligible next
# to conductive loss for a modestly emissive kiln interior, so a small
# placeholder value is used. This is the number to revisit first if real
# high-temperature (bisque+) thermocouple data ever becomes available --
# not K_diag/tau/L, which stay MEASURED regardless.
RAD_LOSS_FRACTION_AT_REF = 0.05

_T_REF_K = T_REF_C + 273.15


def loss_conductance_scale(temp_c) -> float:
    """Total thermal conductance at ``temp_c`` relative to the conductance
    at ``T_REF_C``, i.e. ``C_total(T)/C_total(T_ref)``.

    Physical basis: at steady state ``K = P_max/C_total`` and
    ``tau = C_thermal/C_total`` share the same conductance denominator, so
    ``K/tau = P_max/C_thermal`` is INDEPENDENT of loss conductance --
    scaling both K and tau down by this same factor (see
    ``FOPDTPlant.step``) is therefore the physically consistent way to
    extend a conductance change to both parameters without inventing a
    second free constant.

    Normalized so ``loss_conductance_scale(T_REF_C) == 1.0``: MEASURED
    K/tau are used unchanged at the calibration point, and growth above
    1.0 is entirely governed by RAD_LOSS_FRACTION_AT_REF (ASSUMED).
    """
    temp_k = temp_c + 273.15
    rad_ref = RAD_LOSS_FRACTION_AT_REF
    cond_ref = 1.0 - rad_ref
    rad_now = rad_ref * (temp_k / _T_REF_K) ** 4
    return cond_ref + rad_now


# ---------------------------------------------------------------------------
# Physical high-temperature model (added 2026-09-02b): an energy-balance
# model of a REAL cone-10-capable kiln, used only for band targets above
# EXTRAPOLATION_BOUNDARY_C (see plant_sim_sweep._run_one).
#
# This is a DIFFERENT physical object from the bench rig that produced
# K_full/tau/L above. Proof it must be: the rig's own identified K_diag
# (~38 C/duty at *full* power) means the rig's element, against the rig's
# own losses, cannot even reach 40 C above ambient at full duty -- no loss
# model, however generous, turns that identification into a plant that
# reaches 1285 C without an element rated far outside anything called a
# "bench rig." A kiln that genuinely fires to cone 10 is bigger, more
# powerful and much better insulated than the rig; this section models
# THAT object from first-principles quantities a kiln owner can look up
# (element wattage, wall thickness/k-value, chamber area, thermal mass),
# entirely separate from the rig fit. See PID_EXPANSION_PLAN.md sec 3.4/3.7.
#
# Every constant below is ASSUMED -- typical small/medium electric
# ceramics-kiln values, not measured on any hardware in this repo. The
# MEASURED-derived exception is PHYS_COUPLING_FRAC, which reuses the
# bench rig's identified K_full ratios (see its docstring below).
# ---------------------------------------------------------------------------

_SIGMA = 5.670e-8  # Stefan-Boltzmann constant, W/(m^2 K^4). Physical constant, not a fit.

# Per-zone element rating, W. ASSUMED -- typical single-phase small/medium
# kiln zone (~7.5 kW total across 3 zones), in the range of common
# home/studio electric kilns.
PHYS_P_MAX_W = np.array([2500.0, 2500.0, 2500.0])

# Chamber geometry, ASSUMED: an interior box roughly 0.45 x 0.45 x 0.6 m
# tall (a compact multi-zone kiln), wall area split evenly across 3 zones.
PHYS_ZONE_WALL_AREA_M2 = np.array([0.495, 0.495, 0.495])

# Insulating firebrick (IFB) wall, ASSUMED thickness/conductivity typical of
# a cone-10-rated kiln: thick enough (4.5", common for a high-fire kiln) to
# keep the outer shell touchably cool at cone 10 without exceeding
# PHYS_P_MAX_W -- see the plausibility check in
# firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.4/3.7.
PHYS_WALL_THICKNESS_M = 0.1143
PHYS_WALL_K_W_PER_MK = 0.15
PHYS_WALL_R_K_PER_W = PHYS_WALL_THICKNESS_M / (PHYS_WALL_K_W_PER_MK * PHYS_ZONE_WALL_AREA_M2)

# Outer shell loss: natural convection (ASSUMED h, typical still-air
# vertical-wall value) plus radiation (ASSUMED emissivity, an
# oxidized-steel/refractory-faced shell). The T^4 term acts HERE, on the
# outer shell's own temperature -- not on the (e.g.) 1285 C chamber air
# directly, which is the physically correct place for radiative loss to
# matter on an insulated kiln: the chamber wall conducts heat outward, and
# only the (much cooler) OUTER surface radiates/convects to the room. A
# model that instead radiated straight off the chamber air at 1285 C would
# overstate loss by two-plus orders of magnitude and could never be
# physically real for any plausible element.
PHYS_OUTER_H_W_PER_M2K = 8.0
PHYS_OUTER_EMISSIVITY = 0.85

# Thermal mass per zone, ASSUMED: IFB wall mass (from the geometry above,
# typical firebrick density/specific heat) plus a modest allowance for
# shelves/ware sensible heat.
PHYS_IFB_DENSITY_KG_M3 = 750.0
PHYS_IFB_SPECIFIC_HEAT_J_KGK = 950.0
_wall_mass_kg = PHYS_ZONE_WALL_AREA_M2 * PHYS_WALL_THICKNESS_M * PHYS_IFB_DENSITY_KG_M3
PHYS_THERMAL_MASS_J_PER_K = _wall_mass_kg * PHYS_IFB_SPECIFIC_HEAT_J_KGK + 10000.0  # +shelves/ware, ASSUMED

# Cross-zone coupling ratios, MEASURED-derived: normalized off-diagonal
# shares of the identified K_full, i.e. "what fraction of zone j's own
# effect on itself also shows up in zone i" at the bench rig's low
# temperature. Applied in the physical model as a share of the NEIGHBOR
# ZONE'S ELECTRICAL POWER (not its temperature rise -- see PhysicalKilnPlant
# .step), and grown with temperature by coupling_growth() below. This is
# ASSUMED to carry over from the (differently-sized) rig to a real kiln
# only as a dimensionless SHAPE (asymmetry pattern), never as an absolute
# gain -- absolute coupling gain is set by PHYS_P_MAX_W/PHYS_THERMAL_MASS_J_PER_K
# instead.
_PHYS_COUPLING_FRAC_RAW = K_full / K_diag.reshape(1, -1)  # [i][j] = K_full[i][j] / K_full[j][j]
np.fill_diagonal(_PHYS_COUPLING_FRAC_RAW, 0.0)

# Zone-separation damping, ASSUMED. The raw ratio above, applied
# unmodified, sums to >1.0 on two of three rows (a zone can receive more
# power from its two neighbors combined than its OWN element delivers) --
# physically plausible on the compact bench rig (zones close together,
# thin dividers) but not carried over to a real kiln, which has more
# physical separation and thicker internal refractory between zones. A
# conservative round-number damping keeps the SHAPE (asymmetry pattern)
# from the bench identification while bounding the physical model's total
# cross-zone power to a fraction of a zone's own controllable budget, so
# no combination of neighbor duty can inject more power than the zone's
# own PID has authority to counteract by driving its own duty toward zero.
# Proof this matters: undamped + uncapped growth (see COUPLING_GROWTH_CAP)
# ran away past MAX_PLAUSIBLE_TEMP_C on every high-temperature band with
# both zones 0/1 duty pinned at 0 and still climbing -- caught by running
# the sweep, not by a unit test (see the mutation-testing note in
# PID_EXPANSION_PLAN.md sec 3.4/3.7).
PHYS_COUPLING_SEPARATION_DAMPING = 0.25
PHYS_COUPLING_FRAC = _PHYS_COUPLING_FRAC_RAW * PHYS_COUPLING_SEPARATION_DAMPING


def solve_outer_wall_temp_c(t_in_c, ambient_c, r_wall, h, eps, area, iters=8):
    """Quasi-static outer-shell temperature: Newton-solves
    ``(T_in - T_out)/r_wall == h*A*(T_out-T_amb) + eps*A*sigma*(T_out_K^4 -
    T_amb_K^4)`` -- conduction in equals convection+radiation out at the
    shell. Quasi-static (no separate shell thermal-mass state) is a
    standard simplification here: the shell's own mass/time-constant is
    small next to the chamber's, so it is assumed to track the chamber's
    slower dynamics rather than being integrated as a 4th state per zone.
    """
    t_in_c = np.asarray(t_in_c, dtype=float)
    ambient_c = float(ambient_c)
    t_out = ambient_c + (t_in_c - ambient_c) * 0.1
    amb_k = ambient_c + 273.15
    for _ in range(iters):
        t_out_k = t_out + 273.15
        f = ((t_in_c - t_out) / r_wall
             - h * area * (t_out - ambient_c)
             - eps * area * _SIGMA * (t_out_k ** 4 - amb_k ** 4))
        dfdt = -1.0 / r_wall - h * area - 4.0 * eps * area * _SIGMA * t_out_k ** 3
        t_out = t_out - f / dfdt
    return t_out


def physical_loss_w(t_in_c, ambient_c=20.0):
    """Steady conductive loss (W) from chamber to outer shell, per zone --
    equal at steady state to the outer shell's own convection+radiation
    loss to ambient (energy conservation through the wall, see
    ``solve_outer_wall_temp_c``)."""
    t_out = solve_outer_wall_temp_c(t_in_c, ambient_c, PHYS_WALL_R_K_PER_W,
                                     PHYS_OUTER_H_W_PER_M2K, PHYS_OUTER_EMISSIVITY,
                                     PHYS_ZONE_WALL_AREA_M2)
    return (np.asarray(t_in_c, dtype=float) - t_out) / PHYS_WALL_R_K_PER_W


# Cap on coupling_growth() below. ASSUMED: unbounded reuse of
# loss_conductance_scale() (fit to grow without limit against the rig's
# T_REF_C=55 C anchor) makes coupling power a positive-feedback runaway at
# firing temperature -- caught by mutation-testing this module (see
# PID_EXPANSION_PLAN.md sec 3.4/3.7). Physically, radiative exchange
# between two graybody surfaces is bounded by each surface's own emissive
# budget (~min(A_i,A_j)*eps*sigma*(T_i^4-T_j^4)), not by the zone's OWN
# element power -- it cannot grow indefinitely relative to a zone's own
# loss. A conservative round-number cap (coupling never exceeds 2x its
# bench-identified low-temperature share) keeps the model bounded pending
# real high-temperature multi-zone data to fit this properly.
COUPLING_GROWTH_CAP = 2.0


def coupling_growth(temp_c):
    """How much cross-zone coupling grows above its bench-identified,
    low-temperature share, CAPPED at ``COUPLING_GROWTH_CAP`` (see its
    docstring for why the cap exists). Below the cap, ASSUMED to track the
    same curve as a zone's own radiative-loss-share growth
    (``loss_conductance_scale``, reused rather than inventing a second free
    curve): inter-zone transfer at high temperature is also increasingly
    radiative (direct radiant view factor between adjacent hot
    zones/elements), the same physical mechanism that drives
    ``loss_conductance_scale``'s growth. Not independently measured at any
    temperature -- flag this as the next thing to revisit if real
    high-temperature, multi-zone data ever exists (see
    PID_EXPANSION_PLAN.md sec 3.4/3.7)."""
    return np.minimum(loss_conductance_scale(temp_c), COUPLING_GROWTH_CAP)


class PhysicalKilnPlant:
    """Energy-balance simulation of a REAL cone-10-capable kiln (ASSUMED
    parameters, ``PHYS_*`` above) -- distinct from ``FOPDTPlant``'s
    measured bench-rig identification. Used only for band targets above
    ``EXTRAPOLATION_BOUNDARY_C`` (see ``plant_sim_sweep._run_one`` and
    ``run_profile``'s ``plant_regime`` argument). Dead time is not modeled
    here (ASSUMED negligible against the multi-hour timescale of a
    high-temperature firing -- L is a few tens of seconds, the ramps below
    run for hours)."""

    # Numerical safety valve, not a physical claim: no glaze on earth fires
    # this high, so a run that reaches it has already left plausibility and
    # should be read as "this duty/target combination is not achievable,"
    # not as a temperature prediction. Exists to keep the Newton solve in
    # solve_outer_wall_temp_c() from diverging on a transient that would
    # otherwise feed back into COUPLING_GROWTH_CAP's own inputs.
    MAX_PLAUSIBLE_TEMP_C = 2500.0

    def __init__(self, dt, ambient=20.0, start_temp=None):
        self.dt = dt
        self.ambient = ambient
        self.n = N_ZONES
        self.temp = np.full(self.n, ambient) if start_temp is None else np.array(start_temp, dtype=float)
        # Instance attributes, not bare module-global reads inside step(),
        # specifically so a subclass (e.g. load_mass_sweep.LoadedPhysicalKilnPlant)
        # can override the per-instance values in its own __init__ and reuse
        # this step() unchanged instead of hand-copying it -- see
        # load_mass_sweep.py's module docstring sec 3 / finding 6 tripwire.
        self._p_max_w = PHYS_P_MAX_W
        self._coupling_frac = PHYS_COUPLING_FRAC
        self._thermal_mass = PHYS_THERMAL_MASS_J_PER_K

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        own_power = self._p_max_w * duty
        growth = coupling_growth(self.temp)
        coupling_power = (self._coupling_frac * own_power.reshape(1, -1)).sum(axis=1) * growth
        loss = physical_loss_w(self.temp, self.ambient)
        net_w = own_power + coupling_power - loss
        dTdt = net_w / self._thermal_mass
        self.temp = np.clip(self.temp + dTdt * self.dt, self.ambient, self.MAX_PLAUSIBLE_TEMP_C)
        return self.temp.copy()


# ---------------------------------------------------------------------------
# Bench-rig physical model (added 2026-09-02c). PhysicalKilnPlant above is
# NOT a validation of anything -- its own docstring says so -- because it is
# a deliberately DIFFERENT, larger object than the bench rig, parameterized
# entirely by ASSUMED typical-kiln quantities. It has never been checked
# against a single real measurement. This section anchors an energy-balance
# model to the ACTUAL bench rig instead: same physical structure as
# PhysicalKilnPlant (own-zone power in, conductive loss out, cross-zone
# coupling as a fraction of the neighbor's own power), but every constant
# below is either MEASURED (K_full/tau, same numbers used by FOPDTPlant) or
# DERIVED from those measurements -- nothing here is a typical/looked-up
# value. See ``bench_validation_report()`` and
# ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.4 for what this can
# and cannot be read as proving.
#
# The one thing that CANNOT be derived from data in this repo: absolute
# element wattage. Steady state gives one equation per zone (K_diag =
# P_max/G_loss) and cooldown gives a second (tau = C_thermal/G_loss) -- two
# equations, three unknowns (P_max, G_loss, C_thermal) per zone. A third,
# independent real-units measurement (a wattmeter reading, element
# resistance/supply voltage) does not exist anywhere in this repo's logs or
# docs. Closing that gap needs exactly ONE assumption, made explicit here:
# ---------------------------------------------------------------------------

# RIG_P_MAX: per-zone element power, in an ARBITRARY normalized unit where
# 1.0 == one zone's own full-duty element output. ASSUMED equal across all
# three zones -- not an arbitrary convenience: the three zones share an
# identical relay/heater circuit (`Relay1`..`Relay4` off Q1/Q2/Q3/Q5 in
# firmware/KilnFW/docs/HARDWARE.md, the same schematic block repeated per
# zone), and no per-zone wattage figure exists anywhere in this repo to
# derive a non-uniform split from instead. Every RIG_* constant below is
# expressed in this normalized unit rather than inventing a Watts number
# nothing here supports; see ``bench_validation_report()``'s docstring for
# which downstream comparisons this assumption actually touches (only
# cross-zone coupling -- own-zone dynamics are provably independent of it).
RIG_P_MAX = np.ones(3)

# RIG_G_LOSS: per-zone loss conductance, DERIVED from MEASURED K_diag and
# RIG_P_MAX above (steady state: P_max_i = G_loss_i * K_diag_i at duty=1,
# so G_loss_i = P_max_i / K_diag_i). No assumption beyond RIG_P_MAX.
RIG_G_LOSS = RIG_P_MAX / K_diag

# RIG_C_THERMAL: per-zone thermal mass, DERIVED from MEASURED tau and
# RIG_G_LOSS (tau_i = C_i / G_loss_i, so C_i = tau_i * G_loss_i). Provably
# independent of the RIG_P_MAX split: expand BenchKilnPlant's own-zone
# terms (coupling=0, single zone) and G_loss cancels --
# dT/dt = (K_diag*duty - (T-Tamb)) / tau exactly, the same equation
# FOPDTPlant already uses. So every OWN-ZONE prediction below (DC gain,
# cooldown tau, single-zone hold duty) is unaffected by the RIG_P_MAX
# assumption and reproduces the fitted K_diag/tau BY CONSTRUCTION -- it is
# not independent evidence, see bench_validation_report()'s docstring.
# Only the CROSS-ZONE coupling terms below actually exercise RIG_P_MAX.
RIG_C_THERMAL = tau * RIG_G_LOSS

# RIG_COUPLING_FRAC: cross-zone coupling as a fraction of the STEPPED
# zone's own power, MEASURED directly from K_full (no damping applied,
# unlike PHYS_COUPLING_FRAC above -- PHYS_COUPLING_SEPARATION_DAMPING
# exists specifically because PhysicalKilnPlant models a LARGER kiln with
# more physical separation between zones than this compact rig; testing
# the rig against its own measurements uses the measured ratio undamped).
_RIG_COUPLING_FRAC_RAW = K_full / K_diag.reshape(1, -1)
np.fill_diagonal(_RIG_COUPLING_FRAC_RAW, 0.0)
RIG_COUPLING_FRAC = _RIG_COUPLING_FRAC_RAW


class BenchKilnPlant:
    """Energy-balance reproduction of the ACTUAL bench rig (own-zone power
    in, conductive loss out, cross-zone coupling as a fraction of the
    neighbor's own power -- the same structural form as PhysicalKilnPlant),
    parameterized ONLY by RIG_* constants above (measured K_full/tau, plus
    the single RIG_P_MAX="equal per zone" assumption). No dead time, no
    radiative-loss extension: both are irrelevant at the rig's own ~20-80 C
    operating range (dead time is a few tens of seconds against
    multi-hundred-second tau; loss_conductance_scale()'s own T_REF_C=55 C
    anchor makes the radiative term negligible in exactly this range by
    construction) and adding either would only reintroduce an ASSUMED
    constant (RAD_LOSS_FRACTION_AT_REF) into what is supposed to be the
    fully-measured half of this module.

    This class exists to answer one question honestly:
    ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.4 previously
    described the physical model as reducing "to measured bench behaviour
    -- trivial and exact" below EXTRAPOLATION_BOUNDARY_C. That claim was
    false: PhysicalKilnPlant is a DIFFERENT object (ASSUMED cone-10-kiln
    constants) that is never even instantiated below the boundary --
    ``run_profile``'s ``plant_regime='physical'`` path is only reachable
    above it. This class is the first physical (as opposed to bench-rig
    FOPDT-identified) model actually run against the rig's own
    measurements. See ``bench_validation_report()`` for the result and
    ``PID_EXPANSION_PLAN.md`` sec 3.4 for the plain verdict."""

    def __init__(self, dt, ambient=20.0, start_temp=None):
        self.dt = dt
        self.ambient = ambient
        self.n = 3
        self.temp = np.full(self.n, ambient) if start_temp is None else np.array(start_temp, dtype=float)

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        own_power = RIG_P_MAX * duty
        coupling_power = (RIG_COUPLING_FRAC * own_power.reshape(1, -1)).sum(axis=1)
        loss = RIG_G_LOSS * (self.temp - self.ambient)
        net = own_power + coupling_power - loss
        dTdt = net / RIG_C_THERMAL
        self.temp = self.temp + dTdt * self.dt
        return self.temp.copy()


# Ground truth for bench_validation_report(): the three single-zone
# excitation runs' own settle instants, read directly off
# logs/coupling/cpl_z{0,1,2}_{mcp,thermo}.jsonl via
# coupled_ident.single_zone_column_observations_from_pair() (reproduced
# 2026-09-02, see that function's own settle-criterion gate). Column 0 is
# each run's own excited-zone duty; RISE_MEASURED_C[i][j] is the rise (C
# above THAT run's own recorded ambient) zone j showed while zone i alone
# was excited -- RISE_MEASURED_C[i][i] is the excited zone's own settle,
# RISE_MEASURED_C[i][j] (i != j) is the asymmetric peer rise this section
# exists to check. These are the exact numbers K_full/K_diag above were
# fitted from (via coupled_ident.py's matrix_from_single_zone_columns /
# solve_coupled) -- NOT an independent dataset; see
# bench_validation_report()'s docstring for what that does and does not
# mean for the comparisons below.
RIG_EXCITED_DUTY = np.array([0.64, 0.78, 0.79])
RIG_RISE_MEASURED_C = np.array([
    [24.40, 9.15, 5.33],
    [21.31, 27.99, 9.69],
    [17.16, 17.50, 27.90],
])

# Measured cooldown taus (PID_EXPANSION_PLAN.md sec 3.2, logs/coupling/
# cooldown_*.jsonl) -- the same numbers RIG_C_THERMAL/tau above are built
# from directly.
RIG_TAU_MEASURED_S = tau.copy()

DT = 1.0
N_ZONES = 3


def bench_plant_steady_state(duty, ambient=20.0, dt=DT, max_steps=200_000, tol=1e-9):
    """Runs ``BenchKilnPlant`` forward from ``ambient`` at a fixed duty
    vector until it stops moving (or ``max_steps`` is hit), returning the
    settled rise (T - ambient) per zone. Deliberately uses the same
    ``BenchKilnPlant.step`` code path a caller driving the model would
    actually use, rather than solving the linear steady state
    analytically, so what gets checked is the class's real behaviour."""
    plant = BenchKilnPlant(dt, ambient=ambient)
    duty = np.asarray(duty, dtype=float)
    prev = plant.temp.copy()
    for _ in range(max_steps):
        plant.step(duty)
        if np.max(np.abs(plant.temp - prev)) < tol:
            break
        prev = plant.temp.copy()
    return plant.temp - ambient


def bench_validation_report() -> dict:
    """Runs BenchKilnPlant against every one of the four comparisons the
    owner asked for, and is explicit about which are and are not
    independent evidence.

    DC gain, cooldown tau and each zone's OWN settle duty (the diagonal of
    RIG_RISE_MEASURED_C) are reproduced EXACTLY (0.00 C / 0.00 s error) --
    but that is BY CONSTRUCTION, not a finding: RIG_G_LOSS and
    RIG_C_THERMAL above are algebraically solved from these exact numbers
    (see their docstrings), and BenchKilnPlant's own-zone dynamics reduce
    to FOPDTPlant's own equation once G_loss cancels out. Reporting them
    is useful as a sanity check that the derivation was done correctly,
    not as validation.

    The one comparison that is NOT circular: the off-diagonal (cross-zone)
    entries of RIG_RISE_MEASURED_C. Nothing in RIG_C_THERMAL/RIG_G_LOSS
    forces the coupling-as-power-fraction mechanism (RIG_COUPLING_FRAC,
    injected via BenchKilnPlant.step) to reproduce the measured asymmetric
    peer rises (z1 exciting z0 by ~22 C, z0 exciting z1 by only ~9 C) --
    that asymmetry could easily have come out wrong, or symmetric, or the
    wrong sign, depending on how the model routes power between zones.
    This is the genuine test; see the returned ``off_diag_errors_c`` /
    ``off_diag_rms_c`` and PID_EXPANSION_PLAN.md sec 3.4 for the plain
    verdict on the result.
    """
    predicted = np.zeros((3, 3))
    for i in range(3):
        duty = np.zeros(3)
        duty[i] = RIG_EXCITED_DUTY[i]
        predicted[i, :] = bench_plant_steady_state(duty)

    errors = predicted - RIG_RISE_MEASURED_C
    diag_mask = np.eye(3, dtype=bool)
    off_diag_errors = errors[~diag_mask]

    # DC gain check: BenchKilnPlant's own predicted diagonal rise, divided
    # by the same excited duty, vs the measured K_diag it was derived from.
    predicted_k_diag = np.diag(predicted) / RIG_EXCITED_DUTY
    k_diag_errors = predicted_k_diag - K_diag

    # Cooldown tau check: BenchKilnPlant's own decay from a hot start with
    # zero duty, fit the same simple way coupled_ident.fit_cooldown_tau
    # does (single-exponential least-squares against ln(T - T_inf)), vs
    # RIG_TAU_MEASURED_S (which RIG_C_THERMAL was built from).
    predicted_tau = np.zeros(3)
    for i in range(3):
        plant = BenchKilnPlant(DT, ambient=20.0, start_temp=[20.0, 20.0, 20.0])
        plant.temp[i] = 20.0 + RIG_RISE_MEASURED_C[i, i]
        temps = [plant.temp[i]]
        for _ in range(int(5 * RIG_TAU_MEASURED_S[i])):
            plant.step(np.zeros(3))
            temps.append(plant.temp[i])
        temps = np.array(temps)
        t = np.arange(len(temps), dtype=float) * DT
        y = temps - 20.0
        y = np.clip(y, 1e-6, None)
        # ln(y) = ln(y0) - t/tau -- linear least squares for 1/tau.
        slope, _ = np.polyfit(t, np.log(y), 1)
        predicted_tau[i] = -1.0 / slope

    return dict(
        predicted_rise_c=predicted,
        measured_rise_c=RIG_RISE_MEASURED_C.copy(),
        errors_c=errors,
        off_diag_errors_c=off_diag_errors,
        off_diag_rms_c=float(np.sqrt((off_diag_errors ** 2).mean())),
        off_diag_max_abs_c=float(np.abs(off_diag_errors).max()),
        diag_errors_c=np.diag(errors),
        predicted_k_diag=predicted_k_diag,
        measured_k_diag=K_diag.copy(),
        k_diag_errors=k_diag_errors,
        predicted_tau_s=predicted_tau,
        measured_tau_s=RIG_TAU_MEASURED_S.copy(),
        tau_errors_s=predicted_tau - RIG_TAU_MEASURED_S,
        excited_duty=RIG_EXCITED_DUTY.copy(),
    )


def format_bench_validation_report_text(report: dict) -> str:
    lines = ["=== Bench-rig physical model (BenchKilnPlant) vs measurement ==="]
    lines.append("")
    lines.append("1. DC gain (diagonal, BY CONSTRUCTION -- see docstring):")
    for i in range(3):
        lines.append(
            f"   z{i}: predicted={report['predicted_k_diag'][i]:.3f} "
            f"measured={report['measured_k_diag'][i]:.3f} "
            f"err={report['k_diag_errors'][i]:+.4f} C/duty"
        )
    lines.append("")
    lines.append("2. Cooldown tau (BY CONSTRUCTION -- see docstring):")
    for i in range(3):
        lines.append(
            f"   z{i}: predicted={report['predicted_tau_s'][i]:.1f}s "
            f"measured={report['measured_tau_s'][i]:.1f}s "
            f"err={report['tau_errors_s'][i]:+.2f} s"
        )
    lines.append("")
    lines.append("3. Own-zone settled hold rise at excited duty (BY CONSTRUCTION -- see docstring):")
    for i in range(3):
        lines.append(
            f"   z{i}: duty={report['excited_duty'][i]:.2f} "
            f"predicted={report['predicted_rise_c'][i, i]:.2f}C "
            f"measured={report['measured_rise_c'][i, i]:.2f}C "
            f"err={report['diag_errors_c'][i]:+.3f} C"
        )
    lines.append("")
    lines.append("4. Cross-zone (asymmetric peer) rise -- the GENUINE, non-circular test:")
    for i in range(3):
        for j in range(3):
            if i == j:
                continue
            lines.append(
                f"   excite z{i} -> z{j}: predicted={report['predicted_rise_c'][i, j]:.2f}C "
                f"measured={report['measured_rise_c'][i, j]:.2f}C "
                f"err={report['errors_c'][i, j]:+.3f} C"
            )
    lines.append(
        f"   off-diagonal RMS error = {report['off_diag_rms_c']:.3f} C, "
        f"max|error| = {report['off_diag_max_abs_c']:.3f} C"
    )
    return "\n".join(lines)


class FOPDTPlant:
    """First-order-plus-dead-time plant, one instance per zone, coupled
    through ``K`` (each zone's steady state depends on every zone's duty)."""

    def __init__(self, K, tau, L, dt, ambient=20.0, start_temp=None):
        self.K, self.tau, self.L, self.dt, self.ambient = K, tau, L, dt, ambient
        self.n = len(tau)
        self.temp = np.full(self.n, ambient) if start_temp is None else np.array(start_temp, dtype=float)
        self.max_delay = int(np.max(L) / dt) + 2
        self.duty_hist = [np.zeros(self.n) for _ in range(self.max_delay)]

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        self.duty_hist.append(duty.copy())
        self.duty_hist.pop(0)
        new_temp = self.temp.copy()
        for i in range(self.n):
            delay_steps = min(int(round(self.L[i] / self.dt)), len(self.duty_hist) - 1)
            d_delayed = self.duty_hist[-1 - delay_steps]
            # High-temperature extension: total thermal conductance grows
            # with the ZONE'S OWN current temperature (radiative loss), so
            # both its gain row and its tau shrink by the same factor. The
            # controller (ff/PID, coupled_ff_hold_climb) deliberately does
            # NOT see this -- it always uses the fixed low-temperature
            # K_full/tau, exactly like the real firmware, which has no
            # temperature compensation. That mismatch is what a
            # high-temperature sweep is meant to expose.
            scale = loss_conductance_scale(self.temp[i])
            K_row_eff = self.K[i] / scale
            tau_eff = self.tau[i] / scale
            u_ss = self.ambient + np.dot(K_row_eff, d_delayed)
            dTdt = (u_ss - self.temp[i]) / tau_eff
            new_temp[i] = self.temp[i] + dTdt * self.dt
        self.temp = new_temp
        return self.temp.copy()


class FOPDTPlantPerPath:
    """Same physical structure as ``FOPDTPlant`` -- coupled first-order-plus-
    dead-time zones -- but with an INDEPENDENT dead time and time constant
    per (affected, stepped) PATH instead of one dead time/tau per receiving
    zone applied uniformly to every column of its coupling row. See
    ``L_PAIR``/``TAU_PAIR`` above for what is MEASURED (diagonal) vs ASSUMED
    (off-diagonal, a single range-midpoint value -- sec 2 never isolated a
    per-pair cross-zone step response).

    Linear superposition: each zone's rise above ambient is the SUM of one
    first-order response per source column, ``x[i,j]``, driven by column
    j's own delayed duty, with the (i,j) path's own gain/tau:

        dx_ij/dt = (K_full[i,j] * duty_j_delayed(L_PAIR[i,j]) - x_ij) / TAU_PAIR[i,j]
        T_i = ambient + sum_j x_ij

    ``FOPDTPlant`` is the single-delay-per-zone model this replaces for any
    caller that opts in (``plant_regime='measured_per_path'``); it is left
    unchanged so every existing caller/test stays on the original code path
    -- this class is purely additive.
    """

    def __init__(self, K, L_pair, tau_pair, dt, ambient=20.0, start_temp=None):
        self.K, self.L_pair, self.tau_pair, self.dt, self.ambient = K, L_pair, tau_pair, dt, ambient
        self.n = K.shape[0]
        if start_temp is None:
            self.temp = np.full(self.n, ambient)
            self.x = np.zeros((self.n, self.n))
        else:
            self.temp = np.array(start_temp, dtype=float)
            # Seed each path's steady-state share so a non-ambient start
            # (e.g. a rested-but-not-cold capture) does not have to climb
            # back through a fake transient on tick 0 -- split the starting
            # rise proportionally to each path's steady-state gain, the
            # same assumption an unknown starting duty history implies.
            rise = self.temp - ambient
            k_row_sum = self.K.sum(axis=1)
            k_row_sum_safe = np.where(k_row_sum == 0.0, 1.0, k_row_sum)
            self.x = (self.K / k_row_sum_safe.reshape(-1, 1)) * rise.reshape(-1, 1)
        self.max_delay = int(np.max(self.L_pair) / dt) + 2
        self.duty_hist = [np.zeros(self.n) for _ in range(self.max_delay)]

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        self.duty_hist.append(duty.copy())
        self.duty_hist.pop(0)
        new_x = self.x.copy()
        for i in range(self.n):
            # High-temperature extension: same conductance-scaling treatment
            # as FOPDTPlant.step, applied per path off the RECEIVING zone's
            # current temperature (the controller still never sees this --
            # see FOPDTPlant.step's own note).
            scale = loss_conductance_scale(self.temp[i])
            for j in range(self.n):
                delay_steps = min(int(round(self.L_pair[i, j] / self.dt)), len(self.duty_hist) - 1)
                d_delayed = self.duty_hist[-1 - delay_steps][j]
                k_eff = self.K[i, j] / scale
                tau_eff = self.tau_pair[i, j] / scale
                dxdt = (k_eff * d_delayed - self.x[i, j]) / tau_eff
                new_x[i, j] = self.x[i, j] + dxdt * self.dt
        self.x = new_x
        self.temp = self.ambient + self.x.sum(axis=1)
        return self.temp.copy()


# ---------------------------------------------------------------------------
# Fuzzy-PID layer. The mirror of firmware/KilnFW/App/drivers/control/pid_fuzzy.c's
# pid_fuzzy_adjust() itself now lives in ``fuzzy_band_probe`` (built and
# pinned-by-test 2026-09-04 for the offline band-probe tool) -- this module
# used to carry its OWN hand-copied line-for-line port with the bands
# hardcoded as ``FUZZY_ERROR_BAND_C``/``FUZZY_RATE_BAND_C_PER_S`` module
# constants. Commit 904db54 made those two numbers per-zone runtime config
# (``error_band_c``/``rate_band_c_per_s``, ZONES_CFG_VERSION 19) on the C
# side without updating this file's copy or its call sites, which is
# invisible today only because the config defaults happen to resolve to
# exactly 20.0/0.5 -- the moment a zone is configured with a different band
# (e.g. the 6-8 C / 0.20-0.25 envelope recommended from the 2026-09-04
# capture), every prediction this module makes silently keeps simulating
# the old fixed band. Importing the probe's implementation instead of
# maintaining a second hand copy means there is exactly one Python port of
# pid_fuzzy.c to keep in lockstep with the firmware, not two; see
# ``fuzzy_band_probe``'s own module docstring and
# ``tests/test_fuzzy_band_probe_drift.py`` for the drift check against the
# C source.
from .fuzzy_band_probe import (  # noqa: E402  (import placed here for context)
    ERROR_BAND_C_DEFAULT as FUZZY_ERROR_BAND_C_DEFAULT,
    RATE_BAND_C_PER_S_DEFAULT as FUZZY_RATE_BAND_C_PER_S_DEFAULT,
    pid_fuzzy_adjust as _fuzzy_band_probe_pid_fuzzy_adjust,
)


def pid_fuzzy_adjust(error_c, error_rate_c_per_s, base_kp, base_ki, base_kd,
                      strength_pct, error_band_c=FUZZY_ERROR_BAND_C_DEFAULT,
                      rate_band_c_per_s=FUZZY_RATE_BAND_C_PER_S_DEFAULT):
    """Thin wrapper over ``fuzzy_band_probe.pid_fuzzy_adjust`` (the single
    Python port of pid_fuzzy.c's ``pid_fuzzy_adjust()`` this repo now
    maintains) matching the C function's signature: bands are explicit
    parameters, not module constants, sourced by the caller from the zone's
    actual config -- see ``PID.__init__``/``run_profile``'s
    ``error_band_c``/``rate_band_c_per_s`` parameters below. Defaulting both
    band parameters to the firmware's documented defaults (20.0 C /
    0.5 C/s, the ``ZONE_ERROR_BAND_C_DEFAULT``/``ZONE_RATE_BAND_C_PER_S_
    DEFAULT`` sentinel-resolution values) preserves every existing caller's
    behaviour byte-for-byte."""
    # strength_pct here may be a float (the sim has no uint8_t rounding
    # step); fuzzy_band_probe.pid_fuzzy_adjust clamps to [0, 100] the same
    # way the C caller does before its uint8_t cast, so passing a float
    # through is safe and behaviourally identical.
    return _fuzzy_band_probe_pid_fuzzy_adjust(
        error_c, error_rate_c_per_s, error_band_c, rate_band_c_per_s,
        base_kp, base_ki, base_kd, strength_pct,
    )


def _pid_rescale_integral_for_new_ki(integral, old_ki, new_ki):
    """Mirrors pid.c's pid_rescale_integral_for_new_ki(): bump-transfer the
    raw integral accumulator so the I-term's contribution to duty is
    unchanged by a Ki move alone (hazard 3, profile_executor_pid_tick.c)."""
    if not (old_ki > 0.0) or not (new_ki > 0.0) or old_ki == new_ki:
        return integral
    rescaled = integral * old_ki / new_ki
    bound = 100000.0
    if rescaled > bound:
        rescaled = bound
    elif rescaled < -bound:
        rescaled = -bound
    return rescaled


class PID:
    """Line-for-line match to ``pid.c``'s ``pid_update_terms()``: conditional
    integration freeze, feedforward-relative floor, P/I/D/ff clamp
    structure. See sim_calibration.md sec 1 for the line-by-line check
    against the firmware source this was built against.

    ``fuzzy_strength_pct`` (default 0, no fuzzy layer) mirrors the
    ZONE_CONTROL_MODE_PID_FUZZY wiring in profile_executor_pid_tick.c: each
    tick, BEFORE this tick's P/I/D terms are computed, error_c/error_rate
    (error_rate read from *last* tick's d_filtered, matching the firmware's
    documented one-tick lag) feed pid_fuzzy_adjust() against the zone's base
    gains, and a Ki change is bump-transferred into the integral
    accumulator via pid_rescale_integral_for_new_ki() before it is used.

    ``error_band_c``/``rate_band_c_per_s`` are the same per-zone config
    values profile_executor_pid_tick.c resolves via
    ``zones_config_get_error_band_c()``/``zones_config_get_rate_band_c_per_s()``
    before calling ``pid_fuzzy_adjust()`` (ZONES_CFG_VERSION 19, commit
    904db54). They default to the firmware's documented defaults (20.0 C /
    0.5 C/s) so a caller that never sets them behaves exactly as before
    that commit made the bands configurable; a caller simulating a
    non-default band (e.g. evaluating a tighter band before recommending it
    to the owner) MUST pass the zone's actual configured values here, not
    rely on the default."""

    def __init__(self, kp, ki, kd, d_tau, b, pid_range_c, fuzzy_strength_pct=0.0,
                 error_band_c=FUZZY_ERROR_BAND_C_DEFAULT,
                 rate_band_c_per_s=FUZZY_RATE_BAND_C_PER_S_DEFAULT):
        self.base_kp, self.base_ki, self.base_kd = kp, ki, kd
        self.kp, self.ki, self.kd = kp, ki, kd
        self.d_tau, self.b, self.pid_range_c = d_tau, b, pid_range_c
        self.fuzzy_strength_pct = fuzzy_strength_pct
        self.error_band_c = error_band_c
        self.rate_band_c_per_s = rate_band_c_per_s
        self.integral = 0.0
        self.d_filtered = 0.0
        self.prev_measurement = None
        self.initialized = False
        self.fuzzy_prev_effective_ki = 0.0

    def update(self, setpoint, measurement, dt_s, ff_u, ff_hold, integral_floor='ff_hold'):
        if not self.initialized:
            self.prev_measurement = measurement
            self.integral = 0.0
            self.d_filtered = 0.0
            self.initialized = True

        # Fuzzy gain prep -- profile_executor_pid_tick.c's
        # pid_fuzzy_prepare_gains(), run before this tick's pid_update_terms
        # equivalent below. error_rate uses self.d_filtered as it stands
        # BEFORE this tick's update (one-tick lag, matches firmware).
        error_c = setpoint - measurement
        error_rate_c_per_s = self.d_filtered
        adj_kp, adj_ki, adj_kd = pid_fuzzy_adjust(
            error_c, error_rate_c_per_s, self.base_kp, self.base_ki, self.base_kd,
            self.fuzzy_strength_pct,
            error_band_c=self.error_band_c,
            rate_band_c_per_s=self.rate_band_c_per_s,
        )
        self.integral = _pid_rescale_integral_for_new_ki(
            self.integral, self.fuzzy_prev_effective_ki, adj_ki)
        self.fuzzy_prev_effective_ki = adj_ki
        self.kp, self.ki, self.kd = adj_kp, adj_ki, adj_kd

        if dt_s <= 0:
            dt_s = 1.0
        error = setpoint - measurement
        if abs(error) > self.pid_range_c:
            self.prev_measurement = measurement
            u = 1.0 if error > 0 else 0.0
            return u, dict(p=u, i=0.0, d=0.0, ff=0.0)

        raw_d = -(measurement - self.prev_measurement) / dt_s
        alpha = dt_s / (self.d_tau + dt_s)
        self.d_filtered += alpha * (raw_d - self.d_filtered)
        self.prev_measurement = measurement

        p_term = self.kp * (self.b * setpoint - measurement)
        d_term = self.kd * self.d_filtered

        unclamped = p_term + self.ki * self.integral + d_term + ff_u
        would_push_further = (unclamped >= 1.0 and error > 0) or (unclamped <= 0.0 and error < 0)
        if not would_push_further:
            self.integral += error * dt_s

        i_term = self.ki * self.integral
        floor = -ff_u if integral_floor == 'ff_u' else -ff_hold
        if i_term < floor:
            i_term = floor
            self.integral = floor / self.ki if self.ki > 0 else 0.0
        elif i_term > 1.0:
            i_term = 1.0
            self.integral = 1.0 / self.ki if self.ki > 0 else 0.0

        u = p_term + i_term + d_term + ff_u
        u = min(max(u, 0.0), 1.0)
        return u, dict(p=p_term, i=i_term, d=d_term, ff=ff_u)


# Prior (pre-2026-09-02) coupling matrix, kept only so a sweep can compare
# "controller believes the old matrix" against "controller believes the new
# one" -- see plant_sim_sweep.py. NOT used anywhere by default.
K_full_OLD = np.array([
    [39.25, 26.61, 20.73],
    [15.78, 31.97, 21.09],
    [ 9.70, 11.38, 31.68],
])

MATRIX_VARIANTS = {"new": K_full, "old": K_full_OLD}


def coupled_ff_hold_climb(target_c, target_rate, i, ambient=20.0, K_inv=None, tau_ff=None):
    """The shipped coupled solve ('after' onward): hold and climb duty for
    zone i, solved jointly across all three zones via K_full^-1.

    ``K_inv``/``tau_ff`` let a caller (the high-temperature sweep) ask "what
    would the controller compute if it believed a different matrix" while
    the PLANT it is driving stays fixed to the measured one -- exactly what
    the real firmware does (its coupling matrix is a fixed constant, not
    temperature-adaptive). Defaults to the module's live matrix/tau."""
    K_inv = _K_INV if K_inv is None else K_inv
    tau_ff = tau if tau_ff is None else tau_ff
    rhs_hold = np.full(N_ZONES, target_c - ambient)
    hold_duty = K_inv @ rhs_hold
    rhs_climb = tau_ff * target_rate
    climb_duty = K_inv @ rhs_climb
    hold_i = hold_duty[i]
    climb_i = climb_duty[i]
    total = hold_i + climb_i
    total_clamped = min(max(total, 0.0), 1.0)
    hold_out = min(max(hold_i, 0.0), 1.0)
    climb_out = total_clamped - hold_out
    return hold_out, climb_out, total_clamped


def hold_duty_infeasible(target_c, K_inv=None, ambient=20.0):
    """True if the coupled hold solve for a uniform ``target_c`` across all
    three zones needs a duty outside [0,1] on any zone -- the "coupled hold
    solve going infeasible" failure mode PID_EXPANSION_PLAN.md secs 3.2/3.4
    describe. Used by the temperature-band sweep to find the infeasibility
    boundary for a given matrix."""
    K_inv = _K_INV if K_inv is None else K_inv
    hold_duty = K_inv @ np.full(N_ZONES, target_c - ambient)
    return bool((hold_duty < 0.0).any() or (hold_duty > 1.0).any())


def uncoupled_ff_hold_climb(target_c, target_rate, i, ambient=20.0):
    """Baseline's defect formula: each zone as though it heated alone --
    (T_sp - T_amb)/K_dc + rate*tau/K_dc, no coupling matrix at all. Gets the
    baseline build's over-drive DIRECTION and rough SCALE right, not its
    precise saturation dynamics -- see module docstring's TRUST section."""
    hold_i = (target_c - ambient) / K_diag[i]
    climb_i = target_rate * tau[i] / K_diag[i]
    total = hold_i + climb_i
    total_clamped = min(max(total, 0.0), 1.0)
    hold_out = min(max(hold_i, 0.0), 1.0)
    climb_out = total_clamped - hold_out
    return hold_out, climb_out, total_clamped


class LagCompensatedFF:
    """Candidate feedforward (2026-09-03, opt-in, SIMULATION ONLY -- not a
    firmware change, see PID_EXPANSION_PLAN.md sec 3.2/3.4). ``coupled_ff_
    hold_climb`` solves the joint system ``K_full @ hold = target - ambient``
    as though every zone's contribution to every other zone's temperature
    arrives INSTANTANEOUSLY. Physically it does not -- sec 2 measured
    cross-zone dead time 135-158 s / tau 620-730 s against 34-53 s / 264-271 s
    on the diagonal, 3-4x slower. Sec 3.2's 2026-09-03 hardware A/B mechanism
    traces z2's regression under the new matrix to exactly this: z2's row
    leans harder on neighbour credit that, per this same instantaneous
    assumption, the controller believes has already arrived.

    This candidate instead solves each zone's OWN hold duty directly from
    its own diagonal gain, crediting each neighbour's contribution using the
    DELAYED duty it actually commanded ``L_pair[i,j]`` seconds ago (0 before
    any duty has been commanded -- a neighbour that has not run yet
    contributes no credit, which is the physically correct limit):

        hold_i(t) = (target_c - ambient - sum_{j!=i} K[i,j]*duty_j(t-L[i,j])) / K[i,i]

    No matrix inversion is needed -- each zone's own duty is solved directly,
    only neighbours are looked up from history.

    Climb stays UNCOUPLED (own zone only, ``tau[i]*target_rate/K[i,i]``,
    same formula ``uncoupled_ff_hold_climb`` uses) -- deliberately not
    extended to cross-zone credit. Sec 2's identified lag is evidenced from
    settled DWELL tails (a hold/steady-state phenomenon); giving climb the
    same delayed-credit treatment would be a second, unvalidated guess
    stacked on this one, with no measurement behind it either way.

    ``duty`` fed to ``record()`` is the zones' actually-commanded duty each
    tick (post-PID, what really drove the plant) -- not the feedforward's own
    guess -- since crediting a neighbour's REAL commanded duty from the past
    is what "delayed duty" means physically; crediting its own past
    feedforward guess would compound one candidate's error into another's
    credit term.
    """

    def __init__(self, K, L_pair, dt, n_zones=N_ZONES):
        self.K, self.L_pair, self.dt, self.n = K, L_pair, dt, n_zones
        self.max_delay = int(np.max(L_pair) / dt) + 2
        self.duty_hist = [np.zeros(self.n) for _ in range(self.max_delay)]

    def record(self, duty):
        self.duty_hist.append(np.array(duty, dtype=float).copy())
        self.duty_hist.pop(0)

    def _delayed_duty(self, j, L_ij):
        delay_steps = min(int(round(L_ij / self.dt)), len(self.duty_hist) - 1)
        return self.duty_hist[-1 - delay_steps][j]

    def __call__(self, target_c, target_rate, i, ambient=20.0, tau_ff=None):
        credit = 0.0
        for j in range(self.n):
            if j == i:
                continue
            credit += self.K[i, j] * self._delayed_duty(j, self.L_pair[i, j])
        hold_i = (target_c - ambient - credit) / self.K[i, i]
        tau_diag = tau if tau_ff is None else tau_ff
        climb_i = target_rate * tau_diag[i] / self.K[i, i]
        total = hold_i + climb_i
        total_clamped = min(max(total, 0.0), 1.0)
        hold_out = min(max(hold_i, 0.0), 1.0)
        climb_out = total_clamped - hold_out
        return hold_out, climb_out, total_clamped


def _broadcast_zone_param(x, n=None):
    """Accept either a scalar (applied uniformly to every zone -- every
    caller's behaviour before per-zone gains existed) or a length-``n``
    sequence (one value per zone) for a PID gain, and return a length-``n``
    numpy array either way.

    Added for sec 3.4's per-zone gain sweep (PID_EXPANSION_PLAN.md): before
    this, ``run_profile``'s ``kp``/``ki``/``kd`` were always a single float
    applied to every zone's ``PID`` instance, so nothing in this module
    could even ask "would zone 2's own gains, tuned for zone 2, do better
    than the shared gain." A scalar still produces byte-identical output to
    the old code path -- this is purely additive.
    """
    if n is None:
        n = N_ZONES
    arr = np.atleast_1d(np.asarray(x, dtype=float))
    if arr.size == 1:
        return np.full(n, arr[0])
    if arr.size != n:
        raise ValueError(
            f"PID gain must be a scalar or exactly {n} values (one per zone), "
            f"got {arr.size}"
        )
    return arr


# ---------------------------------------------------------------------------
# PWM (time-proportioning) window model -- added 2026-09-03g, closing the
# "Not modeled" gap the module docstring and PID_EXPANSION_PLAN.md sec 3.4's
# 2026-09-03f addendum both flagged: a gain search scored on whole-run mean
# |error| alone cannot see relay chatter, so it correctly (given the
# question it was asked) answers "more gain, always." This section gives the
# duty command an actual relay to drive, faithfully mirroring
# ``firmware/KilnFW/App/drivers/control/heater_output.c``'s ``heater_output_duty_ex``
# (the non-``force_new_window`` path -- ``heater_output_duty()``, what every
# ordinary PID-driven zone actually calls; the ``_relay_step`` early-window
# variant used only by the bang-bang-mode fuzzy relay law is out of scope
# here) line for line:
#
#   - a fixed window (default ``HEATER_DEFAULT_WINDOW_MS`` = 60 s);
#   - at each window boundary, this window's on-time is
#     ``duty * window_ms``, quantized: an on-time below
#     ``max(min_on_ms, HEATER_MIN_ON_MS_FLOOR)`` renders as OFF for the
#     whole window (not rounded up), and an on-time within ``min_off_ms`` of
#     the full window renders as ON for the whole window;
#   - a RUNNING min-on hold independent of the window boundary: once the
#     relay is actually on, an off decision is deferred until
#     ``HEATER_MIN_ON_MS_FLOOR`` (10 s) of continuous on-time has
#     accumulated, even across a window boundary.
#
# ``HEATER_MIN_ON_MS_FLOOR``/``HEATER_DEFAULT_WINDOW_MS``/
# ``HEATER_DEFAULT_MIN_OFF_MS`` below are the literal values from
# ``heater_output.h`` (10000/60000/2000 ms), not re-derived.
# ---------------------------------------------------------------------------

HEATER_MIN_ON_MS_FLOOR = 10000.0
HEATER_DEFAULT_WINDOW_MS = 60000.0
HEATER_DEFAULT_MIN_OFF_MS = 2000.0

#: Rated mechanical cycle life of the on-board relays (EE2-12NUH), from
#: ``firmware/KilnFW/App/drivers/persist/relay_cycles.h`` -- the firmware module
#: that ACTUALLY performs lifetime contact-cycle accounting (persisted,
#: shown to the operator), not just a comment aside. Its own words:
#: "the EE2-12NUH relays on this board are electromechanical, with a
#: contact life budget on the order of 10^5 operations (docs/HARDWARE.md).
#: A 60 s time-proportioning window can spend that in a few hundred hours
#: of firing. 'A kiln controller that silently eats a relay's contact
#: life is a controller that fails mid-firing at cone temperature' -- so
#: the count is kept, persisted, and shown to the operator." That last
#: sentence is why this constant matters at all: the failure mode is not a
#: maintenance inconvenience, it is a controller that can go dead at cone
#: temperature and ruin the load in the kiln.
#:
#: ``heater_output.h``'s ``window_ms`` field comment separately states K1-K4
#: itself is rated 1e6 operations ("their own loaded life") -- 10x this
#: constant. Both are real firmware comments and they do not reconcile as
#: written; ``relay_cycles.h`` is used here because it is the number wired
#: into the actual persisted accounting logic and its own arithmetic
#: cross-checks ("a few hundred hours of firing" at a 10 s window: 360
#: windows/hour * 2 transitions/window = 720 transitions/hour = 360
#: cycles/hour, and 1e5 / 360 = 278 hours -- "a few hundred hours" matches;
#: 1e6 / 360 = 2,778 hours would not read as "a few hundred"). Treat this
#: as the firmware's own stated OPERATING BUDGET (with margin below the
#: manufacturer's absolute rating), not a from-scratch estimate.
RELAY_RATED_LIFE_CYCLES = 1.0e5


@dataclasses.dataclass
class _PwmZoneState:
    """Mirrors ``heater_output_state_t``'s time-proportioned fields."""
    window_started: bool = False
    window_elapsed_ms: float = 0.0
    on_ms_this_window: float = 0.0
    relay_on: bool = False
    on_elapsed_ms: float = 0.0
    cycle_count: int = 0  # transitions, matching heater_output.c's note_transition()


def _pwm_render(state: _PwmZoneState, duty: float, window_ms: float, min_on_ms: float,
                 min_off_ms: float, dt_ms: float) -> bool:
    """One tick of ``heater_output_duty_ex(..., force_new_window=False)``,
    translated line for line from ``heater_output.c``. Returns the relay
    state to actually command (and drive the plant with) this tick; mutates
    ``state`` in place, including ``cycle_count`` for actuator-cost
    accounting."""
    # heater_output_duty_ex() takes ``duty`` as a C ``float`` parameter, so
    # every caller's double already gets narrowed to float32 on entry, BEFORE
    # the clamp below. Narrow it here too so the clamp and (more importantly)
    # the on_ms product below start from the same bits the firmware does.
    duty = float(np.float32(duty))
    duty = min(max(duty, 0.0), 1.0)

    eff_min_on_ms = max(min_on_ms, HEATER_MIN_ON_MS_FLOOR)
    if eff_min_on_ms > window_ms:
        eff_min_on_ms = window_ms

    state.window_elapsed_ms += dt_ms
    if not state.window_started or state.window_elapsed_ms >= window_ms:
        state.window_started = True
        state.window_elapsed_ms = 0.0
        # heater_output.c computes this as
        # ``(uint32_t)(duty * (float)cfg->window_ms)`` -- both operands
        # float32, the product evaluated in float32 (MSVC/x64 and GCC/clang
        # on this target both keep float*float at single precision, no
        # promotion to double), THEN truncated to whole milliseconds.
        # Matching only the truncation and not the precision is itself a
        # divergence: computing ``duty * window_ms`` as a Python (C double)
        # product before truncating can land on a different integer than the
        # float32 product does, right at the kind of near-integer threshold
        # this function's callers care about (e.g. duty=0.5000166666...,
        # window_ms=60000 -> float32 product truncates to 30001, double
        # product truncates to 30000 -- a whole tick's difference in the
        # on/off decision, confirmed against the harness). So narrow
        # window_ms to float32 too and do the multiply in float32.
        on_ms = float(int(np.float32(duty) * np.float32(window_ms)))
        if on_ms < eff_min_on_ms:
            on_ms = 0.0
        elif window_ms - on_ms < min_off_ms:
            on_ms = window_ms
        state.on_ms_this_window = on_ms

    want_on = state.window_elapsed_ms < state.on_ms_this_window

    if state.relay_on:
        state.on_elapsed_ms += dt_ms
        if not want_on and state.on_elapsed_ms < HEATER_MIN_ON_MS_FLOOR:
            want_on = True
    elif want_on:
        state.on_elapsed_ms = 0.0

    if want_on != state.relay_on:
        state.cycle_count += 1
    state.relay_on = want_on
    return want_on


def run_profile(segs, start_temp, kp=0.06, ki=0.0003, kd=0.0,
                 climb_mode='coupled', integral_floor='ff_hold', ambient=20.0,
                 controller_K_inv=None, controller_tau=None, plant_regime='measured',
                 controller_K=None, controller_L_pair=None,
                 fuzzy_strength_pct=0.0,
                 fuzzy_error_band_c=FUZZY_ERROR_BAND_C_DEFAULT,
                 fuzzy_rate_band_c_per_s=FUZZY_RATE_BAND_C_PER_S_DEFAULT,
                 measurement_quantum_c=0.0, measurement_noise_std_c=0.0, measurement_seed=0,
                 pwm_window_ms=0.0, pwm_min_on_ms=0.0, pwm_min_off_ms=0.0):
    """Run the plant+PID loop over an explicit segment list.

    ``segs``: list of ``(t0, t1, c0, c1, rate)`` tuples, ``rate`` signed
    C/s, ``0.0`` for a dwell. There is deliberately NO default profile and
    NO hardcoded rate here -- see the module docstring's "THE BUG THIS
    REPLACES" section. Every caller must supply real segment timing, either
    by hand or via ``segs_from_capture``/``run_profile_from_capture``.

    ``controller_K_inv``/``controller_tau``: override what the CONTROLLER
    (ff/PID) believes the matrix/tau are, independent of the plant it is
    actually driving (which always uses the module's measured K_full/tau/L
    -- see FOPDTPlant.step's high-temperature note). Only meaningful with
    climb_mode='coupled'; used by the matrix-variant sweep.

    Reports ``max_target_c`` and ``extrapolation`` (True if any target in
    this run exceeded ``EXTRAPOLATION_BOUNDARY_C``) so callers running into
    kiln-firing range know when they've left the measured 0-80 C envelope.

    ``plant_regime``: ``'measured'`` (default, unchanged since before this
    argument existed) drives ``FOPDTPlant`` -- the bench-rig identification,
    valid at and below ``EXTRAPOLATION_BOUNDARY_C``. ``'physical'`` drives
    ``PhysicalKilnPlant`` -- the ASSUMED, from-first-principles model of a
    real cone-10-capable kiln (see that class's docstring). A caller
    choosing between them per band is exactly what
    ``plant_sim_sweep._run_one`` does; every other caller in this module
    (capture comparison, regression tests) always uses ``'measured'``, so
    the identical low-temperature code path this argument's default
    preserves is what keeps the five-capture calibration fit unchanged.

    ``climb_mode='lag_compensated'``: opt-in candidate, SIMULATION ONLY (see
    ``LagCompensatedFF``). ``controller_K``/``controller_L_pair`` let a
    caller ask "what would this candidate compute if it believed a different
    matrix/per-path delay" the same way ``controller_K_inv``/
    ``controller_tau`` do for ``'coupled'`` -- default to the module's
    ``K_full``/``L_PAIR``. Ignored for every other ``climb_mode``.

    ``kp``/``ki``/``kd``: each is EITHER a scalar (applied to all three
    zones, the only behaviour that existed before sec 3.4's per-zone gain
    sweep -- see ``_broadcast_zone_param``) OR a length-3 sequence, one
    value per zone. A scalar reproduces the old code path exactly; nothing
    about the shared-gain default changes.

    ``fuzzy_strength_pct``: 0-100, applied uniformly to all three zones'
    ``PID`` instances -- see ``PID``'s docstring and ``pid_fuzzy_adjust()``
    above for the faithful mirror of ``pid_fuzzy.c``. Default 0.0 keeps
    every existing caller (regression tests, capture comparisons, the
    matrix sweep) on exactly the pre-fuzzy code path.

    ``measurement_quantum_c``/``measurement_noise_std_c``: the PID has
    always been fed ``plant.temp[i]`` directly -- the true, noise-free
    plant state -- with no model of the real measurement chain (MAX31856
    thermocouple ADC quantization) or of thermocouple noise. The fuzzy
    layer's whole design target (see ``pid_fuzzy_adjust``) is rejecting
    noisy-derivative behaviour that this omission cannot reproduce at all:
    a deterministic measurement can never exercise the fuzzy dead-band, so
    any fuzzy-vs-baseline comparison run through this simulator up to
    2026-09-02 was necessarily comparing on a signal the fuzzy layer was
    not built to react to. Both default to ``0.0`` (off, byte-for-byte the
    old code path) so every existing caller of ``run_profile`` itself
    (regression tests, capture comparisons, the matrix sweep) is
    unaffected -- see ``MEASURED_THERMO_NOISE_STD_C``/``MAX31856_QUANTUM_C``
    below for the values the gain-search entry points default these to
    instead.

    ``fuzzy_error_band_c``/``fuzzy_rate_band_c_per_s``: the per-zone
    ``error_band_c``/``rate_band_c_per_s`` config (ZONES_CFG_VERSION 19,
    commit 904db54) each ``PID`` instance's fuzzy layer uses -- see
    ``PID``'s own docstring. Each accepts a scalar (all zones) or a
    length-3 sequence (one per zone), same broadcast convention as
    ``kp``/``ki``/``kd`` (``_broadcast_zone_param``). Both default to the
    firmware's documented defaults (20.0 C / 0.5 C/s), so every existing
    caller is unaffected; a caller evaluating a candidate band -- e.g. the
    6-8 C / 0.20-0.25 C/s envelope under consideration for the owner's
    kiln -- MUST pass it here, or the prediction silently keeps simulating
    the old fixed band while the real board runs the new one.

    ``measurement_seed`` seeds the noise draw so a given call is
    reproducible; ``measurement_noise_std_c`` accepts a scalar (every zone)
    or a length-3 per-zone sequence, same convention as ``kp``/``ki``/
    ``kd`` (see ``_broadcast_zone_param``); quantization rounds to the
    nearest ``measurement_quantum_c`` -- ``MAX31856_QUANTUM_C`` (0.0078125
    C) is the real per-channel LSB, derived from
    ``firmware/KilnFW/App/drivers/hw/max31856_codec.h``, not the earlier 0.1 C
    placeholder some of this module's own mechanism tests still use as a
    generic exercise value -- and is applied AFTER the additive Gaussian
    noise, matching the real chain (continuous sensor + noise, then ADC
    quantization).

    The returned dict's ``measured`` array is the FED measurement series
    (what each PID actually saw each tick, post noise+quantization) --
    identical to ``temps`` when both measurement args are 0.0, and the
    only honest way to test the noise/quantization chain end to end.

    ``pwm_window_ms``/``pwm_min_on_ms``/``pwm_min_off_ms``: the PID's
    continuous duty in [0,1] has always driven the plant directly -- no
    model of ``heater_output.c``'s time-proportioning window, which is what
    actually turns that duty into relay transitions on real hardware. This
    was a deliberate, documented omission (see the module docstring's "Not
    modeled" section, now superseded) until PID_EXPANSION_PLAN.md sec 3.4's
    2026-09-03f addendum found it load-bearing: with no actuator downstream
    of the duty command, nothing in the objective can register the chatter
    a large kp/kd combination causes, so a gain search only ever answers
    "more gain, always." ``pwm_window_ms=0.0`` (default) keeps the exact old
    code path -- the plant sees the PID's continuous duty every tick,
    byte-identical to every caller before this parameter existed.
    ``pwm_window_ms>0`` renders EVERY zone's duty through ``_pwm_render``
    (a line-for-line port of ``heater_output_duty_ex``'s ordinary,
    non-``force_new_window`` path -- see that function's own docstring)
    before the plant ever sees it: the plant is driven by the actual 0/1
    relay state, not the continuous duty, so its own tau/lag is what
    averages the PWM back out, exactly as on real hardware. ``pwm_min_on_ms``/
    ``pwm_min_off_ms`` of ``0.0`` fall back to ``HEATER_DEFAULT_MIN_ON_MS``
    (via the floor inside ``_pwm_render``) / ``HEATER_DEFAULT_MIN_OFF_MS``,
    matching a zone with no per-zone heater timing configured (firmware's
    own "0 = not configured" convention, see ``heater_output.h``).

    The returned dict's ``relay_on`` array (bool, same shape as ``duty``) is
    the actual 0/1 relay command each zone got each tick when PWM modeling
    is active (all-``False``/unset when it is not); ``relay_cycles`` is each
    zone's whole-run relay transition count (``heater_output_state_t``'s own
    ``cycle_count`` field, mirrored here) -- see
    ``sim_relay_transitions_per_hour``/``sim_relay_cycle_life_fraction_per_hour``
    below for the actuator-cost figures the gain search reports.
    """
    if plant_regime == 'physical':
        plant = PhysicalKilnPlant(DT, ambient=ambient, start_temp=start_temp)
    elif plant_regime == 'measured':
        plant = FOPDTPlant(K_full, tau, L, DT, ambient=ambient, start_temp=start_temp)
    elif plant_regime == 'measured_per_path':
        plant = FOPDTPlantPerPath(K_full, L_PAIR, TAU_PAIR, DT, ambient=ambient, start_temp=start_temp)
    else:
        raise ValueError(
            f"unknown plant_regime {plant_regime!r}, expected 'measured', "
            "'measured_per_path' or 'physical'"
        )
    kp_arr = _broadcast_zone_param(kp)
    ki_arr = _broadcast_zone_param(ki)
    kd_arr = _broadcast_zone_param(kd)
    fuzzy_error_band_c_arr = _broadcast_zone_param(fuzzy_error_band_c)
    fuzzy_rate_band_c_per_s_arr = _broadcast_zone_param(fuzzy_rate_band_c_per_s)
    pids = [PID(kp_arr[i], ki_arr[i], kd_arr[i], d_tau=30.0, b=1.0, pid_range_c=1000.0,
                fuzzy_strength_pct=fuzzy_strength_pct,
                error_band_c=fuzzy_error_band_c_arr[i],
                rate_band_c_per_s=fuzzy_rate_band_c_per_s_arr[i]) for i in range(N_ZONES)]
    lag_ff = None
    if climb_mode == 'lag_compensated':
        K_ctrl = K_full if controller_K is None else controller_K
        L_pair_ctrl = L_PAIR if controller_L_pair is None else controller_L_pair
        lag_ff = LagCompensatedFF(K_ctrl, L_pair_ctrl, DT, N_ZONES)
        ff_fn = None
    else:
        ff_fn = coupled_ff_hold_climb if climb_mode == 'coupled' else uncoupled_ff_hold_climb

    total_t = segs[-1][1]
    times, targets, temps_log, duty_log, meas_log = [], [], [], [], []
    duty = np.zeros(N_ZONES)
    t = 0.0
    # measurement_noise_std_c accepts a scalar (applied to every zone,
    # byte-identical to the pre-per-zone-noise code path) or a length-3
    # sequence (e.g. MEASURED_THERMO_NOISE_STD_C, one sigma per zone --
    # real thermocouple noise is not the same on every channel, see that
    # constant's docstring). Broadcasting a scalar through
    # _broadcast_zone_param reproduces the exact same per-tick rng.normal()
    # call sequence the old scalar-only code made, so every existing
    # scalar caller is unaffected.
    noise_std_arr = _broadcast_zone_param(measurement_noise_std_c)
    rng = np.random.default_rng(measurement_seed) if np.any(noise_std_arr > 0.0) else None
    pwm_active = pwm_window_ms > 0.0
    pwm_states = [_PwmZoneState() for _ in range(N_ZONES)] if pwm_active else None
    dt_ms = DT * 1000.0
    relay_log = []
    while t <= total_t:
        seg_idx = None
        for si, (t0, t1, c0, c1, rate) in enumerate(segs):
            if t0 <= t <= t1 or si == len(segs) - 1:
                seg_idx = si
                if rate == 0.0:
                    target_c = c1
                    target_rate = 0.0
                else:
                    target_c = c0 + rate * (t - t0)
                    target_rate = rate
                break
        meas_row = np.zeros(N_ZONES)
        for i in range(N_ZONES):
            if climb_mode == 'coupled':
                hold, climb, ff = ff_fn(target_c, target_rate, i, ambient=ambient,
                                         K_inv=controller_K_inv, tau_ff=controller_tau)
            elif climb_mode == 'lag_compensated':
                hold, climb, ff = lag_ff(target_c, target_rate, i, ambient=ambient,
                                          tau_ff=controller_tau)
            else:
                hold, climb, ff = ff_fn(target_c, target_rate, i, ambient=ambient)
            meas_c = plant.temp[i]
            if rng is not None:
                meas_c = meas_c + rng.normal(0.0, noise_std_arr[i])
            if measurement_quantum_c > 0.0:
                meas_c = round(meas_c / measurement_quantum_c) * measurement_quantum_c
            duty[i], _ = pids[i].update(target_c, meas_c, DT, ff, hold, integral_floor=integral_floor)
            meas_row[i] = meas_c
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        meas_log.append(meas_row.copy())
        if lag_ff is not None:
            lag_ff.record(duty)
        if pwm_active:
            applied = np.array([
                1.0 if _pwm_render(pwm_states[i], duty[i], pwm_window_ms, pwm_min_on_ms,
                                    pwm_min_off_ms, dt_ms) else 0.0
                for i in range(N_ZONES)
            ])
            relay_log.append(applied.astype(bool))
            plant.step(applied)
        else:
            plant.step(duty)
        t += DT

    targets_arr = np.array(targets)
    max_target = float(targets_arr.max()) if len(targets_arr) else float(ambient)
    relay_cycles = ([s.cycle_count for s in pwm_states] if pwm_active
                     else [0, 0, 0])
    relay_on_arr = (np.array(relay_log) if pwm_active
                     else np.zeros((len(times), N_ZONES), dtype=bool))
    return dict(t=np.array(times), target=targets_arr,
                temps=np.array(temps_log), duty=np.array(duty_log),
                measured=np.array(meas_log),
                seg_bounds=[s[1] for s in segs],
                max_target_c=max_target,
                extrapolation=is_extrapolation(max_target),
                plant_regime=plant_regime,
                relay_on=relay_on_arr,
                relay_cycles=relay_cycles,
                pwm_active=pwm_active)


def segs_from_capture(rows: Sequence[log_analysis.PollRow]):
    """Builds the ``(t0,t1,c0,c1,rate)`` segment list ``run_profile()``
    needs directly from a parsed capture's rows (a SINGLE run -- call
    ``log_analysis.split_runs`` first for a multi-run file). Segment
    boundaries are exactly where the real firmware's own
    ``segment_index``/``dwelling`` changed and the ramp rate is derived
    from the capture's own elapsed time and target temperature at each
    boundary -- NOT assumed. This is the fix for the bug described at the
    top of this module: the old sims hardcoded a rate here instead of
    reading it off the capture.
    """
    bounds = []
    prev_key = None
    for r in rows:
        key = (r.segment_index, r.dwelling)
        if key != prev_key:
            bounds.append((r.elapsed_s, r.target_c, r.dwelling))
            prev_key = key
    bounds.append((rows[-1].elapsed_s, rows[-1].target_c, rows[-1].dwelling))

    segs = []
    start_c = None
    for z in rows[0].zones.values():
        start_c = z.actual_c
        break
    c_running = start_c
    for k in range(len(bounds) - 1):
        t0, c_at_start, dwelling = bounds[k]
        t1, c_end, _ = bounds[k + 1]
        if t1 <= t0:
            continue
        if dwelling:
            segs.append((t0, t1, c_end, c_end, 0.0))
            c_running = c_end
        else:
            rate = (c_end - c_running) / (t1 - t0) if t1 > t0 else 0.0
            segs.append((t0, t1, c_running, c_end, rate))
            c_running = c_end
    return segs, start_c


def run_profile_from_capture(rows: Sequence[log_analysis.PollRow], **kwargs):
    """``run_profile`` driven entirely off a real capture's own segment
    boundaries -- see ``segs_from_capture``. Returns ``(result, segs)``."""
    segs, start_c = segs_from_capture(rows)
    start_temp = [start_c, start_c, start_c]
    return run_profile(segs, start_temp, **kwargs), segs


# ---------------------------------------------------------------------------
# Sim-side windowed stats, in the same shape as log_analysis.ZoneWindowStats,
# so a sim run and a hardware capture can be reported and diffed side by
# side.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class SimZoneWindowStats:
    zone: int
    n_samples: int
    mean_error_c: float
    rms_error_c: float
    max_overshoot_c: float
    max_undershoot_c: float


def sim_window_zone_stats(result: dict, t0: float, t1: float, zone: int) -> Optional[SimZoneWindowStats]:
    t, target, temps = result['t'], result['target'], result['temps']
    m = (t >= t0) & (t <= t1)
    if not m.any():
        return None
    err = temps[m, zone] - target[m]
    return SimZoneWindowStats(
        zone=zone,
        n_samples=int(m.sum()),
        mean_error_c=float(err.mean()),
        rms_error_c=float(np.sqrt((err ** 2).mean())),
        max_overshoot_c=float(max(err.max(), 0.0)),
        max_undershoot_c=float(max(-err.min(), 0.0)),
    )


def sim_whole_run_iae_normalized(result: dict, zone: int) -> float:
    """Whole-run, time-weighted mean absolute error for ``zone`` -- the sim
    side of firmware's ``iae_normalized_c`` (``iae_raw_c_s / duration_s``,
    see ``firing_stats`` in a real capture and
    ``PID_EXPANSION_PLAN.md`` sec 3.8/3.4 for the noise-floor numbers this
    is meant to be comparable against). ``run_profile`` ticks at a uniform
    ``DT=1.0`` s, so ``iae_raw`` (the trapezoidal integral of ``|error|``
    over time) divided by the run's duration reduces exactly to the plain
    mean of ``|error|`` over every tick -- no separate integration needed.
    """
    err = np.abs(result['temps'][:, zone] - result['target'])
    return float(err.mean())


def sim_relay_transitions_per_hour(result: dict, zone: int) -> float:
    """Actuator-chatter figure: ``zone``'s whole-run relay transition count
    (``result['relay_cycles']``, only meaningful when ``run_profile`` was
    called with ``pwm_window_ms>0`` -- otherwise 0) normalized to
    transitions/hour by the run's own duration, so runs of different length
    are comparable. Mirrors ``heater_output_state_t.cycle_count``'s own
    accounting (see ``_pwm_render``) -- this is what physically wears K1-K4
    (and whatever it switches downstream)."""
    t = result['t']
    duration_s = float(t[-1] - t[0]) if len(t) > 1 else 0.0
    if duration_s <= 0.0:
        return 0.0
    return result['relay_cycles'][zone] * 3600.0 / duration_s


def sim_relay_cycle_life_fraction_per_hour(result: dict, zone: int) -> float:
    """``sim_relay_transitions_per_hour`` converted to a fraction of
    ``RELAY_RATED_LIFE_CYCLES`` consumed per hour of firing (a relay
    "cycle" is one full ON->OFF->ON, i.e. 2 transitions, matching how
    ``relay_cycles.h``'s own "a 60 s time-proportioning window can spend
    [the ~1e5-operation contact life budget] in a few hundred hours of
    firing" reconciles against ``RELAY_RATED_LIFE_CYCLES``: at a 10 s
    window (the smallest firmware allows, ``HEATER_MIN_ON_MS_FLOOR *
    HEATER_MIN_WINDOW_MULTIPLE``-adjacent), 360 windows/hour * 2
    transitions/window = 720 transitions/hour = 360 cycles/hour, and
    1e5 / 360 = 278 hours -- "a few hundred hours", matching. This is the
    physically GROUNDED half of the actuator-cost accounting (the number
    itself, and the consequence of exhausting it -- ``relay_cycles.h``'s
    own words: "a controller that fails mid-firing at cone temperature")
    -- see ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR``
    below for why the C-per-life-fraction TRADE weight is deliberately NOT
    similarly grounded and is swept instead."""
    cycles_per_hour = sim_relay_transitions_per_hour(result, zone) / 2.0
    return cycles_per_hour / RELAY_RATED_LIFE_CYCLES


def sim_duty_chatter_rate(result: dict, zone: int) -> float:
    """Whole-run mean absolute per-tick change in COMMANDED duty
    (``|diff(duty)| / DT``, duty-fraction/s) -- a second, complementary
    actuator-cost signal to ``sim_relay_transitions_per_hour``, measured on
    the continuous PID output BEFORE the PWM window quantizes it, not
    after.

    Why both are needed: negative-tested this module's own PWM window
    model (``_pwm_render``) by sweeping ``kp_mult`` 0.25x-8.0x on a real
    fit capture and found ``sim_relay_transitions_per_hour`` DECREASES as
    kp rises (98.5/hr at 0.25x down to 46.2/hr at 8.0x) -- the OPPOSITE of
    what "higher gain chatters the relay more" would predict. This is not
    a bug: ``heater_output_duty_ex`` samples ``duty`` only ONCE per 60 s
    window to decide that window's single on-time, so intra-window
    ringing at whatever rate a large kp/kd combination produces can never
    reach the relay as extra transitions -- the window is a hard low-pass
    filter on relay-visible chatter BY CONSTRUCTION, and a higher-gain
    loop that settles faster/harder actually spends MORE time saturated
    near duty 0 or 1 (fewer window-boundary crossings), not less. Real
    relay transition COUNT is therefore not the right signal for the
    "large kp/kd rings against sensor noise" mechanism this task exists to
    price -- confirmed by the same sweep: ``mean(abs(diff(duty)))`` (this
    function) rises MONOTONICALLY with kp_mult (0.0027 at 0.25x to 0.0170
    at 8.0x, roughly 6x), tracking exactly the ringing the relay-transition
    count cannot see. Both are reported (see ``per_zone_gain_grid_search``)
    rather than picking one, since they measure different, both-real
    things: relay contact life (post-window, physically grounded against
    K1-K4's rated cycles) and control-loop aggressiveness/ringing
    (pre-window, the mechanism actually driving whatever wear the window
    does not filter out -- element inrush current, downstream contactor
    chatter if the window is shortened later, etc).
    """
    duty = result['duty'][:, zone]
    if len(duty) < 2:
        return 0.0
    return float(np.mean(np.abs(np.diff(duty))) / DT)


# ---------------------------------------------------------------------------
# Per-zone gain sweep (added for PID_EXPANSION_PLAN.md sec 3.4, ranked
# improvement #3). Every sweep in this module before this point (the fuzzy
# sweep, the load sweep, the tuning campaign) drives all three zones with
# the SAME kp/ki/kd -- ``run_profile``'s ``kp``/``ki``/``kd`` were scalars
# only until ``_broadcast_zone_param`` above. This section asks the
# question directly: does letting each zone have its own gains, fit on one
# real capture and VALIDATED on a different, held-out one, beat the shared
# baseline by more than sec 3.4's own discrimination floor?
#
# Fit methodology, and why it is per-zone-independent rather than a joint
# 3-zone search: a PID's gains only change ITS OWN commanded duty; that
# duty reaches other zones only through the plant's cross-zone coupling
# (a second-order effect next to a zone's own error response). The
# project's own diagonal-only least-squares gain refinement (sec 3.3,
# "Integral diagnosis from dwells") already makes exactly this
# simplification for adaptive tuning on real hardware -- reusing it here
# keeps the sweep's own assumptions consistent with what the project
# already ships, rather than inventing a new one. Each zone's grid search
# holds the OTHER two zones at the shared baseline gains while scoring only
# the zone under test's own whole-run IAE.
# ---------------------------------------------------------------------------

#: Multiplicative grid applied to the per-zone baseline kp/ki independently
#: per zone. Kept small and round-number rather than fine-grained: the
#: discrimination-floor logic below is what decides whether any of this is
#: worth reading, not grid resolution.
PER_ZONE_GAIN_GRID_MULT = (0.5, 0.75, 1.0, 1.25, 1.5, 2.0)

#: The gains the live board actually runs today (read via ``control_get_
#: zones`` 2026-09-03), per zone [z0, z1, z2]. This REPLACES the
#: pre-2026-09-03 scalar defaults (kp=0.06, ki=0.0003, kd=0.0) that every
#: caller in this module used to fall back on -- that scalar was never what
#: any zone actually ran; it was carried forward from the single-zone-gain
#: era before ``_broadcast_zone_param`` existed. Two divergences matter:
#: kp/ki are each roughly 2-3x the old scalar guess (board kp is
#: 0.032-0.063 vs the old 0.06; board ki is 0.0001-0.0002 vs the old
#: 0.0003), and kd is **0.0 in the old default vs 0.84-1.07 on the real
#: board** -- the old default exercised NO derivative action at all, which
#: silently zeroed the ``d_term`` this module's own ``PID.update`` computes
#: (see its docstring: the mechanism is a line-for-line match to
#: ``pid.c``, only the *default gain* fed to it was stale). A retune
#: candidate searched from the old scalar baseline was therefore searching
#: from a controller that is not the one running on the kiln. See
#: PID_EXPANSION_PLAN.md sec 3.4's 2026-09-03 addendum for the fidelity
#: audit and the re-run gain search.
BOARD_ZONE_KP = (0.0318, 0.0485, 0.0631)
BOARD_ZONE_KI = (0.00010, 0.00020, 0.00020)
BOARD_ZONE_KD = (0.8401, 1.0548, 1.0690)


# ---------------------------------------------------------------------------
# Measurement noise/quantization model (added 2026-09-03, closing the gap
# PID_EXPANSION_PLAN.md sec 3.4's 2026-09-03 addendum identified: a gain
# search run with no noise/quantization model active never penalizes an
# ever-larger kp/kd, so its "optimum" just tracks whatever grid edge it is
# given -- every zone pinned at 2.00x on a 0.5x-2.0x grid, then the wide
# 0.25x-8.0x grid just moved the edge and flipped z2's sign. Both constants
# below are DERIVED/MEASURED, not guessed:
# ---------------------------------------------------------------------------

#: Real MAX31856 thermocouple-channel ADC resolution, derived from
#: firmware/KilnFW/App/drivers/hw/max31856_codec.h's
#: MAX31856_TC_TEMP_C_PER_LSB (1/4096 C per raw 24-bit-word LSB; the low 5
#: bits of that word are hardware-fixed 0, so the real step between
#: representable temperatures is 32x that, i.e. 1/128 = 0.0078125 C per
#: 19-bit code -- the codec header states this equivalence directly). This
#: SUPERSEDES the earlier 0.1 C figure used elsewhere in this module/its
#: tests, which was a round-number placeholder for exercising the
#: quantization mechanism, not a value read off the driver.
MAX31856_QUANTUM_C = 0.0078125

#: Per-zone thermocouple measurement noise sigma (C), MEASURED directly off
#: the SAME 2026-09-02 coupling excitation captures used to identify
#: K_full/tau/L above (logs/coupling/cpl_z{0,1,2}_thermo.jsonl, one
#: excitation run per zone, ~20 s poll cadence): each zone's own-channel
#: reading was linearly detrended over a rested/steady dwell window (a
#: plateau well after the step, before that run's own cooldown) and the
#: residual std taken, so the number reflects sensor+ADC noise with the
#: (small, near-linear) settling/cooling drift removed rather than folded
#: in as extra "noise":
#:   z0: cpl_z0_thermo.jsonl CH0, samples[260:300] (40 samples, tail of a
#:       fully-settled dwell, detrend slope -0.017 C/sample) -> std 0.0584 C
#:   z1: cpl_z1_thermo.jsonl CH1, samples[90:139]  (49 samples, dwell
#:       plateau after the ramp) -> std 0.0631 C
#:   z2: cpl_z2_thermo.jsonl CH2, samples[90:139]  (49 samples, same shape)
#:       -> std 0.0907 C
#: Cross-check, same order of magnitude via an independent method: the
#: whole-run IAE noise floors PID_EXPANSION_PLAN.md sec 3.8 built from SIX
#: repeat noise_floor_p7* captures (run-to-run range, not per-tick std, so
#: expected to run somewhat higher) are 0.116/0.077/0.147 C for z0/z1/z2.
#: Injected in run_profile() at DT=1.0 s ticks even though the source
#: captures poll at ~20 s: each MAX31856 conversion is an independent read
#: (no on-board averaging), so per-sample noise magnitude does not shrink
#: at a faster poll rate -- only the number of independent draws per
#: second changes, which is exactly what feeding the same sigma into every
#: 1 Hz tick reproduces.
MEASURED_THERMO_NOISE_STD_C = (0.0584, 0.0631, 0.0907)

#: Fixed, explicit seed set for gain-search noise averaging. A single
#: stochastic draw per candidate would let noise alone pick the "winner";
#: this project has also been bitten by a seed that PERSISTED across runs
#: and bled state between them (project_scenario_runner_state_bleed) -- the
#: opposite failure. Both are avoided here: each run_profile() call builds
#: its OWN fresh ``np.random.default_rng(seed)`` from an explicit seed in
#: this fixed tuple (no shared/mutated generator crosses calls), and every
#: candidate in a given search -- including the baseline -- is scored
#: against this SAME seed set (common random numbers), so a comparison
#: between two candidates is a paired comparison on matched noise draws,
#: not two independent noisy samples.
GAIN_SEARCH_NOISE_SEEDS = (0, 1, 2, 3, 4)

#: Swept, NOT physically derived, weights converting
#: ``sim_relay_cycle_life_fraction_per_hour`` (a physically grounded number
#: -- see that function's docstring) into the same C units as whole-run IAE
#: so the two can be added into one composite objective. The
#: transitions/life-fraction accounting itself IS grounded
#: (``relay_cycles.h``'s stated ~1e5-operation contact life budget, the
#: SAME accounting the firmware persists for the operator); the TRADE --
#: "how many degrees C of tracking error is one hour's worth of 100%
#: rated-life consumption worth" -- is a value judgement this repo has no
#: data to fix a single number for (relay_cycles.h states the CONSEQUENCE
#: of exhausting the budget -- "a controller that fails mid-firing at cone
#: temperature" -- but not a $ or degrees-C price on that risk), so per
#: this task's own instruction
#: ("if you cannot ground the weighting physically ... expose it as a swept
#: parameter rather than inventing authority for a number") it is swept
#: here rather than defaulted to one invented constant.
#: ``per_zone_gain_grid_search``'s own ``actuator_weight_...`` argument
#: still needs ONE value to pick an argmin with -- it defaults to 0.0
#: (pure tracking-error argmin, i.e. today's behaviour) precisely so that
#: choice is explicit and visible at the call site, not buried here.
#: ``actuator_weight_sensitivity_sweep`` below runs the search at every
#: weight in this tuple and reports how (or whether) the chosen gain moves.
GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR = (0.0, 1.0, 10.0, 100.0, 1000.0, 10000.0)

#: Swept, NOT physically derived, weights converting
#: ``sim_duty_chatter_rate`` (duty-fraction/s, pre-window control-signal
#: ringing) into the same C units as whole-run IAE. See that function's
#: docstring for why this SECOND actuator signal exists: the post-window
#: relay-transition count (above) turns out to be a poor proxy for
#: kp/kd-driven ringing, because the 60 s window itself is a hard low-pass
#: filter on relay-visible transitions -- confirmed by a negative test
#: (kp_mult 0.25x-8.0x on p7_oldmatrix_http.jsonl z0: transitions/hour FALL
#: from 98.5 to 46.2 as kp rises, the opposite of "more gain chatters the
#: relay more"), while ``sim_duty_chatter_rate`` rises monotonically
#: (~0.0027 to ~0.0170 duty/s) over the same sweep -- it is what a large
#: kp/kd actually does to the control signal, whether or not the window
#: happens to filter it into relay transitions today. There is no data in
#: this repo converting "duty/s of ringing" into a wear or dollar figure
#: (unlike the relay-life weight above, which at least has K1-K4's rated
#: cycle count to anchor one side of the trade), so this is swept, not
#: defaulted to one number, same rationale as the life-fraction weights.
GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE = (0.0, 1.0, 10.0, 50.0, 100.0, 300.0)


def _scores_over_seeds(rows, kp_vec, ki_vec, kd_vec, climb_mode, integral_floor, zone,
                        measurement_noise_std_c, measurement_quantum_c, measurement_seeds,
                        pwm_window_ms, pwm_min_on_ms, pwm_min_off_ms):
    """Runs ``run_profile_from_capture`` once per seed in ``measurement_seeds``
    (skipped entirely -- a single noise-free call -- when
    ``measurement_noise_std_c`` is all-zero, so a caller that explicitly
    disables noise pays no repeat-averaging cost and gets the exact old
    single-call behaviour) and returns the mean, over seeds, of ALL THREE
    objective components for ``zone``: whole-run normalized IAE (tracking
    error, degrees C), relay life-fraction consumed per hour (post-window
    actuator cost, dimensionless -- see
    ``sim_relay_cycle_life_fraction_per_hour``), and duty chatter rate
    (pre-window actuator cost, duty-fraction/s -- see
    ``sim_duty_chatter_rate``; both actuator signals exist because they
    were found to disagree in sign -- see that function's docstring).
    Averaging every term over the same noise seeds as IAE keeps all three
    components paired on identical draws, same rationale as
    ``GAIN_SEARCH_NOISE_SEEDS``'s common-random-numbers use for IAE alone.
    PWM rendering (when ``pwm_window_ms>0``) is itself noise-independent
    (it depends only on the PID's commanded duty, not the measurement it
    was computed from) but is re-run per seed anyway since it shares the
    ``run_profile_from_capture`` call that also needs the seeded
    measurement chain.
    """
    noise_arr = _broadcast_zone_param(measurement_noise_std_c)
    kwargs = dict(
        climb_mode=climb_mode, integral_floor=integral_floor,
        measurement_noise_std_c=measurement_noise_std_c,
        measurement_quantum_c=measurement_quantum_c,
        pwm_window_ms=pwm_window_ms, pwm_min_on_ms=pwm_min_on_ms,
        pwm_min_off_ms=pwm_min_off_ms,
    )

    def _one(result):
        return dict(iae=sim_whole_run_iae_normalized(result, zone),
                    life_fraction_per_hour=sim_relay_cycle_life_fraction_per_hour(result, zone),
                    transitions_per_hour=sim_relay_transitions_per_hour(result, zone),
                    duty_chatter_rate=sim_duty_chatter_rate(result, zone))

    if not np.any(noise_arr > 0.0):
        result, _ = run_profile_from_capture(rows, kp=kp_vec, ki=ki_vec, kd=kd_vec, **kwargs)
        return _one(result)
    rows_scores = []
    for seed in measurement_seeds:
        result, _ = run_profile_from_capture(rows, kp=kp_vec, ki=ki_vec, kd=kd_vec,
                                              measurement_seed=seed, **kwargs)
        rows_scores.append(_one(result))
    return {k: float(np.mean([r[k] for r in rows_scores])) for k in rows_scores[0]}


def per_zone_gain_grid_search(rows_fit: Sequence[log_analysis.PollRow],
                               base_kp=BOARD_ZONE_KP, base_ki=BOARD_ZONE_KI,
                               base_kd=BOARD_ZONE_KD,
                               grid=PER_ZONE_GAIN_GRID_MULT,
                               climb_mode: str = 'coupled',
                               integral_floor: str = 'ff_hold',
                               measurement_noise_std_c=MEASURED_THERMO_NOISE_STD_C,
                               measurement_quantum_c=MAX31856_QUANTUM_C,
                               measurement_seeds=GAIN_SEARCH_NOISE_SEEDS,
                               pwm_window_ms=HEATER_DEFAULT_WINDOW_MS,
                               pwm_min_on_ms=0.0, pwm_min_off_ms=0.0,
                               actuator_weight_c_per_life_fraction_per_hour=0.0,
                               actuator_weight_c_per_duty_chatter_rate=0.0) -> dict:
    """For each zone independently, grid-searches ``kp``/``ki`` multipliers
    (``kd`` left at ``base_kd`` -- the board's own per-zone ``kd`` is held
    fixed rather than swept, both because no capture in this repo was
    fit/scored with a non-default ``kd`` sweep grid and to keep this
    search's scope matched to sec 3.4's original method: only ``kp``/``ki``
    are searched) that minimize THAT zone's own COMPOSITE objective on
    ``rows_fit`` -- whole-run normalized IAE (tracking error) plus
    ``actuator_weight_c_per_life_fraction_per_hour`` times the relay
    life-fraction consumed per hour of firing (actuator/chatter cost, see
    ``sim_relay_cycle_life_fraction_per_hour``) -- holding the other two
    zones at the shared/per-zone baseline gains (see module-level comment
    above). ``base_kp``/``base_ki``/``base_kd`` each accept a scalar
    (applied to all zones) or a length-3 per-zone sequence -- default is
    ``BOARD_ZONE_KP``/``_KI``/``_KD``, what the live board actually runs,
    per zone. Returns the best per-zone ``(kp, ki)`` plus BOTH objective
    components, reported SEPARATELY, at baseline and at the chosen gains,
    for every zone -- never only the blended composite, so a caller does
    not have to reverse-engineer how much of a "win" is tracking error vs.
    actuator cost.

    ``measurement_noise_std_c``/``measurement_quantum_c``/
    ``measurement_seeds``: default ON here (unlike ``run_profile`` itself,
    which defaults these to 0.0 to keep the historical fixture regressions
    byte-identical -- see the module docstring's "Not modeled" section) to
    ``MEASURED_THERMO_NOISE_STD_C``/``MAX31856_QUANTUM_C``/
    ``GAIN_SEARCH_NOISE_SEEDS``, because a gain search is exactly the case
    this omission broke: PID_EXPANSION_PLAN.md sec 3.4's 2026-09-03
    addendum found every zone's chosen multiplier pinning at the grid's own
    edge (then moving with it on a wider grid) once a realistic nonzero
    ``kd`` was in play, because nothing in a noise-free simulator penalizes
    an ever-larger kp/kd combination with anything that looks like
    noise-amplified ringing. Pass ``measurement_noise_std_c=0.0`` (or an
    all-zero sequence) to recover the old noise-free, single-call-per-
    candidate search exactly.

    ``pwm_window_ms``: also defaults ON here (to ``HEATER_DEFAULT_WINDOW_MS``,
    the firmware's own default -- unlike ``run_profile`` itself, same
    rationale as the noise defaults above), because it is what makes
    ``actuator_weight_c_per_life_fraction_per_hour`` mean anything: with no
    PWM window there ARE no relay transitions to count and every candidate's
    actuator cost reads 0.0 regardless of gain. Pass ``pwm_window_ms=0.0``
    to disable actuator modeling entirely (old behaviour, actuator terms
    all read 0.0).

    ``actuator_weight_c_per_life_fraction_per_hour``/
    ``actuator_weight_c_per_duty_chatter_rate``: the two C-per-actuator-cost
    TRADE weights -- NEITHER is physically derivable from anything in this
    repo (see ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR``/
    ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE``'s docstrings
    for why -- and for why BOTH actuator signals exist: they disagree in
    sign as kp rises, because the PWM window itself filters kp/kd-driven
    ringing out of the relay-transition count before it can be measured
    there). Both default to ``0.0``, i.e. pure tracking-error argmin --
    IDENTICAL candidate selection to before these parameters existed, so an
    existing caller sees no change unless it opts in. Use
    ``actuator_weight_sensitivity_sweep`` to see how the chosen gain moves
    as a weight is swept across
    ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR``/
    ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE`` rather than
    committing to one value here.
    """
    base_kp_arr = _broadcast_zone_param(base_kp)
    base_ki_arr = _broadcast_zone_param(base_ki)
    base_kd_arr = _broadcast_zone_param(base_kd)

    def _composite(scores):
        return (scores['iae']
                + actuator_weight_c_per_life_fraction_per_hour * scores['life_fraction_per_hour']
                + actuator_weight_c_per_duty_chatter_rate * scores['duty_chatter_rate'])

    def _score(kp_vec, ki_vec, zone):
        return _scores_over_seeds(
            rows_fit, kp_vec, ki_vec, list(base_kd_arr),
            climb_mode, integral_floor, zone,
            measurement_noise_std_c, measurement_quantum_c, measurement_seeds,
            pwm_window_ms, pwm_min_on_ms, pwm_min_off_ms,
        )

    best = {}
    for zone in range(N_ZONES):
        best_cost = None
        best_mult = (1.0, 1.0)
        best_scores = None
        for kp_mult in grid:
            for ki_mult in grid:
                kp_vec = list(base_kp_arr)
                ki_vec = list(base_ki_arr)
                kp_vec[zone] = base_kp_arr[zone] * kp_mult
                ki_vec[zone] = base_ki_arr[zone] * ki_mult
                scores = _score(kp_vec, ki_vec, zone)
                cost = _composite(scores)
                if best_cost is None or cost < best_cost:
                    best_cost = cost
                    best_mult = (kp_mult, ki_mult)
                    best_scores = scores
        baseline_scores = _score(list(base_kp_arr), list(base_ki_arr), zone)
        best[zone] = dict(
            kp=base_kp_arr[zone] * best_mult[0], ki=base_ki_arr[zone] * best_mult[1],
            kd=base_kd_arr[zone],
            kp_mult=best_mult[0], ki_mult=best_mult[1],
            # Tracking-error component (degrees C, whole-run mean |error|).
            fit_iae_baseline=baseline_scores['iae'], fit_iae_tuned=best_scores['iae'],
            # Actuator/chatter component, reported in TWO physical units:
            # relay life-fraction consumed per hour of firing (the one used
            # in the composite) and raw transitions/hour (easier to read at
            # a glance -- see sim_relay_transitions_per_hour).
            fit_life_fraction_per_hour_baseline=baseline_scores['life_fraction_per_hour'],
            fit_life_fraction_per_hour_tuned=best_scores['life_fraction_per_hour'],
            fit_transitions_per_hour_baseline=baseline_scores['transitions_per_hour'],
            fit_transitions_per_hour_tuned=best_scores['transitions_per_hour'],
            fit_duty_chatter_rate_baseline=baseline_scores['duty_chatter_rate'],
            fit_duty_chatter_rate_tuned=best_scores['duty_chatter_rate'],
            # Composite (what argmin actually selected on) -- never the
            # only number reported, per this function's own docstring.
            fit_composite_baseline=_composite(baseline_scores), fit_composite_tuned=best_cost,
            actuator_weight_c_per_life_fraction_per_hour=actuator_weight_c_per_life_fraction_per_hour,
            actuator_weight_c_per_duty_chatter_rate=actuator_weight_c_per_duty_chatter_rate,
        )
    return best


def actuator_weight_sensitivity_sweep(rows_fit: Sequence[log_analysis.PollRow],
                                       weight_kind: str = 'duty_chatter',
                                       weights=None,
                                       **kwargs) -> dict:
    """Runs ``per_zone_gain_grid_search`` once per weight and returns, per
    zone, the chosen ``(kp_mult, ki_mult)`` at every weight -- this IS the
    "expose it as a swept parameter" alternative to inventing one trade
    constant (see ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR``/
    ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE``'s docstrings).
    ``weight_kind``: ``'duty_chatter'`` (default -- sweeps
    ``actuator_weight_c_per_duty_chatter_rate`` across
    ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE``; this is the
    signal that actually rises with kp, see ``sim_duty_chatter_rate``) or
    ``'relay_life'`` (sweeps ``actuator_weight_c_per_life_fraction_per_hour``
    across ``GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR`` --
    kept for completeness/negative-testing even though that signal was
    found to move the WRONG way with kp on this window model). ``weights``
    overrides the default tuple for the chosen kind.

    Read the returned ``mult_by_weight`` sequence to see whether any swept
    weight pulls a zone's optimum off the grid edge and keeps it there (an
    interior optimum, the acceptance criterion this sweep exists to test),
    or whether it stays pinned at every weight up to the largest one swept
    (meaning actuator cost alone, at any plausible trade rate, still cannot
    explain a bounded optimum on this capture -- a genuine negative result,
    not a search failure). ``**kwargs`` forwards to
    ``per_zone_gain_grid_search`` (grid, climb_mode, noise/PWM settings,
    etc).
    """
    if weight_kind not in ('duty_chatter', 'relay_life'):
        raise ValueError(f"weight_kind must be 'duty_chatter' or 'relay_life', got {weight_kind!r}")
    param_name = ('actuator_weight_c_per_duty_chatter_rate' if weight_kind == 'duty_chatter'
                  else 'actuator_weight_c_per_life_fraction_per_hour')
    if weights is None:
        weights = (GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE if weight_kind == 'duty_chatter'
                   else GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_LIFE_FRACTION_PER_HOUR)
    kwargs.pop('actuator_weight_c_per_life_fraction_per_hour', None)
    kwargs.pop('actuator_weight_c_per_duty_chatter_rate', None)
    by_zone = {z: [] for z in range(N_ZONES)}
    for w in weights:
        result = per_zone_gain_grid_search(rows_fit, **{param_name: w}, **kwargs)
        for zone in range(N_ZONES):
            by_zone[zone].append(dict(
                weight=w,
                kp_mult=result[zone]['kp_mult'], ki_mult=result[zone]['ki_mult'],
                fit_iae_tuned=result[zone]['fit_iae_tuned'],
                fit_life_fraction_per_hour_tuned=result[zone]['fit_life_fraction_per_hour_tuned'],
                fit_transitions_per_hour_tuned=result[zone]['fit_transitions_per_hour_tuned'],
                fit_duty_chatter_rate_tuned=result[zone]['fit_duty_chatter_rate_tuned'],
            ))
    return {zone: dict(mult_by_weight=vals) for zone, vals in by_zone.items()}


def per_zone_gain_holdout_report(rows_fit: Sequence[log_analysis.PollRow],
                                  rows_test: Sequence[log_analysis.PollRow],
                                  base_kp=BOARD_ZONE_KP, base_ki=BOARD_ZONE_KI,
                                  base_kd=BOARD_ZONE_KD,
                                  grid=PER_ZONE_GAIN_GRID_MULT,
                                  climb_mode: str = 'coupled',
                                  integral_floor: str = 'ff_hold',
                                  measurement_noise_std_c=MEASURED_THERMO_NOISE_STD_C,
                                  measurement_quantum_c=MAX31856_QUANTUM_C,
                                  measurement_seeds=GAIN_SEARCH_NOISE_SEEDS,
                                  pwm_window_ms=HEATER_DEFAULT_WINDOW_MS,
                                  pwm_min_on_ms=0.0, pwm_min_off_ms=0.0,
                                  actuator_weight_c_per_life_fraction_per_hour=0.0,
                                  actuator_weight_c_per_duty_chatter_rate=0.0) -> dict:
    """Fits per-zone gains on ``rows_fit`` (``per_zone_gain_grid_search``),
    then scores BOTH the shared-baseline gains and the per-zone gains on
    ``rows_test`` -- a capture the fit never saw. This is the only honest
    comparison: scoring the tuned gains on the same capture they were fit
    on would trivially favor per-zone gains (more free parameters, same
    data) and cannot show whether the win generalizes.

    Applies all three zones' per-zone-fit gains simultaneously when scoring
    the "per-zone" row on the held-out capture (not one zone at a time) --
    that is the actual deployment shape a per-zone-gains recommendation
    would take.

    ``measurement_noise_std_c``/``measurement_quantum_c``/
    ``measurement_seeds``/``pwm_window_ms``/``pwm_min_on_ms``/
    ``pwm_min_off_ms``/``actuator_weight_c_per_life_fraction_per_hour``:
    forwarded to ``per_zone_gain_grid_search`` for the fit, and used
    identically (same seed set, common random numbers, same PWM window)
    when scoring baseline vs. tuned on the held-out capture -- see that
    function's docstring. Both tracking error AND actuator cost are
    reported separately on the held-out set too, not just at fit time.
    """
    fit = per_zone_gain_grid_search(
        rows_fit, base_kp=base_kp, base_ki=base_ki, base_kd=base_kd,
        grid=grid, climb_mode=climb_mode, integral_floor=integral_floor,
        measurement_noise_std_c=measurement_noise_std_c,
        measurement_quantum_c=measurement_quantum_c,
        measurement_seeds=measurement_seeds,
        pwm_window_ms=pwm_window_ms, pwm_min_on_ms=pwm_min_on_ms,
        pwm_min_off_ms=pwm_min_off_ms,
        actuator_weight_c_per_life_fraction_per_hour=actuator_weight_c_per_life_fraction_per_hour,
        actuator_weight_c_per_duty_chatter_rate=actuator_weight_c_per_duty_chatter_rate,
    )
    kp_vec = [fit[z]['kp'] for z in range(N_ZONES)]
    ki_vec = [fit[z]['ki'] for z in range(N_ZONES)]
    base_kd_arr = list(_broadcast_zone_param(base_kd))

    # One simulation per seed covers all three zones at once (a single
    # run_profile call reports every zone's temps/target/relay state) --
    # computed once here and re-used for every zone's IAE/actuator cost
    # below, rather than re-running the identical trajectory per zone the
    # way per-zone _scores_over_seeds calls would.
    noise_arr = _broadcast_zone_param(measurement_noise_std_c)
    seeds = measurement_seeds if np.any(noise_arr > 0.0) else (None,)

    def _mean_scores_all_zones(kp_v, ki_v):
        per_zone_iae = {z: [] for z in range(N_ZONES)}
        per_zone_life = {z: [] for z in range(N_ZONES)}
        per_zone_trans = {z: [] for z in range(N_ZONES)}
        per_zone_chatter = {z: [] for z in range(N_ZONES)}
        for seed in seeds:
            kwargs = dict(kp=kp_v, ki=ki_v, kd=base_kd_arr,
                           climb_mode=climb_mode, integral_floor=integral_floor,
                           measurement_noise_std_c=measurement_noise_std_c,
                           measurement_quantum_c=measurement_quantum_c,
                           pwm_window_ms=pwm_window_ms, pwm_min_on_ms=pwm_min_on_ms,
                           pwm_min_off_ms=pwm_min_off_ms)
            if seed is not None:
                kwargs["measurement_seed"] = seed
            result, _ = run_profile_from_capture(rows_test, **kwargs)
            for z in range(N_ZONES):
                per_zone_iae[z].append(sim_whole_run_iae_normalized(result, z))
                per_zone_life[z].append(sim_relay_cycle_life_fraction_per_hour(result, z))
                per_zone_trans[z].append(sim_relay_transitions_per_hour(result, z))
                per_zone_chatter[z].append(sim_duty_chatter_rate(result, z))
        return {z: dict(iae=float(np.mean(per_zone_iae[z])),
                         life_fraction_per_hour=float(np.mean(per_zone_life[z])),
                         transitions_per_hour=float(np.mean(per_zone_trans[z])),
                         duty_chatter_rate=float(np.mean(per_zone_chatter[z])))
                for z in range(N_ZONES)}

    base_scores_all = _mean_scores_all_zones(list(_broadcast_zone_param(base_kp)),
                                              list(_broadcast_zone_param(base_ki)))
    tuned_scores_all = _mean_scores_all_zones(kp_vec, ki_vec)

    per_zone = {}
    for zone in range(N_ZONES):
        base_s = base_scores_all[zone]
        tuned_s = tuned_scores_all[zone]
        per_zone[zone] = dict(
            fit=fit[zone],
            test_iae_baseline=base_s['iae'],
            test_iae_tuned=tuned_s['iae'],
            test_delta_c=tuned_s['iae'] - base_s['iae'],  # negative == per-zone gains won on tracking error
            test_life_fraction_per_hour_baseline=base_s['life_fraction_per_hour'],
            test_life_fraction_per_hour_tuned=tuned_s['life_fraction_per_hour'],
            test_transitions_per_hour_baseline=base_s['transitions_per_hour'],
            test_transitions_per_hour_tuned=tuned_s['transitions_per_hour'],
            test_duty_chatter_rate_baseline=base_s['duty_chatter_rate'],
            test_duty_chatter_rate_tuned=tuned_s['duty_chatter_rate'],
        )
    return dict(per_zone=per_zone, base_kp=base_kp, base_ki=base_ki, base_kd=base_kd)


def format_per_zone_gain_holdout_report_text(report: dict) -> str:
    lines = ["=== Per-zone gain grid search: fit on one capture, scored on a held-out one ==="]
    for zone, d in sorted(report["per_zone"].items()):
        f = d["fit"]
        lines.append(
            f"z{zone}: fit kp_mult={f['kp_mult']:.2f} ki_mult={f['ki_mult']:.2f} "
            f"(kp={f['kp']:.5f} ki={f['ki']:.6f})"
        )
        lines.append(
            f"    held-out tracking IAE: shared={d['test_iae_baseline']:.4f}C  "
            f"per-zone={d['test_iae_tuned']:.4f}C  delta={d['test_delta_c']:+.4f}C "
            f"({'per-zone better' if d['test_delta_c'] < 0 else 'shared better or tied'})"
        )
        lines.append(
            f"    held-out actuator cost: shared={d['test_transitions_per_hour_baseline']:.1f}/hr  "
            f"per-zone={d['test_transitions_per_hour_tuned']:.1f}/hr "
            f"(life-fraction/hr shared={d['test_life_fraction_per_hour_baseline']:.6f} "
            f"per-zone={d['test_life_fraction_per_hour_tuned']:.6f})"
        )
    return "\n".join(lines)


def render_sim_report(segs, start_temp, kp=0.06, ki=0.0003, kd=0.0,
                       climb_mode='coupled', integral_floor='ff_hold', ambient=20.0) -> dict:
    """Run the plant model over ``segs`` and report per-window, per-zone
    stats in the shape ``log_analysis.render_firing_report`` reports for
    real hardware -- so sim output is directly comparable to a hardware
    firing report without a separate mental model. No hardware capture
    required (contrast ``render_sim_vs_capture_report``, which additionally
    diffs against one)."""
    result = run_profile(segs, start_temp, kp=kp, ki=ki, kd=kd,
                          climb_mode=climb_mode, integral_floor=integral_floor, ambient=ambient)
    windows = []
    t_arr = result['t']
    boundaries = [0.0] + result['seg_bounds']
    for idx, (t0, t1) in enumerate(zip(boundaries[:-1], boundaries[1:])):
        rate = segs[idx][4]
        phase = "dwell" if rate == 0.0 else "ramp"
        zstats = {}
        for z in range(N_ZONES):
            s = sim_window_zone_stats(result, t0, t1, z)
            if s:
                zstats[z] = s
        windows.append(dict(segment_index=idx, phase=phase, start_s=t0, end_s=t1, zones=zstats))
    return dict(kp=kp, ki=ki, kd=kd, climb_mode=climb_mode, integral_floor=integral_floor,
                windows=windows, result=result)


def format_sim_report_text(report: dict) -> str:
    lines = [
        f"plant_sim run: climb_mode={report['climb_mode']} integral_floor={report['integral_floor']} "
        f"kp={report['kp']} ki={report['ki']} kd={report['kd']}",
    ]
    for w in report["windows"]:
        lines.append(f"-- seg {w['segment_index']} {w['phase']:5s} [{w['start_s']:.0f}s..{w['end_s']:.0f}s]")
        for z in sorted(w["zones"]):
            s = w["zones"][z]
            lines.append(
                f"    z{z}: mean_err={s.mean_error_c:+.2f}C rms={s.rms_error_c:.2f}C "
                f"over={s.max_overshoot_c:.2f}C under={s.max_undershoot_c:.2f}C n={s.n_samples}"
            )
    return "\n".join(lines)


def render_sim_vs_capture_report(path: str, run_idx: int = 0, kp=0.06, ki=0.0003, kd=0.0,
                                  climb_mode='coupled', integral_floor='ff_hold', ambient=20.0) -> dict:
    """Simulate the exact profile a hardware capture ran (via
    ``segs_from_capture``) and diff the sim's windowed per-zone stats
    against ``log_analysis``'s own windowed stats for the same capture.
    This is the calibration comparison itself, promoted to a reusable
    function -- see ``tests/test_plant_sim.py`` for the pinned regression
    that uses this against ``tests/fixtures/plant_sim/after.jsonl``.
    """
    rows_all = log_analysis.parse_profile_exec_jsonl(path)
    runs = log_analysis.split_runs(rows_all)
    if run_idx >= len(runs):
        return {"error": f"{path}: run_idx {run_idx} out of range ({len(runs)} runs in file)"}
    rows = runs[run_idx]

    result, segs = run_profile_from_capture(rows, kp=kp, ki=ki, kd=kd,
                                             climb_mode=climb_mode, integral_floor=integral_floor, ambient=ambient)
    windows = log_analysis.build_windows(rows)
    zones = log_analysis.zones_in_rows(rows)

    rows_out = []
    residuals = []
    for w in windows:
        if w.duration_s <= 0:
            continue
        for z in zones:
            hw = log_analysis.window_zone_stats(w, rows, z)
            if hw is None:
                continue
            sw = sim_window_zone_stats(result, w.start_s, w.end_s, z)
            if sw is None:
                continue
            d_mean = sw.mean_error_c - hw.mean_error_c
            d_rms = sw.rms_error_c - hw.rms_error_c
            residuals.append(d_mean)
            rows_out.append(dict(
                segment_index=w.segment_index, phase=w.phase, zone=z,
                hw_mean=hw.mean_error_c, hw_rms=hw.rms_error_c,
                sim_mean=sw.mean_error_c, sim_rms=sw.rms_error_c,
                d_mean=d_mean, d_rms=d_rms,
            ))

    resid = np.array(residuals) if residuals else np.array([0.0])
    return dict(
        path=path, run_idx=run_idx, runs_in_file=len(runs),
        kp=kp, ki=ki, kd=kd, climb_mode=climb_mode, integral_floor=integral_floor,
        windows=rows_out,
        n_windows=len(rows_out),
        mean_abs_residual_c=float(np.abs(resid).mean()),
        rms_residual_c=float(np.sqrt((resid ** 2).mean())),
        max_abs_residual_c=float(np.abs(resid).max()),
    )


def format_sim_vs_capture_report_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [
        f"sim vs hardware: {report['path']} run #{report['run_idx'] + 1}/{report['runs_in_file']}  "
        f"climb_mode={report['climb_mode']} integral_floor={report['integral_floor']} "
        f"kp={report['kp']} ki={report['ki']} kd={report['kd']}",
    ]
    for w in report["windows"]:
        lines.append(
            f"  seg{w['segment_index']} {w['phase']:5s} z{w['zone']} "
            f"HW mean={w['hw_mean']:+.3f} rms={w['hw_rms']:.3f} | "
            f"SIM mean={w['sim_mean']:+.3f} rms={w['sim_rms']:.3f} | "
            f"d_mean={w['d_mean']:+.3f} d_rms={w['d_rms']:+.3f}"
        )
    lines.append(
        f"n_windows={report['n_windows']}  mean|resid|={report['mean_abs_residual_c']:.3f} C  "
        f"rms(resid)={report['rms_residual_c']:.3f} C  max|resid|={report['max_abs_residual_c']:.3f} C"
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-plant-sim",
        description=__doc__.splitlines()[0] if __doc__ else "kilnCtl calibrated plant simulator",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_cmp = sub.add_parser("compare", help="simulate a hardware capture's own profile and diff against it")
    p_cmp.add_argument("jsonl_path")
    p_cmp.add_argument("--run", type=int, default=0, help="which run in a multi-run file (default 0)")
    p_cmp.add_argument("--kp", type=float, default=0.06)
    p_cmp.add_argument("--ki", type=float, default=0.0003)
    p_cmp.add_argument("--kd", type=float, default=0.0)
    p_cmp.add_argument("--climb-mode", choices=["coupled", "uncoupled"], default="coupled")
    p_cmp.add_argument("--integral-floor", choices=["ff_hold", "ff_u"], default="ff_hold")
    p_cmp.add_argument("--json", action="store_true")

    p_bv = sub.add_parser("bench-validate", help="check BenchKilnPlant (rig-anchored physical model) against the four bench measurements")
    p_bv.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "compare":
        report = render_sim_vs_capture_report(
            args.jsonl_path, run_idx=args.run, kp=args.kp, ki=args.ki, kd=args.kd,
            climb_mode=args.climb_mode, integral_floor=args.integral_floor,
        )
        if args.json:
            import json
            print(json.dumps(report, indent=2))
        else:
            print(format_sim_vs_capture_report_text(report))
    elif args.cmd == "bench-validate":
        report = bench_validation_report()
        if args.json:
            import json
            print(json.dumps({k: (v.tolist() if hasattr(v, "tolist") else v)
                               for k, v in report.items()}, indent=2))
        else:
            print(format_bench_validation_report_text(report))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
