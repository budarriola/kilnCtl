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

Not modeled: PWM window quantization (``heater_output.c``) -- averages out
under 10 s sampling and is not implicated by any of the five captures'
error shape. Thermocouple resolution is not separately modeled either; the
five captures are already hardware-quantized, and this module's own output
is compared against them at their native 10 s cadence, never resampled or
smoothed to hide quantization on either side (see
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

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        own_power = PHYS_P_MAX_W * duty
        growth = coupling_growth(self.temp)
        coupling_power = (PHYS_COUPLING_FRAC * own_power.reshape(1, -1)).sum(axis=1) * growth
        loss = physical_loss_w(self.temp, self.ambient)
        net_w = own_power + coupling_power - loss
        dTdt = net_w / PHYS_THERMAL_MASS_J_PER_K
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


# ---------------------------------------------------------------------------
# Fuzzy-PID layer -- FAITHFUL mirror of firmware/KilnFW/App/drivers/
# pid_fuzzy.c's pid_fuzzy_adjust(), not an approximation. Constants, the 3x3
# rule table, the triangular-membership shape and the strength_pct==0
# short-circuit are copied line-for-line from the C source (read
# 2026-09-02 for this sweep). Any future edit to pid_fuzzy.c must be
# mirrored here too, or this module's PID_FUZZY_STRENGTH_ZERO_INVARIANT
# test (tests/test_plant_sim.py) is the tripwire that should catch the drift
# -- it only proves strength=0 is a no-op in THIS mirror, so a firmware
# change that only affects strength>0 behaviour would not be caught by it.
FUZZY_ERROR_BAND_C = 20.0
FUZZY_RATE_BAND_C_PER_S = 0.5
FUZZY_MAX_NUDGE_FRACTION = 0.5

# rule table [error_bucket][rate_bucket] -> (kp_dir, ki_dir, kd_dir),
# bucket order 0=NEG/FALLING, 1=ZERO/STEADY, 2=POS/RISING -- copied from
# pid_fuzzy.c's RULE_TABLE.
FUZZY_RULE_TABLE = [
    [(1.0, -1.0, 1.0), (1.0, 0.0, 0.0), (-1.0, 1.0, -1.0)],
    [(-1.0, -1.0, 1.0), (-1.0, 1.0, -1.0), (1.0, -1.0, 1.0)],
    [(-1.0, 1.0, -1.0), (1.0, 0.0, 0.0), (1.0, -1.0, 1.0)],
]


def _fuzzy_triangular_memberships(x, band):
    """Mirrors pid_fuzzy.c's triangular_memberships(): returns
    (neg, zero, pos), each in [0,1], summing to exactly 1.0."""
    if x <= -band:
        return 1.0, 0.0, 0.0
    if x >= band:
        return 0.0, 0.0, 1.0
    if x <= 0.0:
        t = (-x) / band
        return t, 1.0 - t, 0.0
    t = x / band
    return 0.0, 1.0 - t, t


def _fuzzy_clamp_gain(g):
    if not math.isfinite(g) or g < 0.0:
        return 0.0
    return g


def _fuzzy_sanitize_base(base):
    if not math.isfinite(base) or base < 0.0:
        return 0.0
    return base


def pid_fuzzy_adjust(error_c, error_rate_c_per_s, base_kp, base_ki, base_kd, strength_pct):
    """Python mirror of pid_fuzzy.c's ``pid_fuzzy_adjust()``. ``strength_pct``
    is a float here (the sim has no uint8_t rounding step) but is clamped to
    [0, 100] exactly as the C caller (profile_executor_pid_tick.c) clamps
    before the uint8_t cast -- strength_pct == 0 is the safety-contract
    short-circuit and MUST reproduce the base gains bit-for-bit (see
    pid_fuzzy.c's own comment on that short-circuit)."""
    kp = _fuzzy_sanitize_base(base_kp)
    ki = _fuzzy_sanitize_base(base_ki)
    kd = _fuzzy_sanitize_base(base_kd)

    if strength_pct > 100.0:
        strength_pct = 100.0
    if strength_pct < 0.0:
        strength_pct = 0.0

    if strength_pct == 0.0:
        return kp, ki, kd

    if not math.isfinite(error_c) or not math.isfinite(error_rate_c_per_s):
        return kp, ki, kd

    e_neg, e_zero, e_pos = _fuzzy_triangular_memberships(error_c, FUZZY_ERROR_BAND_C)
    r_neg, r_zero, r_pos = _fuzzy_triangular_memberships(error_rate_c_per_s, FUZZY_RATE_BAND_C_PER_S)
    e_deg = (e_neg, e_zero, e_pos)
    r_deg = (r_neg, r_zero, r_pos)

    kp_sum = ki_sum = kd_sum = weight_sum = 0.0
    for ei in range(3):
        for ri in range(3):
            firing = e_deg[ei] * r_deg[ri]
            kp_dir, ki_dir, kd_dir = FUZZY_RULE_TABLE[ei][ri]
            kp_sum += firing * kp_dir
            ki_sum += firing * ki_dir
            kd_sum += firing * kd_dir
            weight_sum += firing

    kp_dir = (kp_sum / weight_sum) if weight_sum > 0.0 else 0.0
    ki_dir = (ki_sum / weight_sum) if weight_sum > 0.0 else 0.0
    kd_dir = (kd_sum / weight_sum) if weight_sum > 0.0 else 0.0

    scale = (strength_pct / 100.0) * FUZZY_MAX_NUDGE_FRACTION

    out_kp = _fuzzy_clamp_gain(kp * (1.0 + scale * kp_dir))
    out_ki = _fuzzy_clamp_gain(ki * (1.0 + scale * ki_dir))
    out_kd = _fuzzy_clamp_gain(kd * (1.0 + scale * kd_dir))
    return out_kp, out_ki, out_kd


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
    accumulator via pid_rescale_integral_for_new_ki() before it is used."""

    def __init__(self, kp, ki, kd, d_tau, b, pid_range_c, fuzzy_strength_pct=0.0):
        self.base_kp, self.base_ki, self.base_kd = kp, ki, kd
        self.kp, self.ki, self.kd = kp, ki, kd
        self.d_tau, self.b, self.pid_range_c = d_tau, b, pid_range_c
        self.fuzzy_strength_pct = fuzzy_strength_pct
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


def run_profile(segs, start_temp, kp=0.06, ki=0.0003, kd=0.0,
                 climb_mode='coupled', integral_floor='ff_hold', ambient=20.0,
                 controller_K_inv=None, controller_tau=None, plant_regime='measured',
                 fuzzy_strength_pct=0.0,
                 measurement_quantum_c=0.0, measurement_noise_std_c=0.0, measurement_seed=0):
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

    ``fuzzy_strength_pct``: 0-100, applied uniformly to all three zones'
    ``PID`` instances -- see ``PID``'s docstring and ``pid_fuzzy_adjust()``
    above for the faithful mirror of ``pid_fuzzy.c``. Default 0.0 keeps
    every existing caller (regression tests, capture comparisons, the
    matrix sweep) on exactly the pre-fuzzy code path.

    ``measurement_quantum_c``/``measurement_noise_std_c``: the PID has
    always been fed ``plant.temp[i]`` directly -- the true, noise-free
    plant state -- with no model of the real measurement chain (MAX31856
    thermocouple ADC, 0.1 C LSB quantization) or of thermocouple noise. The
    fuzzy layer's whole design target (see ``pid_fuzzy_adjust``) is
    rejecting noisy-derivative behaviour that this omission cannot
    reproduce at all: a deterministic measurement can never exercise the
    fuzzy dead-band, so any fuzzy-vs-baseline comparison run through this
    simulator up to 2026-09-02 was necessarily comparing on a signal the
    fuzzy layer was not built to react to. Both default to ``0.0``
    (off, byte-for-byte the old code path) so every existing caller
    (regression tests, capture comparisons, the matrix sweep) is
    unaffected. ``measurement_seed`` seeds the noise draw so a given call
    is reproducible; quantization rounds to the nearest
    ``measurement_quantum_c`` (0.1 for the real MAX31856 LSB) and is
    applied AFTER the additive Gaussian noise, matching the real chain
    (continuous sensor + noise, then ADC quantization).
    """
    if plant_regime == 'physical':
        plant = PhysicalKilnPlant(DT, ambient=ambient, start_temp=start_temp)
    elif plant_regime == 'measured':
        plant = FOPDTPlant(K_full, tau, L, DT, ambient=ambient, start_temp=start_temp)
    else:
        raise ValueError(f"unknown plant_regime {plant_regime!r}, expected 'measured' or 'physical'")
    pids = [PID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0,
                fuzzy_strength_pct=fuzzy_strength_pct) for _ in range(N_ZONES)]
    ff_fn = coupled_ff_hold_climb if climb_mode == 'coupled' else uncoupled_ff_hold_climb

    total_t = segs[-1][1]
    times, targets, temps_log, duty_log = [], [], [], []
    duty = np.zeros(N_ZONES)
    t = 0.0
    rng = np.random.default_rng(measurement_seed) if measurement_noise_std_c > 0.0 else None
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
        for i in range(N_ZONES):
            if climb_mode == 'coupled':
                hold, climb, ff = ff_fn(target_c, target_rate, i, ambient=ambient,
                                         K_inv=controller_K_inv, tau_ff=controller_tau)
            else:
                hold, climb, ff = ff_fn(target_c, target_rate, i, ambient=ambient)
            meas_c = plant.temp[i]
            if rng is not None:
                meas_c = meas_c + rng.normal(0.0, measurement_noise_std_c)
            if measurement_quantum_c > 0.0:
                meas_c = round(meas_c / measurement_quantum_c) * measurement_quantum_c
            duty[i], _ = pids[i].update(target_c, meas_c, DT, ff, hold, integral_floor=integral_floor)
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        plant.step(duty)
        t += DT

    targets_arr = np.array(targets)
    max_target = float(targets_arr.max()) if len(targets_arr) else float(ambient)
    return dict(t=np.array(times), target=targets_arr,
                temps=np.array(temps_log), duty=np.array(duty_log),
                seg_bounds=[s[1] for s in segs],
                max_target_c=max_target,
                extrapolation=is_extrapolation(max_target),
                plant_regime=plant_regime)


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
