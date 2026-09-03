"""Simulator-side model of "ramp assist" (§7 of ``firmware/KilnFW/docs/
PID_EXPANSION_PLAN.md``), built to validate the algorithm BEFORE it lands in
firmware. Not wired into any firmware build; this module only drives
``plant_sim``'s calibrated plant models.

WHAT THIS MODELS, and how it maps onto the plan:

  Lag detection (§7.1, ALREADY BUILT in firmware, not re-implemented here):
  a zone is "lagging" (not achieving the commanded ramp rate) when
  ``|actual_c - commanded_target_c| > lag_band_c`` (3.0 C, mirroring
  ``EXEC_RAMP_LOCK_BAND_C``). While lagging, ``commanded_target_c`` for that
  zone simply stops advancing -- the same mechanism the plan says already
  guarantees every ramp endpoint is eventually reached.

  Auto-stretch (§7.2): this module does not add a second mechanism for it.
  The lag-detector's own hold-and-resume behaviour above IS the stretch --
  freezing the commanded target while lagging and resuming once caught up
  produces a schedule that runs at whatever rate the zone can actually
  sustain, arbitrarily extended, while still converging on the exact same
  endpoint. HARD CONSTRAINT, enforced before simulation ever starts: any
  ramp step whose target exceeds ``max_temp_c`` is a hard REFUSAL
  (``RampAssistRefused``), never stretched -- checked against every step of
  the schedule up front, independent of what the plant would have done.

  Dwell credit (§7.3): a heat-work-weighted accumulator
  (``cone_table.heat_work_weight``) that runs ONLY while a zone is lagging
  (the exact "not rising at the desired rate" gate the plan specifies,
  reusing the lag signal rather than a second detector) AND the zone's
  actual temperature has already entered the half-cone-step band below the
  segment's target (``cone_table.band_bottom_c``). Credit accumulates across
  a whole run of consecutive ramp steps (back-to-back ramps do not reset
  it) and is spent -- once -- against the very next dwell step's nominal
  duration: ``effective_dwell_s = max(0.0, nominal_dwell_s - credit_s)``.
  This mirrors the blocker the plan's survey found in ``profile_executor.c``
  (``segment_elapsed_s`` zeroing at the ramp->dwell transition): the credit
  has to be tracked as its own accumulator precisely because that history is
  otherwise discarded.

WHAT THIS CANNOT MODEL (see also the module-level scenario report emitted
by ``run_ramp_assist_scenarios.py``-style callers, and ``plant_sim.py``'s
own TRUST section, which every claim below inherits):

  - Anything above ``plant_sim.EXTRAPOLATION_BOUNDARY_C`` (80 C) runs on
    ``PhysicalKilnPlant`` (or ``load_mass_sweep.LoadedPhysicalKilnPlant``
    for the mass-varied scenarios), whose element wattage, wall
    geometry/insulation and thermal mass are ASSUMED, not measured -- see
    ``plant_sim.py``'s "Physical high-temperature model" section. Any
    conclusion drawn from a cone-scale scenario in this module is
    simulator-only and model-dependent; it is evidence about the mechanism
    (does the algorithm's logic behave correctly), not about a real kiln's
    numbers.
  - Cross-zone coupling above 80 C is itself a damped/capped reuse of the
    bench rig's LOW-temperature coupling shape (``PHYS_COUPLING_FRAC``,
    ``PHYS_COUPLING_SEPARATION_DAMPING``, both ASSUMED) -- see
    ``plant_sim.py`` for the caveats already attached to that number.
  - The cone-table's Ea=300 kJ/mol Arrhenius approximation is itself an
    engineering approximation (see ``cone_table.py``/``cone_table.h``), not
    fit to real vitrification data. Any heat-work number this module reports
    is only as good as that approximation.
  - Whether the credit's heat-work weight tracks an ACTUAL ware load. Per
    the plan's own owner decision (§7, "Load/mass is not measurable"), this
    module's mass-multiplier scenarios use ``load_mass_sweep``'s ASSUMED
    load model (mass scales tau only, coupling scaled independently) -- they
    show whether the *mechanism* still behaves sensibly under a heavier
    load, not what a real loaded kiln's heat work actually is.
  - A real firing. The plan is explicit that a real firing is required
    before the feature's default flips to ON, specifically to check the
    two points above -- this harness is the "simulator can check" half of
    §7.6, not a substitute for the "simulator cannot check" half.
"""
from __future__ import annotations

import dataclasses
from typing import List, Optional, Sequence

import numpy as np

from . import cone_table as ct
from . import plant_sim as ps

# Mirrors EXEC_RAMP_LOCK_BAND_C(zi) (profile_executor_internal.h, 3.0 C for
# every zone) -- see §7.1's "ALREADY BUILT, do not rebuild" note. This is
# the ONLY lag/achievability signal this module uses.
DEFAULT_LAG_BAND_C = 3.0


# ---------------------------------------------------------------------------
# Schedule representation
# ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class RampStep:
    """One commanded ramp segment: move toward ``target_c`` at
    ``rate_c_per_min`` (unsigned magnitude; sign is inferred from the
    direction to ``target_c`` at simulation start, matching how a real
    profile step is authored)."""
    target_c: float
    rate_c_per_min: float


@dataclasses.dataclass(frozen=True)
class DwellStep:
    """One commanded dwell: hold at whatever the previous step's target was
    for ``duration_min`` minutes, before dwell credit is applied."""
    duration_min: float


ScheduleStep = object  # RampStep | DwellStep, kept loose for dataclass frozen unions


class RampAssistRefused(Exception):
    """Raised by ``check_schedule_feasible``/``run_ramp_assist`` when a
    ``RampStep`` targets above ``max_temp_c``. Hard refusal -- §7's owner
    decision is explicit that this case is NEVER auto-stretched, so this
    module refuses before a single simulation tick runs rather than trying
    to simulate an over-temperature target and clamping after the fact."""

    def __init__(self, target_c: float, max_temp_c: float, step_index: int):
        super().__init__(
            f"schedule step {step_index}: target {target_c:.1f} C exceeds "
            f"max_temp_c {max_temp_c:.1f} C -- refused, not stretched"
        )
        self.target_c = target_c
        self.max_temp_c = max_temp_c
        self.step_index = step_index


def check_schedule_feasible(schedule: Sequence[ScheduleStep], max_temp_c: float) -> None:
    """Raises ``RampAssistRefused`` on the first ``RampStep`` whose target
    exceeds ``max_temp_c``. Pure precondition check, no plant/PID involved
    -- the hard constraint applies regardless of whether the kiln could
    physically have gotten there."""
    for i, step in enumerate(schedule):
        if isinstance(step, RampStep) and step.target_c > max_temp_c:
            raise RampAssistRefused(step.target_c, max_temp_c, i)


# ---------------------------------------------------------------------------
# Per-zone ramp-assist state machine
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class _ZoneState:
    seg_idx: int = 0
    commanded_c: float = 0.0
    lagging: bool = False
    credit_s: float = 0.0
    dwell_remaining_s: Optional[float] = None  # None until a dwell step is entered
    done: bool = False
    # accounting, for the scenario report
    stretched_s: float = 0.0        # total extra ramp time spent lagging
    credit_applied_s: float = 0.0   # total credit actually spent reducing a dwell
    heat_work_s: float = 0.0        # Sigma weight*dt over the whole run (see run docstring)
    dwell_heat_work_s: float = 0.0  # Sigma weight*dt over DwellStep ticks ONLY (see
                                     # dwell_parity_report -- isolates the dwell-window
                                     # budget from the ramp's own workload)


def _step_direction(start_c: float, target_c: float) -> float:
    if target_c > start_c:
        return 1.0
    if target_c < start_c:
        return -1.0
    return 0.0


def run_ramp_assist(schedule: Sequence[ScheduleStep], max_temp_c: float,
                     start_temp_c: float = 20.0, ambient: float = 20.0,
                     plant_regime: str = 'measured', mass_mult: float = 1.0,
                     coupling_mult: float = 1.0, lag_band_c: float = DEFAULT_LAG_BAND_C,
                     apply_dwell_credit: bool = True,
                     kp: float = 0.06, ki: float = 0.0003, kd: float = 0.0,
                     dt: Optional[float] = None, max_sim_s: float = 12.0 * 3600.0) -> dict:
    """Drives ``schedule`` (a list of ``RampStep``/``DwellStep``, same
    schedule replicated across all ``N_ZONES``, matching every existing
    profile scenario in this codebase) against a calibrated plant, applying
    the ramp-assist algorithm per zone.

    ``apply_dwell_credit=False`` runs the identical schedule/lag mechanics
    but always uses each dwell's full nominal duration (credit computed,
    never spent) -- this is the "assist OFF" reference run used to quantify
    what the credit does to accumulated heat work (see
    ``compare_heat_work``).

    ``plant_regime``: 'measured' (bench FOPDT, valid to ~80 C) or
    'physical' (ASSUMED cone-10-scale kiln, >80 C territory -- see this
    module's docstring for what that model can and cannot prove).
    ``mass_mult``/``coupling_mult`` are passed straight to
    ``load_mass_sweep``'s load model when != 1.0 (0 extra plant code here;
    see that module for what they mean).

    Raises ``RampAssistRefused`` up front if any step targets above
    ``max_temp_c`` -- see ``check_schedule_feasible``.

    Returns a dict of time series (``t``, ``target`` per zone, ``temp`` per
    zone, ``duty`` per zone, ``lagging`` per zone) plus per-zone summary
    stats (``stretched_s``, ``credit_applied_s``, ``heat_work_s``,
    ``dwell_heat_work_s`` -- heat work accumulated during DwellStep ticks
    only, used by ``dwell_credit_parity`` to isolate the dwell's own
    workload from the ramp's -- and ``dwell_nominal_s``) and
    ``targets_reached`` (bool, per zone -- proof every ramp endpoint was
    actually hit, not just commanded).
    """
    check_schedule_feasible(schedule, max_temp_c)
    if not schedule:
        raise ValueError("schedule must have at least one step")

    dt = ps.DT if dt is None else dt
    n = ps.N_ZONES

    if mass_mult != 1.0 or coupling_mult != 1.0:
        from . import load_mass_sweep as lms
        K_loaded, tau_loaded = lms.loaded_K_tau(mass_mult, coupling_mult)
    else:
        K_loaded, tau_loaded = ps.K_full, ps.tau

    if plant_regime == 'physical':
        if mass_mult != 1.0 or coupling_mult != 1.0:
            from . import load_mass_sweep as lms
            plant = lms.LoadedPhysicalKilnPlant(
                dt, ambient=ambient, start_temp=[start_temp_c] * n,
                mass_mult=mass_mult, coupling_mult=coupling_mult)
        else:
            plant = ps.PhysicalKilnPlant(dt, ambient=ambient, start_temp=[start_temp_c] * n)
    elif plant_regime == 'measured':
        plant = ps.FOPDTPlant(K_loaded, tau_loaded, ps.L, dt, ambient=ambient,
                               start_temp=[start_temp_c] * n)
    else:
        raise ValueError(f"unknown plant_regime {plant_regime!r}")

    # Controller feedforward always uses the FIXED bench-identified matrix,
    # exactly like real firmware (no load sensor) -- see load_mass_sweep.py
    # sec 3. Kept local (not ps.coupled_ff_hold_climb) because that helper
    # assumes one shared target_c across all zones; here each zone can carry
    # its own commanded target once ramp-lock desyncs them.
    K_inv = np.linalg.inv(ps.K_full)

    pids = [ps.PID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0) for _ in range(n)]
    zones = [_ZoneState(commanded_c=start_temp_c) for _ in range(n)]

    # Per-zone "entry point" of the current ramp step, needed to know the
    # sign/direction of travel and how far the commanded value has to move.
    seg_start_c = [start_temp_c] * n

    times: List[float] = []
    target_log: List[np.ndarray] = []
    temp_log: List[np.ndarray] = []
    duty_log: List[np.ndarray] = []
    lag_log: List[np.ndarray] = []

    duty = np.zeros(n)
    t = 0.0

    def zone_active_target_c(zi: int) -> float:
        """The temperature ``cone_table``'s heat-work weight should be
        evaluated against right now for zone ``zi``: the CURRENT step's
        target if ramping, or the dwell's held target if dwelling."""
        z = zones[zi]
        idx = min(z.seg_idx, len(schedule) - 1)
        step = schedule[idx]
        if isinstance(step, RampStep):
            return step.target_c
        return seg_start_c[zi]  # dwell target == the temp it was holding at entry

    while not all(z.done for z in zones) and t <= max_sim_s:
        target_row = np.empty(n)
        rate_row = np.zeros(n)

        for zi in range(n):
            z = zones[zi]
            if z.done:
                target_row[zi] = z.commanded_c
                continue
            step = schedule[z.seg_idx]
            actual_c = plant.temp[zi]

            if isinstance(step, RampStep):
                direction = _step_direction(seg_start_c[zi], step.target_c)
                rate_c_per_s = direction * step.rate_c_per_min / 60.0
                z.lagging = abs(actual_c - z.commanded_c) > lag_band_c
                if z.lagging:
                    z.stretched_s += dt
                else:
                    z.commanded_c += rate_c_per_s * dt
                    if (direction > 0 and z.commanded_c > step.target_c) or \
                       (direction < 0 and z.commanded_c < step.target_c):
                        z.commanded_c = step.target_c

                # Dwell credit gate (§7.3): only while lagging AND within
                # the half-cone-step band below THIS step's own target.
                try:
                    band_bottom = ct.band_bottom_c(step.target_c)
                    in_band = band_bottom <= actual_c < step.target_c
                except ct.ConeTableError:
                    in_band = False  # target outside the cone table's range -- no credit, no crash
                if z.lagging and in_band:
                    try:
                        w = ct.heat_work_weight(actual_c, step.target_c)
                    except ct.ConeTableError:
                        w = 0.0
                    z.credit_s += w * dt

                reached = (direction >= 0 and z.commanded_c >= step.target_c) or \
                          (direction < 0 and z.commanded_c <= step.target_c) or direction == 0.0
                if reached:
                    z.commanded_c = step.target_c
                    z.seg_idx += 1
                    if z.seg_idx >= len(schedule):
                        z.done = True
                    else:
                        next_step = schedule[z.seg_idx]
                        seg_start_c[zi] = step.target_c
                        if isinstance(next_step, DwellStep):
                            nominal_s = next_step.duration_min * 60.0
                            spend = z.credit_s if apply_dwell_credit else 0.0
                            spend = min(spend, nominal_s)
                            z.dwell_remaining_s = nominal_s - spend
                            z.credit_applied_s += spend
                            z.credit_s = 0.0
                target_row[zi] = z.commanded_c
                rate_row[zi] = rate_c_per_s if z.lagging is False else 0.0

            elif isinstance(step, DwellStep):
                z.lagging = False
                z.commanded_c = seg_start_c[zi]
                assert z.dwell_remaining_s is not None
                z.dwell_remaining_s -= dt
                if z.dwell_remaining_s <= 0.0:
                    z.seg_idx += 1
                    if z.seg_idx >= len(schedule):
                        z.done = True
                    else:
                        seg_start_c[zi] = z.commanded_c
                        z.dwell_remaining_s = None
                target_row[zi] = z.commanded_c
                rate_row[zi] = 0.0
            else:
                raise TypeError(f"unknown schedule step type {type(step)!r}")

            # Heat-work accounting over the WHOLE run (both ramp and dwell
            # ticks), evaluated against whatever this zone's currently
            # active target is -- this is the number ``compare_heat_work``
            # totals to answer "does the credit under/over-fire".
            if not z.done:
                try:
                    tick_hw = ct.heat_work_weight(actual_c, zone_active_target_c(zi)) * dt
                except ct.ConeTableError:
                    tick_hw = None  # outside the cone table's covered range -- not counted, not fatal
                if tick_hw is not None:
                    z.heat_work_s += tick_hw
                    if isinstance(step, DwellStep):
                        z.dwell_heat_work_s += tick_hw

        meas = plant.temp.copy()
        # Solve the coupled hold/climb jointly, per-zone independent
        # targets. Controller feedforward always uses the FIXED
        # bench-identified K_full/tau -- never tau_loaded -- matching real
        # firmware, which has no load sensor and cannot see mass_mult (see
        # this function's own docstring, "controller feedforward always
        # uses the FIXED bench-identified matrix").
        rhs_hold = target_row - ambient
        hold_duty = K_inv @ rhs_hold
        rhs_climb = ps.tau * rate_row
        climb_duty = K_inv @ rhs_climb
        for zi in range(n):
            total = hold_duty[zi] + climb_duty[zi]
            total_clamped = min(max(total, 0.0), 1.0)
            hold_out = min(max(hold_duty[zi], 0.0), 1.0)
            duty[zi], _ = pids[zi].update(target_row[zi], meas[zi], dt, total_clamped, hold_out)

        times.append(t)
        target_log.append(target_row.copy())
        temp_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        lag_log.append(np.array([z.lagging for z in zones]))

        plant.step(duty)
        t += dt

    targets_reached = []
    for zi in range(n):
        final_step = schedule[-1]
        final_target = final_step.target_c if isinstance(final_step, RampStep) else seg_start_c[zi]
        targets_reached.append(bool(zones[zi].done))

    dwell_nominal_s = [s.duration_min * 60.0 for s in schedule if isinstance(s, DwellStep)]

    return dict(
        t=np.array(times), target=np.array(target_log), temp=np.array(temp_log),
        duty=np.array(duty_log), lagging=np.array(lag_log),
        stretched_s=[z.stretched_s for z in zones],
        credit_applied_s=[z.credit_applied_s for z in zones],
        heat_work_s=[z.heat_work_s for z in zones],
        dwell_heat_work_s=[z.dwell_heat_work_s for z in zones],
        targets_reached=targets_reached,
        dwell_nominal_s=dwell_nominal_s,
        wall_clock_s=t,
        plant_regime=plant_regime, mass_mult=mass_mult, coupling_mult=coupling_mult,
        apply_dwell_credit=apply_dwell_credit,
    )


def compare_heat_work(schedule: Sequence[ScheduleStep], max_temp_c: float, **kwargs) -> dict:
    """Runs ``schedule`` twice -- once with dwell credit applied, once
    without -- and reports total accumulated heat-work (weight-seconds, per
    zone) for the WHOLE run (ramp + dwell), plus the delta.

    CAUTION -- this metric is NOT a valid under/over-fire measurement on its
    own, and using it that way produced a misleading "4.5-16% under-fire"
    read during this module's own validation pass (see
    ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` §7.3 "credit unit
    investigation" for the full writeup). The ramp phase is bit-for-bit
    identical between the two runs (``apply_dwell_credit`` only changes the
    ``dwell_remaining_s`` computed at the ramp->dwell transition, nothing
    upstream of it), so whole-run heat work necessarily differs from the
    unassisted run by close to ``-credit_applied_s`` REGARDLESS of what unit
    the credit was banked or spent in -- shortening a period spent near 1.0
    weight by C seconds always removes close to C weight-seconds of total
    run heat work; there is nothing for the credited ramp-tail work to
    "give back" here because that ramp-tail work is already counted
    equally in both runs' totals. Kept for back-compat / whole-run
    bookkeeping; use ``dwell_credit_parity`` to actually answer "does the
    credit under/over-fire the dwell".
    """
    kwargs.pop('apply_dwell_credit', None)
    assisted = run_ramp_assist(schedule, max_temp_c, apply_dwell_credit=True, **kwargs)
    unassisted = run_ramp_assist(schedule, max_temp_c, apply_dwell_credit=False, **kwargs)
    n = ps.N_ZONES
    delta = [assisted['heat_work_s'][i] - unassisted['heat_work_s'][i] for i in range(n)]
    pct = [
        (100.0 * delta[i] / unassisted['heat_work_s'][i]) if unassisted['heat_work_s'][i] > 0 else float('nan')
        for i in range(n)
    ]
    return dict(assisted=assisted, unassisted=unassisted, delta_heat_work_s=delta,
                delta_heat_work_pct=pct)


def dwell_credit_parity(schedule: Sequence[ScheduleStep], max_temp_c: float, **kwargs) -> dict:
    """The metric that actually answers "does dwell credit under/over-fire":
    isolates the DWELL-WINDOW heat-work budget from the ramp's own workload,
    instead of comparing whole-run totals (see ``compare_heat_work``'s
    caution note for why that comparison is not valid here).

    Runs ``schedule`` twice (credit applied / not applied -- ramp phase is
    identical in both, only the dwell's spent duration differs) and reports,
    per zone:

      ``credit_s``            -- W, the heat-work (weight*dt) banked while
                                  lagging AND in-band during the ramp tail,
                                  and the exact number of SECONDS spent off
                                  the following dwell (``credit_applied_s``
                                  from the assisted run -- this module banks
                                  and spends the same unit, see the module
                                  docstring's §7.3 note).
      ``dwell_heat_work_assisted_s``   -- heat work actually accumulated
                                  during the shortened dwell.
      ``dwell_heat_work_unassisted_s`` -- heat work actually accumulated
                                  during the FULL nominal dwell, credit off.
                                  This is the real achievable baseline --
                                  NOT ``dwell_nominal_s``, because the plant
                                  is often still catching up to target when
                                  the dwell begins (see
                                  ``catchup_deficit_s`` below), so even an
                                  uncredited dwell does not deliver
                                  ``dwell_nominal_s`` of heat work.
      ``recipe_total_s``       -- ``credit_s + dwell_heat_work_assisted_s``,
                                  i.e. what the credited ramp tail plus the
                                  shortened dwell actually delivered toward
                                  the dwell's workload.
      ``parity_vs_unassisted_pct`` -- ``recipe_total_s`` against
                                  ``dwell_heat_work_unassisted_s`` (the
                                  achievable baseline) -- the real
                                  under/over-fire number. Near 0 means the
                                  credit mechanism is unit-consistent.
      ``parity_vs_nominal_pct`` -- ``recipe_total_s`` against
                                  ``dwell_nominal_s`` (the idealized
                                  recipe-book target) -- always shows a
                                  negative residual whenever
                                  ``catchup_deficit_s`` > 0, WITH OR WITHOUT
                                  ramp assist (see the zero-credit sanity
                                  check in this module's tests); do not
                                  mistake this for a credit-accounting bug.
      ``catchup_deficit_s``    -- ``dwell_nominal_s -
                                  dwell_heat_work_unassisted_s``: how much
                                  heat work the plant's own catch-up lag at
                                  dwell entry costs, independent of ramp
                                  assist entirely (measurable with
                                  ``apply_dwell_credit`` irrelevant since
                                  it's the SAME in the unassisted run, which
                                  never shortens anything).
    """
    kwargs.pop('apply_dwell_credit', None)
    assisted = run_ramp_assist(schedule, max_temp_c, apply_dwell_credit=True, **kwargs)
    unassisted = run_ramp_assist(schedule, max_temp_c, apply_dwell_credit=False, **kwargs)
    n = ps.N_ZONES
    dwell_nominal_s = sum(assisted['dwell_nominal_s'])
    out = []
    for zi in range(n):
        credit_s = assisted['credit_applied_s'][zi]
        dhw_a = assisted['dwell_heat_work_s'][zi]
        dhw_u = unassisted['dwell_heat_work_s'][zi]
        recipe_total_s = credit_s + dhw_a
        parity_vs_unassisted_pct = (
            100.0 * (recipe_total_s - dhw_u) / dhw_u if dhw_u > 0 else float('nan'))
        parity_vs_nominal_pct = (
            100.0 * (recipe_total_s - dwell_nominal_s) / dwell_nominal_s
            if dwell_nominal_s > 0 else float('nan'))
        catchup_deficit_s = dwell_nominal_s - dhw_u
        out.append(dict(
            credit_s=credit_s, dwell_heat_work_assisted_s=dhw_a,
            dwell_heat_work_unassisted_s=dhw_u, dwell_nominal_s=dwell_nominal_s,
            recipe_total_s=recipe_total_s,
            parity_vs_unassisted_pct=parity_vs_unassisted_pct,
            parity_vs_nominal_pct=parity_vs_nominal_pct,
            catchup_deficit_s=catchup_deficit_s,
        ))
    return dict(per_zone=out, assisted=assisted, unassisted=unassisted)
