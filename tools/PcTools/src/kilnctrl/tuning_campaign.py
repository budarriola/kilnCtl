"""PID tuning-method recommendation campaign.

Answers a different question than a normal autotune run: not "which method
tunes best at the condition it was tuned at" but "which method's EMPTY-KILN
gains hold up once the kiln is actually loaded, across the peak-temperature
range this firmware fires to." Tuning is always performed empty (that's how
``autotune_engine.c`` runs it); firing happens at many loads. This module
tunes once per method on an empty-kiln step/relay test, then evaluates every
resulting gain set across a peak-temperature x load grid, and writes a small
flat recommendation artifact for the web GUI to render.

METHOD COVERAGE -- mirrors firmware/KilnFW/App/drivers/pid_autotune.c
exactly, no invented conversions:
  * SIMC       (FOPDT step-test path, ``pid_autotune_tune_from_fopdt``, the
                default rule)
  * Cohen-Coon (FOPDT step-test path, same function)
  * Ziegler-Nichols   (relay-feedback path, ``pid_autotune_tune_from_relay``)
  * Tyreus-Luyben     (relay-feedback path, same function)
"step" itself is not a tuning rule -- it is the step test SIMC/Cohen-Coon
are fitted from -- so it is not a fifth row here.

FOPDT FIT SIMPLIFICATION (documented, not hidden): this module fits tau/L
from the classic two-point (28.3%/63.2%) crossing method the firmware also
uses as its *starting* candidate, but does NOT reproduce
``pid_autotune_fit_fopdt``'s iterative end-of-trace-slope asymptote
correction (that refinement corrects for a trace that hasn't fully settled;
this campaign's step tests are run to settling, so the correction is a
near no-op on data shaped like these -- see that function's own docstring:
"a trace that genuinely reached steady state ... this fix is a no-op"). The
TUNING-RULE ARITHMETIC that consumes the fitted {K, tau, L} or {Ku, Tu} is
copied verbatim from pid_autotune.c (see ``simc_gains``/``cohen_coon_gains``/
``ziegler_nichols_gains``/``tyreus_luyben_gains`` below) -- that is the part
the owner asked to be mirrored exactly.

LOAD MODEL: reuses ``load_mass_sweep.py`` (landed before this module was
written -- the other agent's mass-scaling work, per the owner's "if it has
landed, reuse it" instruction). ``load_mass_sweep.LoadedPhysicalKilnPlant``
scales thermal mass (``mass_mult``) and cross-zone coupling
(``coupling_mult``) off the ASSUMED ``PhysicalKilnPlant`` constants; see that
module's docstring for the full MEASURED/ASSUMED/DERIVED breakdown. This
module reuses its ``mass_mult`` axis only (coupling fixed at 1.0 --
identification-time value -- since coupling is not the variable this
campaign is evaluating) and does not duplicate its model.

HONESTY GATE: every recommendation carries a ``confidence`` field.
``EXTRAPOLATION_BOUNDARY_C`` (plant_sim.py, 80 C) is the line past which
every plant parameter is ASSUMED rather than MEASURED. A peak-temperature
bucket whose max is above that boundary can never be "measured" here,
regardless of how large the margin between methods looks -- see
``_confidence_for``.
"""
from __future__ import annotations

import concurrent.futures
import dataclasses
import json
import math
import pathlib
import subprocess
from typing import Optional, Sequence

import numpy as np

from . import load_mass_sweep, plant_sim

DT = plant_sim.DT
N_ZONES = plant_sim.N_ZONES
AMBIENT_C = 20.0

#: Anchored on __file__, not the process CWD -- a bare relative literal here
#: ("tools/PcTools/config_presets/...") only resolved when the process
#: happened to be launched from the repo root; from anywhere else `open(...,
#: "w")` either raises FileNotFoundError (parent dir absent) or, worse,
#: silently writes into a same-named directory that exists for an unrelated
#: reason. Same bug class fixed for noise_floor.py's DEFAULT_ARTIFACT_PATH in
#: 9311f3c.
_REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_OUT_PATH = str(_REPO_ROOT / "tools" / "PcTools" / "config_presets" / "tuning_recommendations.json")

# ---------------------------------------------------------------------------
# Load model -- reused from load_mass_sweep.py (see module docstring). Three
# points spanning that module's MASS_MULTIPLIERS: the identification-time
# load, a moderate load, and its "realistically full" ceiling. Coupling
# multiplier fixed at 1.0 (identification-time, unblocked) throughout --
# this campaign varies mass only, not the coupling axis.
# ---------------------------------------------------------------------------

LOAD_CONDITIONS = {
    "empty_1.0x_identified": 1.0,
    "moderate_2.0x": 2.0,
    "full_4.0x": 4.0,
}
COUPLING_MULT = 1.0

# Peak temperatures evaluated. 60 C sits below EXTRAPOLATION_BOUNDARY_C (80,
# plant_sim.py) so it is the one bucket the sim's own MEASURED bench-rig
# identification actually covers; the other three are firing temperatures
# named in the owner's request and are necessarily ASSUMED (PhysicalKilnPlant
# regime). Peak temp -> firing name.
PEAK_TEMPS_C = {
    60.0: "bench",
    1000.0: "bisque",
    1222.0: "cone6",
    1285.0: "cone10",
}

RAMP_RATE_C_PER_HR = 120.0  # ASSUMED, typical bisque/glaze schedule pace
DWELL_S = 1800.0  # evaluation dwell length -- shorter than a real hold, ASSUMED representative of steady-state behavior
GUARD_TRIP_OVERSHOOT_C = 15.0  # ASSUMED stand-in for a guard-worthy overshoot; not read from thermal_guard.c
UNREACHABLE_OFFSET_C = 20.0  # ASSUMED: still this far below target at dwell-end means the ramp was
                              # never caught up within the evaluation window -- a physical-limit or
                              # gain-too-weak failure, not "no overshoot" (dwell_overshoot_c alone
                              # cannot see this: an error that never crosses the setpoint reports as
                              # zero overshoot, which would otherwise rank a method that never even
                              # got close as the "best" one -- see build_recommendations' use below)
SETTLE_BAND_C = 2.0

METHODS = ("simc", "cohen_coon", "ziegler_nichols", "tyreus_luyben")


# ---------------------------------------------------------------------------
# Loaded plant -- load_mass_sweep.LoadedPhysicalKilnPlant, reused unmodified
# (see module docstring's LOAD MODEL section).
# ---------------------------------------------------------------------------

def make_plant(mass_mult: float, ambient: float = AMBIENT_C, start_temp=None, regime: str = "physical"):
    """``regime='physical'``: the ASSUMED, cone-10-capable full kiln
    (``PhysicalKilnPlant``) -- used for every peak temp above
    ``EXTRAPOLATION_BOUNDARY_C``. ``regime='measured'``: the actual
    bench-rig identification (``FOPDTPlant``, ``K_full``/``tau``/``L``) --
    used only for the one bench-scale peak (<= boundary), the same
    measured/assumed split ``plant_sim_sweep._run_one`` makes by
    ``max_target_c``. Both go through ``load_mass_sweep``'s mass/coupling
    scaling so the load axis means the same thing in both regimes."""
    if regime == "measured":
        K_loaded, tau_loaded = load_mass_sweep.loaded_K_tau(mass_mult, COUPLING_MULT)
        return plant_sim.FOPDTPlant(K_loaded, tau_loaded, plant_sim.L, DT, ambient=ambient, start_temp=start_temp)
    return load_mass_sweep.LoadedPhysicalKilnPlant(
        DT, ambient=ambient, start_temp=start_temp, mass_mult=mass_mult, coupling_mult=COUPLING_MULT)


# ---------------------------------------------------------------------------
# FOPDT step-test fit -- two-point (28.3%/63.2%) crossing method, mirroring
# pid_autotune_fit_fopdt's non-iterative core (see module docstring).
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class FopdtModel:
    valid: bool
    k_gain_c_per_duty: float = 0.0
    tau_s: float = 0.0
    dead_time_s: float = 0.0
    invalid_reason: str = ""


def _find_crossing_time(t: np.ndarray, y: np.ndarray, target: float) -> Optional[float]:
    for i in range(1, len(t)):
        prev_c, cur_c = y[i - 1], y[i]
        if prev_c <= target <= cur_c:
            span = cur_c - prev_c
            frac = (target - prev_c) / span if abs(span) > 1e-9 else 0.0
            frac = min(max(frac, 0.0), 1.0)
            return t[i - 1] + frac * (t[i] - t[i - 1])
    return None


def fit_fopdt(t: np.ndarray, y: np.ndarray, baseline_c: float, duty_step: float) -> FopdtModel:
    if abs(duty_step) < 1e-6:
        return FopdtModel(valid=False, invalid_reason="duty_step too small")
    if len(t) < 2:
        return FopdtModel(valid=False, invalid_reason="not enough samples")
    raw_rise = y[-1] - baseline_c
    if abs(raw_rise) < 0.5:
        return FopdtModel(valid=False, invalid_reason="response too small to fit")
    target28 = baseline_c + 0.283 * raw_rise
    target63 = baseline_c + 0.632 * raw_rise
    t28 = _find_crossing_time(t, y, target28)
    t63 = _find_crossing_time(t, y, target63)
    if t28 is None or t63 is None:
        return FopdtModel(valid=False, invalid_reason="trace never reaches 28.3%/63.2% of rise")
    tau = 1.5 * (t63 - t28)
    dead_time = max(t63 - tau, 0.0)
    if tau <= 0.0:
        return FopdtModel(valid=False, invalid_reason="fitted tau <= 0")
    return FopdtModel(valid=True, k_gain_c_per_duty=raw_rise / duty_step, tau_s=tau, dead_time_s=dead_time)


def _quantize(value: float) -> float:
    """Round to the nearest MAX31856 representable temperature.

    ``plant_sim.MAX31856_QUANTUM_C`` (0.0078125 C = 1/128 C, the ADC's LSB)
    -- see plant_sim.py's own doc comment on that constant. The rest of this
    module's step/relay tests emitted raw floats from the continuous plant
    model; project history (see MEMORY "Idealized test input bug class") is
    that unquantized synthetic measurement data can hide whole branches a
    real, quantized thermocouple reading would exercise, while the suite
    still reports green. Quantizing the measured-temperature outputs here
    keeps this campaign's synthetic data honest about what the real sensor
    chain actually delivers.
    """
    return round(value / plant_sim.MAX31856_QUANTUM_C) * plant_sim.MAX31856_QUANTUM_C


def run_step_test(duty_step: float = 0.05, duration_s: float = 150_000.0, regime: str = "physical",
                   quantize: bool = True) -> FopdtModel:
    """Empty-kiln (mass_mult=1.0, the identification-time load) open-loop
    step test, symmetric duty across all 3 zones (so coupling nets out and
    zone 0's trace is representative -- see module docstring on the
    single-representative-zone scope). ``duty_step``/``duration_s`` chosen
    so the trace actually reaches steady state before the two-point fit
    runs -- PhysicalKilnPlant's tau at this duty is tens of thousands of
    seconds (checked empirically), far longer than a bench-rig identification
    run; anything shorter fits an asymptote the trace hasn't reached."""
    plant = make_plant(mass_mult=1.0, regime=regime)
    duty = np.full(N_ZONES, duty_step)
    n = int(duration_s / DT)
    t = np.arange(n) * DT
    y = np.empty(n)
    for i in range(n):
        temps = plant.step(duty)
        y[i] = _quantize(temps[0]) if quantize else temps[0]
    return fit_fopdt(t, y, AMBIENT_C, duty_step)


# ---------------------------------------------------------------------------
# Relay-feedback test -- Astrom-Hagglund describing-function Ku, and the
# measured limit-cycle period Tu. Mirrors pid_autotune_fit_relay's Ku
# formula verbatim.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class RelayModel:
    valid: bool
    ku: float = 0.0
    tu_s: float = 0.0
    invalid_reason: str = ""


def run_relay_test(relay_amplitude_duty: float = 0.05, hysteresis_c: float = 1.0,
                    center_c: float = 150.0, duration_s: float = 400_000.0,
                    regime: str = "physical", quantize: bool = True) -> RelayModel:
    """Empty-kiln (mass_mult=1.0) relay-feedback test around ``center_c``,
    symmetric across zones (same rationale as the step test). Amplitude/
    center/duration picked empirically to reach a genuine limit cycle on
    this plant's very long time constant (tens of minutes per half-cycle)."""
    plant = make_plant(mass_mult=1.0, regime=regime)
    n = int(duration_s / DT)
    relay_on = True
    ys = np.empty(n)
    for i in range(n):
        y0 = plant.temp[0]
        y0 = _quantize(y0) if quantize else y0
        ys[i] = y0
        if relay_on and y0 >= center_c + hysteresis_c:
            relay_on = False
        elif not relay_on and y0 <= center_c - hysteresis_c:
            relay_on = True
        duty = np.full(N_ZONES, relay_amplitude_duty if relay_on else 0.0)
        plant.step(duty)

    # Use the trailing half of the trace (past initial transient) to find
    # zero-crossings of (y - center) and derive cycle periods + amplitude.
    tail0 = n // 2
    y_tail = ys[tail0:]
    t_tail = np.arange(tail0, n) * DT
    crossings = []
    for i in range(1, len(y_tail)):
        if (y_tail[i - 1] - center_c) * (y_tail[i] - center_c) < 0:
            crossings.append(t_tail[i])
    if len(crossings) < 4:
        return RelayModel(valid=False, invalid_reason="trace never oscillated enough to measure a limit cycle")
    periods = np.diff(crossings) * 2.0  # half-period between opposite-sign crossings
    tu = float(np.median(periods))
    a = float((y_tail.max() - y_tail.min()) / 2.0)
    denom = a * a - hysteresis_c * hysteresis_c
    if denom <= 0.0 or tu <= 0.0:
        return RelayModel(valid=False, invalid_reason="degenerate oscillation amplitude")
    ku = 4.0 * relay_amplitude_duty / (math.pi * math.sqrt(denom))
    return RelayModel(valid=True, ku=ku, tu_s=tu)


# ---------------------------------------------------------------------------
# Tuning-rule arithmetic, copied verbatim (units and all) from
# pid_autotune.c's pid_autotune_tune_from_fopdt / pid_autotune_tune_from_relay.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class Gains:
    kp: float
    ki: float
    kd: float
    refusal: Optional[str] = None


def simc_gains(model: FopdtModel, lambda_s: float = 0.0) -> Gains:
    if not model.valid:
        return Gains(0, 0, 0, refusal=f"invalid FOPDT model: {model.invalid_reason}")
    if model.k_gain_c_per_duty <= 0.0:
        return Gains(0, 0, 0, refusal="nonpositive fitted gain")
    lam = lambda_s if lambda_s > 0.0 else 3.0 * model.dead_time_s
    if lam <= 0.0:
        lam = model.tau_s if model.tau_s > 0.0 else 1.0
    kc = model.tau_s / (model.k_gain_c_per_duty * (lam + model.dead_time_s))
    ti = min(model.tau_s, 4.0 * (lam + model.dead_time_s))
    td = model.dead_time_s / 2.0
    return Gains(kc, (kc / ti if ti > 0 else 0.0), kc * td)


AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S = 0.5


def cohen_coon_gains(model: FopdtModel) -> Gains:
    if not model.valid:
        return Gains(0, 0, 0, refusal=f"invalid FOPDT model: {model.invalid_reason}")
    if model.dead_time_s < AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S:
        return Gains(0, 0, 0, refusal="dead time below Cohen-Coon's minimum")
    if model.tau_s <= 0.0 or model.k_gain_c_per_duty <= 0.0:
        return Gains(0, 0, 0, refusal="nonpositive tau or gain")
    L, tau = model.dead_time_s, model.tau_s
    r = L / tau
    kc = (1.0 / model.k_gain_c_per_duty) * (tau / L) * (4.0 / 3.0 + r / 4.0)
    ti = L * (32.0 + 6.0 * r) / (13.0 + 8.0 * r)
    td = L * 4.0 / (11.0 + 2.0 * r)
    return Gains(kc, (kc / ti if ti > 0 else 0.0), kc * td)


def ziegler_nichols_gains(model: RelayModel) -> Gains:
    if not model.valid:
        return Gains(0, 0, 0, refusal=f"invalid relay model: {model.invalid_reason}")
    if model.ku <= 0.0 or model.tu_s <= 0.0:
        return Gains(0, 0, 0, refusal="nonpositive Ku/Tu")
    kc = 0.6 * model.ku
    ti = model.tu_s / 2.0
    td = model.tu_s / 8.0
    return Gains(kc, kc / ti, kc * td)


def tyreus_luyben_gains(model: RelayModel) -> Gains:
    if not model.valid:
        return Gains(0, 0, 0, refusal=f"invalid relay model: {model.invalid_reason}")
    if model.ku <= 0.0 or model.tu_s <= 0.0:
        return Gains(0, 0, 0, refusal="nonpositive Ku/Tu")
    kc = model.ku / 3.2
    ti = 2.2 * model.tu_s
    td = model.tu_s / 6.3
    return Gains(kc, kc / ti, kc * td)


def tune_all_methods() -> dict:
    """TUNE step, empty kiln, once per method -- but ONCE PER PLANT REGIME,
    not once overall. The one bench-scale peak (<=EXTRAPOLATION_BOUNDARY_C)
    is evaluated against the actual bench-rig identification (``FOPDTPlant``,
    tau ~ hundreds of seconds); the three firing peaks are evaluated against
    the ASSUMED full-size kiln (``PhysicalKilnPlant``, tau ~ tens of
    thousands of seconds at low duty). These are physically different plants
    in this repo already (see plant_sim.py), so tuning each once against
    the plant it will actually be evaluated on is the faithful mirror of "an
    autotune commissioning run for whichever kiln this is" -- tuning the
    bench rig's gains against the full-kiln model (or vice versa) would not
    be a tuning-METHOD comparison any more, just a plant mismatch.

    Returns ``{"measured": {method: Gains}, "physical": {method: Gains}}``.
    """
    out = {}
    for regime, step_kwargs, relay_kwargs in (
        ("measured", dict(duty_step=0.8, duration_s=6000.0),
         dict(relay_amplitude_duty=0.5, hysteresis_c=1.0, center_c=40.0, duration_s=6000.0)),
        ("physical", dict(), dict()),
    ):
        fopdt = run_step_test(regime=regime, **step_kwargs)
        relay = run_relay_test(regime=regime, **relay_kwargs)
        out[regime] = {
            "simc": simc_gains(fopdt),
            "cohen_coon": cohen_coon_gains(fopdt),
            "ziegler_nichols": ziegler_nichols_gains(relay),
            "tyreus_luyben": tyreus_luyben_gains(relay),
        }
    return out


def _regime_for_peak(peak_c: float) -> str:
    return "measured" if peak_c <= EXTRAPOLATION_BOUNDARY_C else "physical"


# ---------------------------------------------------------------------------
# Evaluation -- closed-loop ramp+dwell at (peak_temp, load), scored with a
# metric set that mirrors pid_ab_compare.py's names/concepts (normalized
# IAE, ramp error, dwell overshoot, steady offset, settle time). Computed
# directly from the sim trace here rather than importing pid_ab_compare's
# run-selection code, which this task must not touch.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class EvalResult:
    method: str
    peak_c: float
    mass_mult: float
    iae_norm: float
    ramp_rms_c: float
    dwell_overshoot_c: float
    steady_offset_c: float
    settle_time_s: Optional[float]
    duty_saturated_frac: float
    oscillating: bool
    guard_trip_worthy: bool
    coupled_solve_infeasible: bool
    unreachable: bool


def evaluate_one(method: str, gains: Gains, peak_c: float, mass_mult: float,
                  regime: str = "physical") -> EvalResult:
    if gains.refusal is not None:
        return EvalResult(method, peak_c, mass_mult, float("nan"), float("nan"), float("nan"),
                           float("nan"), None, 0.0, False, False, False, False)

    plant = make_plant(mass_mult=mass_mult, regime=regime)
    pid = plant_sim.PID(gains.kp, gains.ki, gains.kd, d_tau=30.0, b=1.0, pid_range_c=1000.0)

    ramp_duration_s = max((peak_c - AMBIENT_C) / (RAMP_RATE_C_PER_HR / 3600.0), 1.0)
    total_s = ramp_duration_s + DWELL_S
    n = int(total_s / DT)

    err = np.empty(n)
    target_arr = np.empty(n)
    duty_arr = np.empty(n)
    t = 0.0
    for i in range(n):
        target_c = AMBIENT_C + (RAMP_RATE_C_PER_HR / 3600.0) * t if t < ramp_duration_s else peak_c
        meas = plant.temp[0]
        # Pure-PID evaluation: no feedforward, so the comparison isolates
        # each tuning method's own gains rather than the shared ff formula.
        duty, _ = pid.update(target_c, meas, DT, ff_u=0.0, ff_hold=0.0)
        duty_arr[i] = duty
        target_arr[i] = target_c
        err[i] = meas - target_c
        plant.step(np.full(N_ZONES, duty))
        t += DT

    ramp_mask = target_arr < peak_c
    dwell_mask = ~ramp_mask
    ramp_err = err[ramp_mask]
    dwell_err = err[dwell_mask]

    iae = float(np.mean(np.abs(err)))
    iae_norm = iae / max(peak_c - AMBIENT_C, 1.0) * 100.0  # % of span, our own normalized-IAE definition
    ramp_rms = float(np.sqrt(np.mean(ramp_err ** 2))) if len(ramp_err) else float("nan")
    dwell_overshoot = float(max(dwell_err.max(), 0.0)) if len(dwell_err) else float("nan")
    tail_n = max(int(0.2 * len(dwell_err)), 1) if len(dwell_err) else 0
    steady_offset = float(np.mean(dwell_err[-tail_n:])) if tail_n else float("nan")

    settle_time = None
    if len(dwell_err):
        dwell_t0_idx = int(np.argmax(dwell_mask))
        within = np.abs(dwell_err) < SETTLE_BAND_C
        for i in range(len(within)):
            if within[i:].all():
                settle_time = i * DT
                break

    sat_frac = float(np.mean((duty_arr <= 1e-6) | (duty_arr >= 1.0 - 1e-6)))
    sign_changes = int(np.sum(np.diff(np.sign(dwell_err)) != 0)) if len(dwell_err) > 1 else 0
    oscillating = sign_changes > 6
    guard_trip = dwell_overshoot > GUARD_TRIP_OVERSHOOT_C if not math.isnan(dwell_overshoot) else False
    unreachable = (not math.isnan(steady_offset)) and steady_offset < -UNREACHABLE_OFFSET_C

    return EvalResult(method, peak_c, mass_mult, iae_norm, ramp_rms, dwell_overshoot, steady_offset,
                       settle_time, sat_frac, oscillating, guard_trip, False, unreachable)


def _eval_worker(args):
    method, gains_tuple, peak_c, mass_mult, regime = args
    gains = Gains(*gains_tuple[:3], refusal=gains_tuple[3])
    return dataclasses.asdict(evaluate_one(method, gains, peak_c, mass_mult, regime=regime))


def run_campaign(gains_by_regime: dict, max_workers: int = 24) -> list:
    """EVALUATE step, run in parallel across the (method, peak, load) grid.
    ``gains_by_regime`` is ``tune_all_methods()``'s return shape
    (``{regime: {method: Gains}}``); each peak temp picks its regime (and
    therefore its gains) via ``_regime_for_peak``. Deterministic: the
    plant/PID loop has no randomness, so no seed is needed for
    reproducibility -- every worker's inputs fully determine its output."""
    jobs = []
    for peak_c in PEAK_TEMPS_C:
        regime = _regime_for_peak(peak_c)
        for method, gains in gains_by_regime[regime].items():
            gt = (gains.kp, gains.ki, gains.kd, gains.refusal)
            for load_name, mass_mult in LOAD_CONDITIONS.items():
                jobs.append((method, gt, peak_c, mass_mult, regime))

    results = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as ex:
        for r in ex.map(_eval_worker, jobs):
            results.append(r)
    return results


# ---------------------------------------------------------------------------
# Recommendation aggregation + artifact generation.
# ---------------------------------------------------------------------------

HELD_OUT_RMS_C = [1.05, 0.61, 0.67]
DISCRIMINATION_THRESHOLD_C = [2.1, 1.2, 1.3]
EXTRAPOLATION_BOUNDARY_C = plant_sim.EXTRAPOLATION_BOUNDARY_C
# Single representative zone (see module docstring) -> zone-0 threshold.
MARGIN_THRESHOLD_C = DISCRIMINATION_THRESHOLD_C[0]


def _git_sha() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True, check=True, cwd=None)
        return out.stdout.strip()
    except Exception:
        return "unknown"


def _worst_case_score(rows: Sequence[dict]) -> Optional[float]:
    """Robustness score for one method at one peak temp, across the mass
    grid: the WORST (largest) ABSOLUTE steady-state offset seen at any
    FEASIBLE load -- the quantity this campaign exists to compare, per the
    owner's framing ("which method's empty-kiln gains hold up once
    loaded"). Deliberately NOT ``dwell_overshoot_c``: that metric is
    clamped at 0 on the undershoot side, so a method that chronically
    undershoots by hundreds of degrees (steady_offset_c very negative,
    ``unreachable=True`` -- SIMC's gains at the identified-empty-kiln
    lambda were far too weak for these firing-schedule ramps) reports zero
    overshoot and would rank as "best" under that metric, which is exactly
    backwards. Rows flagged ``unreachable`` (chronic tracking failure, not
    a tuning-quality difference between methods -- see that field's own
    comment) are EXCLUDED from the score so a shared ramp-rate/thermal-mass
    physical limit at heavy load does not swamp the comparison between
    methods that both hit it equally; a method with no feasible row at all
    scores None (refused for ranking, not silently 0)."""
    vals = [abs(r["steady_offset_c"]) for r in rows
            if not math.isnan(r["steady_offset_c"]) and not r["unreachable"]]
    if not vals:
        return None
    return max(vals)


def _confidence_for(peak_c: float, margin_c: Optional[float]) -> str:
    if margin_c is not None and margin_c < MARGIN_THRESHOLD_C:
        return "indistinguishable"
    return "measured" if peak_c <= EXTRAPOLATION_BOUNDARY_C else "extrapolated"


def build_recommendations(eval_rows: list) -> dict:
    by_peak_method = {}
    for r in eval_rows:
        by_peak_method.setdefault(r["peak_c"], {}).setdefault(r["method"], []).append(r)

    recommendations = []
    for peak_c, fw_name in sorted(PEAK_TEMPS_C.items()):
        by_method = by_peak_method.get(peak_c, {})
        scored = []
        for method in METHODS:
            rows = by_method.get(method, [])
            refused = len(rows) == 0 or all(math.isnan(r["steady_offset_c"]) for r in rows)
            score = _worst_case_score(rows) if not refused else None
            all_unreachable = len(rows) > 0 and all(r["unreachable"] for r in rows)
            scored.append((method, score, refused or all_unreachable))

        usable = [(m, s) for m, s, refused in scored if not refused and s is not None]
        if not usable:
            recommendations.append({
                "peak_temp_c_max": peak_c,
                "load": "any",
                "method": "none",
                "rule": "no method tracked the ramp at any evaluated load without chronic (>20C) undershoot",
                "confidence": "extrapolated" if peak_c > EXTRAPOLATION_BOUNDARY_C else "measured",
                "why": ("every tuning method either refused or never caught up to the ramp/dwell target "
                        "at every load evaluated -- likely a ramp-rate-vs-thermal-mass physical limit at "
                        "this peak, not something re-tuning alone fixes; do not autotune unattended here"),
                "runner_up": None,
                "margin_c": None,
            })
            continue

        usable.sort(key=lambda ms: ms[1])
        best_method, best_score = usable[0]
        runner_up_method, runner_up_score = (usable[1] if len(usable) > 1 else (None, None))
        margin_c = (runner_up_score - best_score) if runner_up_score is not None else None
        confidence = _confidence_for(peak_c, margin_c)

        if confidence == "indistinguishable":
            why = (f"worst-case (feasible-load) steady-state offset is within the sim's own "
                   f"{MARGIN_THRESHOLD_C:.1f} C discrimination floor between {best_method} and "
                   f"{runner_up_method} -- treat them as equivalent, pick either")
        elif confidence == "measured":
            why = (f"{best_method} held the lowest worst-case steady-state offset "
                   f"({best_score:.2f} C) across the load grid at this bench-scale "
                   f"target, inside the sim's calibrated range")
        else:
            why = (f"{best_method} ranked lowest worst-case steady-state offset "
                   f"({best_score:.2f} C) across loads in simulation; this peak is above the "
                   f"{EXTRAPOLATION_BOUNDARY_C:.0f} C calibration boundary so treat this as a "
                   f"mechanism comparison, not a calibrated prediction")

        recommendations.append({
            "peak_temp_c_max": peak_c,
            "load": "any",
            "method": best_method,
            "rule": f"lowest worst-case feasible-load steady-state offset across {', '.join(LOAD_CONDITIONS)} at {fw_name} peak",
            "confidence": confidence,
            "why": why,
            "runner_up": runner_up_method,
            "margin_c": (round(margin_c, 3) if margin_c is not None else None),
        })

    caveats = [
        "Tuning is always run on an EMPTY kiln; recommendations rank each method's empty-kiln "
        "gains by how well they hold up once loaded, not by best-case tuning-condition performance.",
        "Single representative zone (coupling and 3-zone coordination are not modeled in the "
        "evaluation loop, only in the plant used to generate the trace); see tuning_campaign.py.",
        "FOPDT fit uses the two-point crossing method without the firmware's iterative "
        "asymptote correction -- a documented simplification, not a formula change to the "
        "tuning-rule arithmetic itself (which is copied verbatim from pid_autotune.c).",
        f"Above {EXTRAPOLATION_BOUNDARY_C:.0f} C every plant parameter is ASSUMED, not measured; "
        "confidence='extrapolated' rows are a mechanism comparison, not a calibrated prediction.",
        "Evaluation runs a pure-PID loop (no feedforward) to isolate each tuning method's own "
        "gains from the shared ff_hold/ff_climb formula.",
        f"At the heaviest evaluated load ({max(LOAD_CONDITIONS.values()):.1f}x the identified mass), "
        "every method's firing-temperature gains failed to track the ramp (chronic >20 C undershoot, "
        "duty saturated) -- a shared ramp-rate-vs-thermal-mass ceiling this campaign's ramp schedule "
        "hits regardless of tuning method, excluded from the ranking score rather than left to swamp "
        "it (see EvalResult.unreachable / _worst_case_score).",
        "At firing temperatures, SIMC's empty-kiln gains (tuned near the identification point's own "
        "lambda) were too weak to track this campaign's ramp rate even at the identified (1x) load -- "
        "the opposite failure from Cohen-Coon's guard-trip-worthy overshoot at the same loads; see the "
        "per-method table this artifact was generated from.",
    ]

    return {
        "schema_version": 1,
        "generated_from": _git_sha(),
        "sim_confidence": {
            "held_out_rms_c": HELD_OUT_RMS_C,
            "discrimination_threshold_c": DISCRIMINATION_THRESHOLD_C,
            "extrapolation_boundary_c": EXTRAPOLATION_BOUNDARY_C,
        },
        "recommendations": recommendations,
        "caveats": caveats,
    }


def main() -> int:
    print("tuning campaign: fitting empty-kiln step + relay tests...")
    gains = tune_all_methods()
    for regime, by_method in gains.items():
        print(f"  regime={regime}:")
        for m, g in by_method.items():
            if g.refusal:
                print(f"    {m}: REFUSED -- {g.refusal}")
            else:
                print(f"    {m}: kp={g.kp:.5f} ki={g.ki:.6f} kd={g.kd:.4f}")

    print("tuning campaign: running evaluation grid in parallel...")
    eval_rows = run_campaign(gains)
    print(f"  {len(eval_rows)} (method, peak, load) evaluations complete")

    artifact = build_recommendations(eval_rows)
    out_path = DEFAULT_OUT_PATH
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(artifact, f, indent=2)
        f.write("\n")
    print(f"wrote {out_path}")
    for rec in artifact["recommendations"]:
        print(f"  <= {rec['peak_temp_c_max']:.0f}C: {rec['method']} ({rec['confidence']}) -- {rec['why']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
