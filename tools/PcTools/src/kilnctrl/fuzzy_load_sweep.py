"""Fuzzy strength x load cross sweep -- owner request 2026-09-02: the two
sweeps that ran the night before this one never crossed.

  * The fuzzy-strength sweep (PID_EXPANSION_PLAN.md sec 3.6, commits
    e41dbd1/69df78e) ran strength 0/25/50/75/100 at ONE load -- the
    identified/unloaded condition (``load_mass_sweep.MASS_MULTIPLIERS``'s
    ``1.0x_identified`` point). Strength 0 won monotonically, but every gap
    was well under the discrimination threshold.
  * The load sweep (sec 3.8, commit 556f8c5, ``load_mass_sweep.py``) ran
    mass 1.0-4.0x and coupling 1.0/0.7/0.4x at ONE fuzzy strength -- zero.

Neither sweep can answer whether the fuzzy layer helps AT a load it was
never exercised against. ``pid_fuzzy.h``'s stated intent is robustness to
TEMPERATURE ("one set of tuning numbers keeps working across a temperature
range wider than the point Autotune ran at"), but added mass perturbs the
plant the same way a temperature swing does -- slower response, larger
effective tau (``load_mass_sweep``'s own model, sec 1 of that module's
docstring) -- so a gain-scheduling layer driven by error and error-rate
could plausibly absorb some of what a fixed feedforward matrix cannot. There
is a concrete reason to expect an effect here specifically: sec 3.8 found
z2's duty saturating first as load rises (41-78% at 1.0-2.0x, pinned by
3.0x) while the controller's feedforward matrix is fixed and cannot see
load at all -- exactly where a feedback-side adaptation would have to do
the compensating, if anything can.

This module is purely additive glue over the two existing, unmodified
pieces named above: ``load_mass_sweep.loaded_K_tau`` for the load model
(mass scales tau only, coupling_mult scales K_full's off-diagonal only,
diagonal/DC-gain untouched) and ``plant_sim``'s fuzzy PID mirror
(``PID.fuzzy_strength_pct``, wired byte-for-byte off ``pid_fuzzy_adjust``)
plus its measurement-chain model (quantization + Gaussian noise,
``run_profile``'s own args). Neither ``plant_sim.py`` nor
``load_mass_sweep.py`` is edited by this file. The per-tick loop below is a
third copy of the same coupled-ff/PID loop ``plant_sim.run_profile`` and
``load_mass_sweep._run_loop`` already each carry locally (for the same
reason ``load_mass_sweep.py`` gives for its own copy: a load-scaled plant
can't be driven through either existing function's fixed signature without
editing it, and two other agents are using both files tonight) -- with
fuzzy strength AND the measurement chain both wired in, which neither of
those two loops does at once.

THE CONTROLLER NEVER SEES load OR THE MEASUREMENT MOCK's true state -- feed-
forward always uses the plant's bench-identified (unloaded) K_full/tau
(``load_mass_sweep`` sec 3), and every PID reads only the quantized+noisy
measurement, exactly like the real firmware reads its ADC. Both matches
firmware behaviour with no load sensor and no perfect thermocouple.

See ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.6/3.8 for where
this is recorded, and ``tests/test_fuzzy_load_sweep.py`` for the mutation-
tested checks.
"""
from __future__ import annotations

import dataclasses
import multiprocessing
import os
from typing import Optional, Sequence

import numpy as np

from . import load_mass_sweep as lms
from . import log_analysis as la
from . import plant_sim as ps

FUZZY_STRENGTHS = (0.0, 25.0, 50.0, 75.0, 100.0)

# Owner asked for the full mass axis crossed with "at least the 1.0/0.7
# coupling variants" -- both from load_mass_sweep.py's own named sets, not
# reinvented here.
MASS_MULTIPLIERS = lms.MASS_MULTIPLIERS
COUPLING_MULTIPLIERS = {
    k: v for k, v in lms.COUPLING_MULTIPLIERS.items() if k in ("1.0x_unblocked", "0.7x_shelved")
}

# Hardware discrimination thresholds, PID_EXPANSION_PLAN.md sec 3.4
# (2026-09-02d, from the rested-start held-out RMS) -- unchanged, imported
# nowhere else in code because they've so far only ever been compared by
# hand in the doc; pinned here as constants so this module's own verdict
# logic can't silently drift from the number the doc quotes.
DISCRIMINATION_THRESHOLD_C = {0: 2.1, 1: 1.2, 2: 1.3}

DEFAULT_SEEDS = (0, 1, 2, 3, 4)
DEFAULT_MEASUREMENT_QUANTUM_C = 0.1
DEFAULT_MEASUREMENT_NOISE_STD_C = 0.05


def _default_capture_path() -> str:
    return os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "fixtures", "plant_sim", "final.jsonl")


def _profile7_segs(capture_path: Optional[str] = None):
    rows_all = la.parse_profile_exec_jsonl(capture_path or _default_capture_path())
    runs = la.split_runs(rows_all)
    rows = runs[0]
    segs, start_c = ps.segs_from_capture(rows)
    return segs, start_c


def _run_loop_fuzzy(plant, segs, kp, ki, kd, ambient, fuzzy_strength_pct,
                     measurement_quantum_c, measurement_noise_std_c, measurement_seed,
                     integral_floor='ff_hold'):
    """Same coupled-ff/PID loop as ``plant_sim.run_profile`` and
    ``load_mass_sweep._run_loop``, with BOTH the fuzzy gain layer and the
    measurement mock wired in against a load-scaled plant object -- see
    module docstring for why this is a third copy rather than editing
    either existing one."""
    pids = [ps.PID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0,
                    fuzzy_strength_pct=fuzzy_strength_pct) for _ in range(ps.N_ZONES)]
    total_t = segs[-1][1]
    times, targets, temps_log, duty_log = [], [], [], []
    duty = np.zeros(ps.N_ZONES)
    t = 0.0
    rng = np.random.default_rng(measurement_seed) if measurement_noise_std_c > 0.0 else None
    while t <= total_t:
        target_c = target_rate = None
        for si, (t0, t1, c0, c1, rate) in enumerate(segs):
            if t0 <= t <= t1 or si == len(segs) - 1:
                if rate == 0.0:
                    target_c, target_rate = c1, 0.0
                else:
                    target_c, target_rate = c0 + rate * (t - t0), rate
                break
        for i in range(ps.N_ZONES):
            hold, climb, ff = ps.coupled_ff_hold_climb(target_c, target_rate, i, ambient=ambient)
            meas_c = plant.temp[i]
            if rng is not None:
                meas_c = meas_c + rng.normal(0.0, measurement_noise_std_c)
            if measurement_quantum_c > 0.0:
                meas_c = round(meas_c / measurement_quantum_c) * measurement_quantum_c
            duty[i], _ = pids[i].update(target_c, meas_c, ps.DT, ff, hold,
                                         integral_floor=integral_floor)
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        plant.step(duty)
        t += ps.DT

    targets_arr = np.array(targets)
    return dict(t=np.array(times), target=targets_arr,
                temps=np.array(temps_log), duty=np.array(duty_log),
                seg_bounds=[s[1] for s in segs])


def run_profile7_fuzzy_loaded(mass_mult: float, coupling_mult: float, fuzzy_strength_pct: float,
                               measurement_seed: int = 0,
                               kp: float = 0.06, ki: float = 0.0003, kd: float = 0.0,
                               ambient: float = 20.0,
                               measurement_quantum_c: float = DEFAULT_MEASUREMENT_QUANTUM_C,
                               measurement_noise_std_c: float = DEFAULT_MEASUREMENT_NOISE_STD_C,
                               capture_path: Optional[str] = None) -> dict:
    """Profile 7, current production gains/ADOPTED matrix, one (mass,
    coupling, fuzzy strength, seed) cell. Plant is load-scaled
    (``load_mass_sweep.loaded_K_tau``); controller feedforward is not
    (module docstring). Measurement chain (0.1 C quantum / 0.05 C noise by
    default) is always on, matching the 69df78e re-run's finding that the
    noise-free sim structurally cannot exercise the fuzzy layer's design
    target."""
    segs, start_c = _profile7_segs(capture_path)
    start_temp = [start_c, start_c, start_c]
    K_loaded, tau_loaded = lms.loaded_K_tau(mass_mult, coupling_mult)
    plant = ps.FOPDTPlant(K_loaded, tau_loaded, ps.L, ps.DT, ambient=ambient, start_temp=start_temp)
    result = _run_loop_fuzzy(plant, segs, kp, ki, kd, ambient, fuzzy_strength_pct,
                              measurement_quantum_c, measurement_noise_std_c, measurement_seed)
    result.update(mass_mult=mass_mult, coupling_mult=coupling_mult,
                   fuzzy_strength_pct=fuzzy_strength_pct, measurement_seed=measurement_seed,
                   segs=segs)
    return result


# ---------------------------------------------------------------------------
# Metrics -- whole-run normalized IAE (time-weighted mean |error|, matching
# log_analysis.window_zone_stats's definition: DT is uniform here so this
# reduces to a plain mean over samples, last-sample weight 0 as in that
# left-Riemann convention), ramp mean/worst error, dwell overshoot peak,
# steady-state offset (final 60s of the run), settle metrics, and duty
# saturation fraction -- the same set used throughout PID_EXPANSION_PLAN.md.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class FuzzyLoadMetrics:
    mass_mult: float
    coupling_mult: float
    fuzzy_strength_pct: float
    measurement_seed: int
    zone: int
    iae_whole_c: float
    ramp_mean_err_c: float
    ramp_worst_err_c: float
    dwell_overshoot_peak_c: float
    steady_offset_c: float          # signed mean error, final 60s of the run
    settle_s: Optional[float]       # elapsed time from run start to |err|<1.0C sustained to the end; None if never
    duty_saturated_frac: float      # fraction of whole run at duty>=0.999


def _time_weighted_mean_abs(t: np.ndarray, err: np.ndarray) -> float:
    if len(t) < 2:
        return float(np.abs(err).mean()) if len(err) else float("nan")
    dts = np.diff(t)
    dts = np.append(dts, 0.0)
    total = dts.sum()
    if total <= 0:
        return float(np.abs(err).mean())
    return float((np.abs(err) * dts).sum() / total)


def compute_metrics(result: dict) -> list:
    t, target, temps, duty = result['t'], result['target'], result['temps'], result['duty']
    segs = result['segs']
    ramp_mask = np.zeros(len(t), dtype=bool)
    for (t0, t1, c0, c1, rate) in segs:
        if rate != 0.0:
            ramp_mask |= (t >= t0) & (t <= t1)
    final_60_mask = t >= (t[-1] - 60.0) if len(t) else np.array([], dtype=bool)

    out = []
    for z in range(ps.N_ZONES):
        err = temps[:, z] - target
        iae_whole = _time_weighted_mean_abs(t, err)
        if ramp_mask.any():
            ramp_err = err[ramp_mask]
            ramp_mean = float(ramp_err.mean())
            ramp_worst = float(np.abs(ramp_err).max())
        else:
            ramp_mean = ramp_worst = float("nan")

        overshoot_peak = 0.0
        for (t0, t1, c0, c1, rate) in segs:
            if rate == 0.0:
                w = (t >= t0) & (t <= min(t1, t0 + 220.0))
                if w.any():
                    overshoot_peak = max(overshoot_peak, float(err[w].max()))

        steady_offset = float(err[final_60_mask].mean()) if final_60_mask.any() else float("nan")

        settle_s = None
        below = np.abs(err) < 1.0
        for i in range(len(t)):
            if below[i:].all():
                settle_s = float(t[i])
                break

        out.append(FuzzyLoadMetrics(
            mass_mult=result['mass_mult'], coupling_mult=result['coupling_mult'],
            fuzzy_strength_pct=result['fuzzy_strength_pct'], measurement_seed=result['measurement_seed'],
            zone=z, iae_whole_c=iae_whole, ramp_mean_err_c=ramp_mean, ramp_worst_err_c=ramp_worst,
            dwell_overshoot_peak_c=overshoot_peak, steady_offset_c=steady_offset, settle_s=settle_s,
            duty_saturated_frac=float((duty[:, z] >= 0.999).mean()),
        ))
    return out


# ---------------------------------------------------------------------------
# Parallel grid runner
# ---------------------------------------------------------------------------

def _run_one_cell(task):
    (mass_key, mass_mult, coupling_key, coupling_mult, strength, seed,
     kp, ki, kd, ambient, quantum, noise_std) = task
    result = run_profile7_fuzzy_loaded(
        mass_mult, coupling_mult, strength, measurement_seed=seed,
        kp=kp, ki=ki, kd=kd, ambient=ambient,
        measurement_quantum_c=quantum, measurement_noise_std_c=noise_std)
    return [dataclasses.asdict(m) for m in compute_metrics(result)]


def run_grid(mass_mults: dict = None, coupling_mults: dict = None,
             strengths: Sequence[float] = FUZZY_STRENGTHS, seeds: Sequence[int] = DEFAULT_SEEDS,
             kp: float = 0.06, ki: float = 0.0003, kd: float = 0.0, ambient: float = 20.0,
             measurement_quantum_c: float = DEFAULT_MEASUREMENT_QUANTUM_C,
             measurement_noise_std_c: float = DEFAULT_MEASUREMENT_NOISE_STD_C,
             workers: int = 4) -> list:
    """Every (mass, coupling, fuzzy strength, seed) cell, in parallel across
    ``workers`` processes (per CLAUDE.md's multicore-whenever-possible
    guidance -- this grid is |mass|*|coupling|*|strengths|*|seeds| runs).
    Returns a flat list of ``FuzzyLoadMetrics``-shaped dicts (one row per
    zone per cell), in deterministic order regardless of which worker
    finishes first."""
    mass_mults = mass_mults or MASS_MULTIPLIERS
    coupling_mults = coupling_mults or COUPLING_MULTIPLIERS

    mass_items = sorted(mass_mults.items(), key=lambda kv: kv[1])
    coupling_items = sorted(coupling_mults.items(), key=lambda kv: -kv[1])

    tasks = []
    order = []
    for mkey, mmult in mass_items:
        for ckey, cmult in coupling_items:
            for strength in strengths:
                for seed in seeds:
                    tasks.append((mkey, mmult, ckey, cmult, strength, seed, kp, ki, kd, ambient,
                                  measurement_quantum_c, measurement_noise_std_c))
                    order.append((mkey, ckey, strength, seed))

    if workers <= 1:
        results = [_run_one_cell(t) for t in tasks]
    else:
        with multiprocessing.Pool(processes=workers) as pool:
            results = pool.map(_run_one_cell, tasks)

    rows = []
    for cell_rows in results:
        rows.extend(cell_rows)
    return rows


def summarize_by_strength_load(rows: Sequence[dict]) -> dict:
    """Groups rows by (mass_mult, coupling_mult, fuzzy_strength_pct, zone)
    and reports the mean IAE across seeds plus the seed-to-seed std (the
    "seed spread" the honesty gate is checked against)."""
    from collections import defaultdict
    grouped = defaultdict(list)
    for r in rows:
        key = (r['mass_mult'], r['coupling_mult'], r['fuzzy_strength_pct'], r['zone'])
        grouped[key].append(r['iae_whole_c'])
    out = {}
    for key, vals in grouped.items():
        arr = np.array(vals)
        out[key] = dict(mean_iae_c=float(arr.mean()), seed_std_c=float(arr.std()), n=len(arr))
    return out


def find_best_strength_per_load(summary: dict) -> dict:
    """For each (mass, coupling, zone), the strength with the lowest mean
    IAE and whether it beats strength=0 by more than the larger of the two
    cells' seed std AND the zone's hardware discrimination threshold --
    the honesty gate from the task: refuse a winner inside either bound."""
    from collections import defaultdict
    by_load_zone = defaultdict(dict)
    for (mass, coupling, strength, zone), stats in summary.items():
        by_load_zone[(mass, coupling, zone)][strength] = stats

    out = {}
    for (mass, coupling, zone), by_strength in by_load_zone.items():
        if 0.0 not in by_strength:
            continue
        base = by_strength[0.0]
        best_strength, best_stats = 0.0, base
        for strength, stats in by_strength.items():
            if stats['mean_iae_c'] < best_stats['mean_iae_c']:
                best_strength, best_stats = strength, stats
        margin = base['mean_iae_c'] - best_stats['mean_iae_c']
        seed_bound = max(base['seed_std_c'], best_stats['seed_std_c'])
        threshold = DISCRIMINATION_THRESHOLD_C.get(zone, 1.0)
        clears = margin > seed_bound and margin > threshold
        out[(mass, coupling, zone)] = dict(
            best_strength=best_strength, base_iae_c=base['mean_iae_c'],
            best_iae_c=best_stats['mean_iae_c'], margin_c=margin,
            seed_bound_c=seed_bound, threshold_c=threshold,
            clears_honesty_gate=bool(best_strength != 0.0 and clears),
        )
    return out


def format_grid_table(summary: dict) -> str:
    header = (f"{'mass':>6s} {'cpl':>5s} {'strength':>8s} {'z':>1s} "
              f"{'mean_iae_c':>10s} {'seed_std_c':>10s} {'n':>3s}")
    lines = [header]
    for key in sorted(summary.keys(), key=lambda k: (k[0], -k[1], k[3], k[2])):
        mass, coupling, strength, zone = key
        s = summary[key]
        lines.append(f"{mass:6.1f} {coupling:5.2f} {strength:8.0f} {zone:1d} "
                      f"{s['mean_iae_c']:10.3f} {s['seed_std_c']:10.4f} {s['n']:3d}")
    return "\n".join(lines)
