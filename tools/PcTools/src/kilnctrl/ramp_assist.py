"""Simulator-side model of "ramp assist" (§7 of ``firmware/KilnFW/docs/
PID_EXPANSION_PLAN.md``), built to validate the algorithm BEFORE it lands in
firmware. Not wired into any firmware build; this module only drives
``plant_sim``'s calibrated plant models.

WHAT THIS MODELS, and how it maps onto the plan:

  Lag detection (§7.1, ALREADY BUILT in firmware, not re-implemented here):
  a zone is "lagging" (not achieving the commanded ramp rate) when
  ``(commanded_target_c - actual_c) > lag_band_c`` (25.0 C, mirroring
  ``PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`` -- see ``DEFAULT_LAG_BAND_C``'s own
  comment for the cross-language pin that now enforces this against
  ``profile_executor.h`` directly, and the "MIRROR BUG" note below for how
  this was wrong for a while). ONE-SIDED, not ``abs(...)``: only a zone
  COLDER than commanded by more than the band locks (firmware commit
  8f12449, "Fix ramp-lock hot-start stall: one-sided lock + guard 4 arming
  backstop" -- the old symmetric form froze the whole shared setpoint
  indefinitely on a zone that starts a firing already hot). While lagging,
  ``commanded_target_c`` for that zone simply stops advancing -- the same
  mechanism the plan says already guarantees every ramp endpoint is
  eventually reached.

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
  (``cone_table.heat_work_weight``) that runs ONLY while a zone is BEHIND
  SCHEDULE AT ALL (``actual_c < z.commanded_c``, the moving setpoint --
  deliberately NOT the ``lag_band_c``-gated ``z.lagging`` used for the
  ramp-lock/stretch accounting above) AND the zone's actual temperature has
  already entered the half-cone-step band below the segment's target.
  Fixed 2026-09-03: this used to reuse ``z.lagging`` (i.e. the 25 C
  ``lag_band_c`` lock), which made the credit gate and the in-band
  condition mutually exclusive at every real cone-scale target -- during a
  ramp ``z.commanded_c <= step.target_c``, so a lock-lagging zone is always
  >25 C below ``step.target_c``, while in-band requires within half a cone
  step (well under 25 C almost everywhere in the Orton table) -- see
  ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` §7.3/§7.6 for the
  measured-zero finding this produced and the fix. ``lag_band_c`` still
  governs ramp-lock/stretch (``z.lagging``, ``z.stretched_s``) exactly as
  before; it no longer has any bearing on dwell credit. Credit accumulates
  across a whole run of consecutive
  ramp steps (back-to-back ramps do not reset it) and is spent -- once --
  against the very next dwell step's nominal duration:
  ``effective_dwell_s = max(0.0, nominal_dwell_s - credit_s)``. This
  mirrors the blocker the plan's survey found in ``profile_executor.c``
  (``segment_elapsed_s`` zeroing at the ramp->dwell transition): the credit
  has to be tracked as its own accumulator precisely because that history is
  otherwise discarded. Dwell state (including the credit spend) is
  initialised LAZILY, on a ``DwellStep``'s own first tick, rather than only
  at a ramp->dwell transition -- see ``_ZoneState.dwell_remaining_s`` and
  the DEFECT 3 note at its use site below: a schedule can open with a dwell
  (a candling hold) or chain two dwells back to back, and neither of those
  is reached via a ramp->dwell transition.

  Extended past the nominal ramp end (2026-09-03, "extend dwell-credit
  accrual past the nominal ramp end"): the RampStep's own freeze-and-resume
  (§7.2 above) releases once a lagging zone is back within the WIDE
  ``lag_band_c`` (25.0 C) -- but the credit band above is much NARROWER
  (half a cone step). Under heavy thermal mass a zone routinely un-locks
  (ending the ramp step, ``z.seg_idx`` moving on to the following
  ``DwellStep``) while still outside the credit band, and only crosses into
  it after the dwell has already begun -- gating credit on "still inside a
  ``RampStep``" alone made that catch-up window uncreditable (measured:
  exactly 0.0 s of credit at 2x/4x mass for bisque/cone 6, 4x for cone 10).
  Credit now ALSO accrues on ``DwellStep`` ticks, using the same behind-
  schedule/in-band test against ``seg_start_c[zi]`` (the dwell's own held
  target, i.e. the just-finished ramp's ``step.target_c``) -- see the
  ``DwellStep`` branch below for the accrual itself and the hazard note
  guarding it. THE HAZARD: credit banked during a dwell occurrence must
  never shorten THAT SAME occurrence -- ``z.dwell_remaining_s`` is computed
  ONCE, from a frozen ``spend``, on the occurrence's own first tick, and is
  only ever decremented by ``dt`` afterward, never recomputed from
  ``z.credit_s``. Later accrual is only ever spent at the NEXT ``DwellStep``
  occurrence's own first tick.

  Band-width note (DEFECT 2, RESOLVED): ``cone_table.band_bottom_c`` used
  to anchor the credit band's half-width on the distance from the segment
  target down to the nearest tabulated cone BELOW it, not on the local
  cone spacing -- for a target a hair above a tabulated cone that
  collapsed the band to a few thousandths of a degree instead of the
  several degrees the surrounding cone spacing implies. Fixed in
  ``cone_table.c``/``.py`` (commit f84d4c8) to use the bracketing-pair
  half-spacing formula; this module no longer carries its own copy of
  that formula and calls ``cone_table.band_bottom_c`` directly, so the
  simulator and the firmware now provably share one band-width formula.

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

# Mirrors PROFILE_EXECUTOR_RAMP_LOCK_BAND_C (profile_executor.h), the
# fallback EXEC_RAMP_LOCK_BAND_C(zi)/exec_threshold(zi, 3) (profile_executor_
# pid_tick.c) returns whenever no per-zone override is configured -- true for
# every shipped config; the only assignments in the tree are in
# test_zones_http.c. See §7.1's "ALREADY BUILT, do not rebuild" note. This is
# the ONLY lag/achievability signal this module uses.
#
# MIRROR BUG (found 2026-09, fixed here): this constant was hard-coded to
# 3.0 C -- actually PROGRESS_BAND_C, a DIFFERENT constant (guard 1's arrival
# band, thermal_guard.c), confused for this one -- making the simulator
# 25.0/3.0 = 8.3x more sensitive to lag than the firmware it claims to
# mirror. That 8.3x-too-tight band produced a plausible-looking false alarm
# in commit a19c1c4 (~1/3 of zone-scenarios reported as never reaching
# dwell, attributed to a cross-zone coupling deadlock) that an adversarial
# review showed is arithmetically impossible at the real 25 C band on this
# kiln's coupling matrix. See PID_EXPANSION_PLAN.md §7.6 for the corrected
# cone-scale numbers. ``test_ramp_assist.py``'s
# ``RampAssistLagBandCrossLanguageTest`` now parses
# ``PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`` out of profile_executor.h directly
# and asserts this constant matches it, so this specific mirror cannot go
# stale silently again -- though a hand-copied number for a DIFFERENT
# firmware constant, as happened here, is exactly the failure mode that
# check cannot see; only a wrong human-written comment ever claimed the
# mirror, so read this comment against the pin test, not the other way
# round.
DEFAULT_LAG_BAND_C = 25.0


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
    credit_reference_heat_s: float = 0.0   # independent audit accumulator for the same accrual
                                            # gate/formula, tracked on its OWN statement so a
                                            # regression in the credit_s accrual line specifically
                                            # does not also corrupt this one -- see credit_audit_pct.
                                            # MUST be reset in lockstep with credit_s (2026-09-03,
                                            # sec 7.3 extension) -- see credit_reference_applied_s.
    credit_reference_applied_s: float = 0.0  # running total of credit_reference_heat_s actually
                                              # "spent" (mirrors credit_applied_s's own accumulation,
                                              # from the independent accumulator) -- see the
                                              # credit_audit_pct note below for why this, not the
                                              # raw whole-run credit_reference_heat_s, is what
                                              # dwell_credit_parity() must compare credit_applied_s
                                              # against once accrual can outlive a dwell it can
                                              # never be spent against (a terminal dwell with no
                                              # later dwell occurrence in the schedule).
    heat_work_s: float = 0.0        # Sigma weight*dt over the whole run (see run docstring)
    dwell_heat_work_s: float = 0.0  # Sigma weight*dt over DwellStep ticks ONLY (see
                                     # dwell_credit_parity -- isolates the dwell-window
                                     # budget from the ramp's own workload)

    # --- window-parity bookkeeping (DEFECT 1 fix; see dwell_credit_parity) ---
    # A "window" is FIXED-LENGTH -- exactly that dwell's own
    # ``dwell_nominal_s`` of real wall-clock time -- starting at the
    # dwell's own entry (NOT at band-entry during the preceding ramp tail:
    # a heavily-lagging ramp tail can itself run longer than the dwell's
    # nominal duration, which would make a band-entry-anchored window
    # close before the dwell even starts). The length is deliberately NOT
    # ``nominal_s - credit_s`` (the shortened/assisted duration): if it
    # were, a wrong credit would shrink the window right along with the
    # dwell it shortens, and "heat delivered in the window" vs "window
    # length" would cancel out almost exactly regardless of whether the
    # credit was right -- precisely the failure mode DEFECT 1 documents.
    # Keeping the window's length fixed at the FULL nominal duration means
    # that when credit is wrong, the window runs past the (incorrectly)
    # early-shortened dwell into whatever real ticks come after it --
    # ticks the credit spent but did not actually earn -- and those ticks'
    # real heat work (evaluated against the DWELL's own target, even if
    # the schedule has already moved the zone toward something else) shows
    # up directly in the comparison.
    window_end_t: Optional[float] = None    # wall-clock end of the pending window, None if none open
    window_nominal_s: float = 0.0           # fixed length of the pending window (that dwell's own
                                             # nominal_s, set once when the window opens)
    window_target_c: Optional[float] = None  # temperature the pending window's real heat work is
                                              # evaluated against (the dwell's held target)
    window_pending_heat_s: float = 0.0      # real weight*dt accumulated since the window opened
    window_actual_total_s: float = 0.0  # Sigma real heat work over every CLOSED window
    window_ideal_total_s: float = 0.0   # Sigma window_nominal_s over every CLOSED window (== the
                                         # heat work an ideal, always-at-target zone would have
                                         # delivered over that same fixed-length span)


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

    ``schedule`` may start with a ``DwellStep`` (a leading candling hold)
    or chain two ``DwellStep``s back to back -- dwell state (including any
    credit spend) is initialised lazily on that step's own first tick, not
    only at a ramp->dwell transition (DEFECT 3 fix).

    Returns a dict of time series (``t``, ``target`` per zone, ``temp`` per
    zone, ``duty`` per zone, ``lagging`` per zone) plus per-zone summary
    stats (``stretched_s``, ``credit_applied_s``, ``heat_work_s``,
    ``dwell_heat_work_s`` -- heat work accumulated during DwellStep ticks
    only, used by ``dwell_credit_parity`` to isolate the dwell's own
    workload from the ramp's -- ``window_actual_heat_s``/
    ``window_ideal_heat_s`` -- the DEFECT 1 fixed-window parity bookkeeping,
    see ``_ZoneState`` -- and ``dwell_nominal_s``) and ``targets_reached``
    (bool, per zone -- proof every ramp endpoint was actually hit, not just
    commanded).
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

    def _sim_should_continue() -> bool:
        # DEFECT 1 fix: keep simulating past a zone's "done" point as long
        # as it still has a pending fixed-length window open -- that
        # overrun (real ticks after a credit-shortened dwell ends early,
        # with nothing left scheduled) is exactly what window_parity_pct
        # needs to see. Without this, the sim stops the instant every zone
        # finishes and every pending window gets force-closed near-empty
        # regardless of how wrong its credit was, which would make the
        # metric read a constant, uninformative value no matter the SCALE.
        return any((not z.done) or z.window_end_t is not None for z in zones)

    while _sim_should_continue() and t <= max_sim_s:
        target_row = np.empty(n)
        rate_row = np.zeros(n)

        for zi in range(n):
            z = zones[zi]
            actual_c = plant.temp[zi]

            # DEFECT 1 fix: advance any pending fixed-length window for this
            # zone REGARDLESS of whether it is "done" -- a window opened
            # against a dwell that credit shortens can run past the end of
            # the schedule, and the real heat (or absence of it) during
            # that overrun is exactly what a wrong credit needs to be
            # caught by. Uses window_target_c (the dwell's held target)
            # snapshotted when the window opened, not zone_active_target_c
            # (which would already have moved on).
            if z.window_end_t is not None:
                try:
                    w_win = ct.heat_work_weight(actual_c, z.window_target_c)
                except ct.ConeTableError:
                    w_win = 0.0
                if t < z.window_end_t:
                    z.window_pending_heat_s += w_win * dt
                if t + dt >= z.window_end_t:
                    z.window_actual_total_s += z.window_pending_heat_s
                    z.window_ideal_total_s += z.window_nominal_s
                    z.window_end_t = None
                    z.window_pending_heat_s = 0.0
                    z.window_target_c = None

            if z.done:
                target_row[zi] = z.commanded_c
                continue
            step = schedule[z.seg_idx]

            if isinstance(step, RampStep):
                direction = _step_direction(seg_start_c[zi], step.target_c)
                rate_c_per_s = direction * step.rate_c_per_min / 60.0
                # ONE-SIDED (commit 8f12449, firmware profile_executor.c:383):
                # ``(target_c - actual_c) > band`` -- only a zone COLDER than
                # commanded by more than the band locks; a zone HOTTER than
                # commanded (cross-zone coupling overshoot, or a downward
                # ramp lagging on the cooling side) does NOT lock. Firmware
                # used ``fabsf()`` here (a symmetric ``abs(target-actual) >
                # band``) until 8f12449 fixed a hot-start stall: a zone that
                # starts a firing already hot froze the whole shared
                # setpoint indefinitely, since guards 1/2/7 gate on
                # commanded duty (which a too-hot zone doesn't have) and
                # guard 4 never armed. This module mirrored the OLD
                # symmetric form; fixed here to match.
                z.lagging = (z.commanded_c - actual_c) > lag_band_c
                if z.lagging:
                    z.stretched_s += dt
                else:
                    z.commanded_c += rate_c_per_s * dt
                    if (direction > 0 and z.commanded_c > step.target_c) or \
                       (direction < 0 and z.commanded_c < step.target_c):
                        z.commanded_c = step.target_c

                # Dwell credit gate (§7.3): only while BEHIND SCHEDULE AT ALL
                # (actual_c < the moving z.commanded_c) AND within the
                # half-cone-step band below THIS step's own target.
                # cone_table.band_bottom_c now implements the corrected
                # bracketing-pair half-spacing formula directly (DEFECT 2
                # fixed in cone_table.c/.py, commit f84d4c8) -- ramp_assist
                # used to carry a local duplicate of this formula as a
                # workaround; that duplicate is gone, this is the one and
                # only band-width formula shared by the simulator and the
                # firmware.
                #
                # DELIBERATELY NOT `z.lagging` (mirrors firmware fix
                # 2026-09-03, profile_executor_ramp_assist.c's
                # ramp_assist_dwell_credit_tick()): `z.lagging` is the
                # ramp-lock's wide DEFAULT_LAG_BAND_C=25.0 band, which
                # decides when the schedule freezes and must stay wide.
                # Reusing it here made credit and in_band mutually
                # exclusive -- during a ramp z.commanded_c <= step.target_c,
                # so a lock-lagging zone is always >25C below step.target_c,
                # while in_band requires within half a cone step (<25C
                # almost everywhere in the Orton table) -- credit was
                # measured at EXACTLY ZERO at bisque/cone6/cone10, every
                # mass loading, before this was split out. `behind_schedule`
                # asks a narrower question ("behind at all"), already scoped
                # tight by in_band and the heat-work weight below.
                behind_schedule = actual_c < z.commanded_c
                try:
                    band_bottom = ct.band_bottom_c(step.target_c)
                    in_band = band_bottom <= actual_c < step.target_c
                except ct.ConeTableError:
                    in_band = False  # target outside the cone table's range -- no credit, no crash
                if behind_schedule and in_band:
                    try:
                        w = ct.heat_work_weight(actual_c, step.target_c)
                    except ct.ConeTableError:
                        w = 0.0
                    z.credit_s += w * dt
                    # DEFECT 1 fix: independent audit accumulator -- see
                    # credit_audit_pct in dwell_credit_parity. Tracked on
                    # its own separate statement (not derived from
                    # credit_s) specifically so that a regression confined
                    # to the credit_s accrual line above does not also
                    # corrupt this reference value.
                    try:
                        w_ref = ct.heat_work_weight(actual_c, step.target_c)
                    except ct.ConeTableError:
                        w_ref = 0.0
                    z.credit_reference_heat_s += w_ref * dt

                reached = (direction >= 0 and z.commanded_c >= step.target_c) or \
                          (direction < 0 and z.commanded_c <= step.target_c) or direction == 0.0
                if reached:
                    z.commanded_c = step.target_c
                    z.seg_idx += 1
                    if z.seg_idx >= len(schedule):
                        z.done = True
                    else:
                        seg_start_c[zi] = step.target_c
                        # DEFECT 3 fix: dwell initialisation (credit spend,
                        # dwell_remaining_s) no longer happens here -- it
                        # happens lazily on the DwellStep's own first tick
                        # below, so a leading or back-to-back dwell (no
                        # RampStep immediately before it) is initialised
                        # the same way as one reached from a ramp.
                target_row[zi] = z.commanded_c
                rate_row[zi] = rate_c_per_s if z.lagging is False else 0.0

            elif isinstance(step, DwellStep):
                # DEFECT 3 fix: initialise dwell state lazily, on this
                # dwell occurrence's own first tick, instead of only at a
                # ramp->dwell transition. That transition is not the only
                # way a zone can enter a DwellStep -- a schedule can open
                # with one (a candling hold) or chain two in a row -- and
                # the old code asserted ``dwell_remaining_s is not None``,
                # which raised on both shapes because nothing upstream ever
                # set it. Guarded so this only fires once per occurrence.
                if z.dwell_remaining_s is None:
                    nominal_s = step.duration_min * 60.0
                    spend = z.credit_s if apply_dwell_credit else 0.0
                    # ALSO fix (weaker defect): pin credit-exceeds-dwell
                    # behaviour -- never let a dwell go negative; excess
                    # credit is silently discarded, not carried forward.
                    spend = min(spend, nominal_s)
                    z.dwell_remaining_s = nominal_s - spend
                    z.credit_applied_s += spend
                    z.credit_s = 0.0
                    # Mirror the above in lockstep, from the INDEPENDENT
                    # audit accumulator, so credit_audit_pct keeps comparing
                    # like with like now that accrual can outlive a dwell it
                    # is never spent against (sec 7.3 extension, 2026-09-03):
                    # credit_reference_heat_s must be reset here too, or a
                    # terminal dwell's own in-dwell accrual (which credit_s
                    # legitimately never spends, because there is no LATER
                    # dwell occurrence left in the schedule to spend it
                    # against) would sit in credit_reference_heat_s forever,
                    # permanently outgrowing credit_applied_s for a reason
                    # that has nothing to do with an accrual-formula defect
                    # -- exactly the false positive this block prevents.
                    spend_ref = min(z.credit_reference_heat_s, nominal_s) if apply_dwell_credit else 0.0
                    z.credit_reference_applied_s += spend_ref
                    z.credit_reference_heat_s = 0.0
                    # DEFECT 1 fix: open this dwell's fixed-length window,
                    # starting at the dwell's own entry (NOT band-entry
                    # during the preceding ramp tail -- a heavily-lagging
                    # ramp tail can itself run longer than the dwell's
                    # nominal duration, which would make a band-entry-
                    # anchored window close before the dwell even starts).
                    # Length is the dwell's full nominal_s, independent of
                    # spend/credit -- see ``_ZoneState``'s window-parity
                    # fields and ``dwell_credit_parity`` for why.
                    z.window_nominal_s = nominal_s
                    z.window_end_t = t + nominal_s
                    z.window_target_c = seg_start_c[zi]
                    z.window_pending_heat_s = 0.0
                z.lagging = False
                z.commanded_c = seg_start_c[zi]

                # PID_EXPANSION_PLAN.md sec 7.3 extension (2026-09-03,
                # "extend dwell-credit accrual past the nominal ramp end"):
                # mirrors firmware's ramp_assist_credit_should_accrue() --
                # keep banking credit for AS LONG AS actual_c has not yet
                # reached this dwell's own held target, even after the
                # RampStep formally ended (z.lagging released at the WIDE
                # lag_band_c=25C band above) and z.seg_idx moved on to this
                # DwellStep. Same gap as firmware: the ramp step's own
                # freeze-and-resume releases at 25C, but credit's in_band
                # test requires within half a cone step (well under 25C for
                # every shipped Orton target) -- under heavy thermal mass a
                # zone routinely un-locks (ending the ramp) while still
                # outside the credit band, and only crosses into it after
                # this dwell has already begun. seg_start_c[zi] here is the
                # dwell's held target -- the SAME step.target_c the
                # preceding RampStep branch above used, just read from the
                # zone's own commanded temperature now that the step object
                # itself has moved past the ramp.
                #
                # THE HAZARD (do not remove without re-reading): credit
                # banked HERE, during THIS dwell occurrence, must never
                # shorten THIS SAME occurrence -- z.dwell_remaining_s was
                # already computed once, above, from a frozen `spend` at
                # this occurrence's own first tick, and is only ever
                # decremented by dt from here on, never recomputed from
                # z.credit_s. Any further accrual this loop adds to
                # z.credit_s is carried forward and can only ever be spent
                # at the NEXT DwellStep occurrence's own first tick (the
                # `if z.dwell_remaining_s is None:` block above) -- a
                # structurally later point in time, so it cannot reach back
                # and shorten the dwell it was earned during. See
                # test_dwell_credit_accrual_during_dwell_does_not_shorten_
                # that_dwell in test_ramp_assist_cone_scale.py for the
                # pinned proof.
                behind_schedule = actual_c < seg_start_c[zi]
                try:
                    band_bottom = ct.band_bottom_c(seg_start_c[zi])
                    in_band = band_bottom <= actual_c < seg_start_c[zi]
                except ct.ConeTableError:
                    in_band = False  # target outside the cone table's range -- no credit, no crash
                if behind_schedule and in_band:
                    try:
                        w = ct.heat_work_weight(actual_c, seg_start_c[zi])
                    except ct.ConeTableError:
                        w = 0.0
                    z.credit_s += w * dt
                    try:
                        w_ref = ct.heat_work_weight(actual_c, seg_start_c[zi])
                    except ct.ConeTableError:
                        w_ref = 0.0
                    z.credit_reference_heat_s += w_ref * dt

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

    # Any window still pending when the simulation loop ends (e.g. it hit
    # max_sim_s, or every zone finished before a shortened dwell's fixed
    # window naturally closed above) is force-closed here with whatever
    # was accumulated -- an incomplete window undercounts window_actual
    # relative to window_ideal, which is the correct, honest result of
    # running out of simulated time, not a bug to paper over.
    for zi in range(n):
        z = zones[zi]
        if z.window_end_t is not None:
            z.window_actual_total_s += z.window_pending_heat_s
            z.window_ideal_total_s += z.window_nominal_s
            z.window_end_t = None
            z.window_pending_heat_s = 0.0
            z.window_target_c = None

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
        credit_reference_heat_s=[z.credit_reference_heat_s for z in zones],
        credit_reference_applied_s=[z.credit_reference_applied_s for z in zones],
        heat_work_s=[z.heat_work_s for z in zones],
        dwell_heat_work_s=[z.dwell_heat_work_s for z in zones],
        window_actual_heat_s=[z.window_actual_total_s for z in zones],
        window_ideal_heat_s=[z.window_ideal_total_s for z in zones],
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
    """The metric that actually answers "does dwell credit under/over-fire".

    DEFECT 1, found by adversarial review (see
    ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` §7.3): the PREVIOUS
    version of this function computed ``parity_vs_unassisted_pct`` as
    ``(credit_s + dwell_heat_work_assisted_s - dwell_heat_work_unassisted_s)
    / dwell_heat_work_unassisted_s``. During a dwell the zone sits at
    target, so its heat-work weight is ~1.0 -- shortening the dwell by
    ``credit_s`` seconds removes ~``credit_s`` weight-seconds from
    ``dwell_heat_work_assisted_s`` relative to the unassisted run, so
    algebraically ``credit_s + dwell_heat_work_assisted_s ~=
    dwell_heat_work_unassisted_s`` FOR ANY VALUE OF ``credit_s`` -- the
    metric compared credit against itself and read ~0 regardless of
    whether the credit was correct, half, double or triple what it should
    have been (proven by scaling only the accrual line
    ``z.credit_s += w * dt``; a bare-seconds regression that inflated
    credit ~10-30x on this function's own test schedule still reported
    parity spanning only 0.000%-4.380%, and two of three zones read
    EXACTLY 0.000% despite being genuinely mis-banked). The docstring claim
    "near 0 means the credit mechanism is unit-consistent" was false and
    has been removed along with the metric it described.

    THE FIX has two independent parts, because the two candidate
    replacements turned out to have different sensitivity in practice (see
    "note on iteration" below):

    ``credit_audit_pct`` compares ``credit_applied_s`` (the running total
    of credit actually spent off dwells over the whole run -- what
    ``credit_s`` was really used for) against ``credit_reference_heat_s``
    -- an audit accumulator computed at the SAME accrual gate (same tick,
    same ``lagging``/``in_band`` condition) but on its OWN separate ``+=``
    statement, not derived from ``credit_s`` in any way. In correct code
    both statements compute the identical ``weight * dt`` and the two
    totals are exactly equal (0.000% deviation, to float precision) for
    ANY schedule -- but a regression confined to the real accrual line (a
    wrong scale factor, or banking raw seconds instead of weight-seconds)
    leaves the independent audit line untouched, so the two totals diverge
    by EXACTLY the size of the regression: scaling the real accrual line
    by a factor S makes ``credit_audit_pct`` read exactly ``(S-1)*100%``,
    confirmed empirically. This is not an identity in the way the old
    metric was: the old metric forced ``credit_s +
    dwell_heat_work_assisted_s ~= dwell_heat_work_unassisted_s`` to hold
    for any value of ``credit_s`` by construction (both sides
    algebraically absorb it); here, a wrong ``credit_s`` has nothing
    canceling it on the other side of the comparison.

    WHAT THIS DOES NOT PROVE: ``credit_audit_pct`` is a two-writer
    consistency check on a SINGLE accrual statement -- both writers share
    the same weight function, the same band (``cone_table.band_bottom_c``),
    the same target, the same accrual gate, and the same tick loop. It
    detects an ACCRUAL/SPEND IMPLEMENTATION SLIP (the two statements
    disagreeing about how much was banked or spent) and nothing else. It
    is BLIND to an error shared by both writers, because a shared error
    moves both sides of the comparison together and cancels out of the
    ratio. Confirmed by two independent mutations that left it unmoved:
    doubling the credit band's half-width (``credit_s`` rose from 32.45 to
    132.16, +307%, on this module's own reference schedule) and changing
    Ea from 300 kJ/mol to 500 kJ/mol (``credit_s`` fell from 32.45 to
    27.26, -16%) both left ``credit_audit_pct`` at exactly 0.0% on every
    zone. The weight function, the activation energy, and the band width
    are NOT validated by this check, and are not validated by any metric
    in this module -- see ``cone_table.h``'s Ea-sensitivity note for the
    current honest error range on the weight function's Ea choice.

    ``window_parity_pct`` (a secondary, physically-grounded signal) is
    computed from the ASSISTED run's ``window_actual_heat_s``/
    ``window_ideal_heat_s`` (populated by ``run_ramp_assist`` -- see
    ``_ZoneState``'s window-parity fields). A "window" is FIXED-LENGTH --
    exactly the dwell's own nominal duration -- starting at the dwell's
    own entry, independent of ``credit_s``. ``window_actual_heat_s`` is
    REAL heat work (``weight * dt``) integrated tick-by-tick straight from
    the plant over that fixed span (which runs PAST an early-ended,
    over-credited dwell into whatever comes next); it never reads
    ``credit_s`` either. This one IS still sensitive to the underlying
    physics (how much the zone's real trajectory diverges once the
    schedule moves on), so on short dwells/small credits its signal can be
    noisy or, in schedules where nothing physically distinguishable
    follows the dwell (e.g. it is the LAST step and the zone simply keeps
    holding), it can fail to move with the credit's error at all -- this
    module's test file documents that a trailing, unfollowed dwell is a
    known blind spot for this signal specifically, which is why
    ``credit_audit_pct`` is the one this module treats as authoritative.

    Note on iteration: the FIRST replacement attempted here was
    ``window_parity_pct`` alone, using a window anchored at ramp-tail
    band-entry. On this function's own test schedule that version was
    ALSO revealed to be flat/uninformative by the same SCALE-sweep
    discipline used to catch the original bug (in one arrangement the
    window closed before the dwell even started because a lagging ramp
    tail can itself run longer than a short dwell's nominal length; in
    another, a trailing unfollowed dwell made "the ticks after the dwell"
    physically indistinguishable from "the dwell itself"). Both were
    genuine dead ends, not just documentation gaps -- caught and discarded
    before landing, which is exactly why ``credit_audit_pct`` exists as
    the metric this module actually relies on.

    Also still runs ``schedule`` a second time with credit disabled to
    report a couple of older, weaker whole-run/dwell-only figures for
    continuity:

      ``credit_s``               -- heat-work (weight*dt) banked while
                                  lagging AND in-band, spent once against
                                  the following dwell's nominal duration.
      ``dwell_heat_work_assisted_s``/``dwell_heat_work_unassisted_s`` --
                                  real heat work delivered during the
                                  (shortened / full-nominal) dwell alone.
      ``parity_vs_nominal_pct``  -- recipe total against the idealized
                                  ``dwell_nominal_s`` recipe-book target;
                                  always shows a negative residual whenever
                                  ``catchup_deficit_s`` > 0, WITH OR WITHOUT
                                  ramp assist -- a real plant-catchup
                                  effect, not a credit-accounting bug (see
                                  the zero-credit sanity check in this
                                  module's tests).
      ``catchup_deficit_s``      -- ``dwell_nominal_s -
                                  dwell_heat_work_unassisted_s``: how much
                                  heat work the plant's own catch-up lag at
                                  dwell entry costs, independent of ramp
                                  assist entirely.

    ``parity_vs_unassisted_pct`` from the old implementation has been
    REMOVED, not merely renamed -- it is the discredited near-identity
    metric described above; do not resurrect it as a proxy for
    correctness.
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
        parity_vs_nominal_pct = (
            100.0 * (recipe_total_s - dwell_nominal_s) / dwell_nominal_s
            if dwell_nominal_s > 0 else float('nan'))
        catchup_deficit_s = dwell_nominal_s - dhw_u

        window_actual_s = assisted['window_actual_heat_s'][zi]
        window_ideal_s = assisted['window_ideal_heat_s'][zi]
        window_parity_pct = (
            100.0 * (window_actual_s - window_ideal_s) / window_ideal_s
            if window_ideal_s > 0 else float('nan'))

        # credit_reference_applied_s (NOT the raw whole-run credit_reference_
        # heat_s) -- see _ZoneState.credit_reference_applied_s's own doc
        # comment: since the sec 7.3 extension (2026-09-03) let accrual
        # outlive a dwell it may never be spent against (a terminal dwell
        # with no later dwell occurrence in the schedule), the raw
        # accumulator legitimately grows past what credit_applied_s could
        # ever reflect, for a reason that has nothing to do with an
        # accrual-formula defect. The "applied" variant is reset in lockstep
        # with credit_s, so it stays the correct like-for-like comparison.
        reference_heat_s = assisted['credit_reference_applied_s'][zi]
        credit_audit_pct = (
            100.0 * (credit_s - reference_heat_s) / reference_heat_s
            if reference_heat_s > 0 else float('nan'))

        out.append(dict(
            credit_s=credit_s, dwell_heat_work_assisted_s=dhw_a,
            dwell_heat_work_unassisted_s=dhw_u, dwell_nominal_s=dwell_nominal_s,
            recipe_total_s=recipe_total_s,
            parity_vs_nominal_pct=parity_vs_nominal_pct,
            catchup_deficit_s=catchup_deficit_s,
            window_actual_heat_s=window_actual_s,
            window_ideal_heat_s=window_ideal_s,
            credit_reference_heat_s=reference_heat_s,
            credit_audit_pct=credit_audit_pct,
            window_parity_pct=window_parity_pct,
        ))
    return dict(per_zone=out, assisted=assisted, unassisted=unassisted)
