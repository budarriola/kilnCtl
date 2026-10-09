"""Offline membership-band probe for the fuzzy-PID layer
(``firmware/KilnFW/App/drivers/control/pid_fuzzy.c``).

WHY THIS EXISTS. ``pid_fuzzy_adjust()`` is a PURE FUNCTION of
``(error_c, error_rate_c_per_s)`` given a band setting, base gains and
strength -- so for any candidate ``(error_band_c, rate_band_c_per_s)``, the
cell occupancy AND the resulting gain multipliers can be recomputed offline
from an *already-recorded* capture. No kiln time, no firing, no hardware.
This module is the checked-in, tested form of an ad hoc analysis that
answered exactly this question on 2026-09-04 and settled a band question
that was otherwise going to cost a ~10 h re-fire -- the throwaway script was
never kept, so the next band question would have cost another ad hoc replica
of pid_fuzzy.c's math. Not again.

THE SINGLE MOST VALUABLE THING THIS MODULE DOES: it refuses to pool
``control_mode`` != 3 samples into the analysis. The fuzzy layer
(``ZONE_CONTROL_MODE_PID_FUZZY`` == 3) only ever runs under mode 3 --
``pid_fuzzy_prepare_gains()`` is not even called for mode 2 (plain PID). An
earlier ad hoc pass pooled 28 mode-2 captures with a single mode-3 capture
and reported a 37,008-sample conclusion where the real n (mode-3 samples)
was 2178 -- see ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.6g and
MEMORY's "Fuzzy bands never leave the centre cell" note. Every capture here
is checked per-zone, per-tick, against its own recorded ``control_mode``
(``exec.zones[i].control_mode``, always present in an HTTP-capture JSONL
line, unlike the optional ``"control"`` body); a zone/file that never
reaches mode 3 contributes ZERO samples and is reported LOUDLY, not
silently dropped.

MATH SOURCE OF TRUTH. ``pid_fuzzy_adjust()`` below is a line-for-line port
of ``pid_fuzzy.c``'s ``pid_fuzzy_adjust()`` (triangular membership, Mamdani
product over the 3x3 rule table, weighted-average defuzzification, the
strength=0 and non-finite-input short circuits, and the ``clamp_gain``/
``sanitize_base`` guards). ``tools/PcTools/tests/test_fuzzy_band_probe.py``
pins this against the same corner cases
``firmware/KilnFW/App/test/test_pid_fuzzy.c`` asserts on the C side, so the
two cannot silently drift apart -- see that test module's docstring for the
drift-check precedent (``test_autotune_rules_drift_guard.py``,
``test_uart_version_independence.py``). If the two ever disagree, this tool
is confidently wrong, which is worse than not having it: do not trust a
band recommendation from a version of this file that fails its own drift
test.

RATE AXIS: RECONSTRUCTED FROM ``pid_d``/``bd_kd_effective``, NOT A
FINITE-DIFFERENCE PROXY. The rate axis pid_fuzzy_adjust() actually receives
is ``z->pid_state.d_filtered`` -- measurement-derivative, low-pass filtered
inside the firmware's own control loop (``pid.c``'s ``raw_d``/``alpha``
filter), NOT re-derivable from a finite difference of the ``actual_c``
samples this capture format records once per HTTP poll: the poll period is
far coarser than the control tick, so a finite difference over poll rows is
a different (much noisier, differently-scaled) signal than what the
firmware actually fed into the membership math. Fortunately the capture
already contains the firmware's own filtered value, indirectly:
``profile_executor_pid_tick.c``'s ``pid_fuzzy_prepare_gains()`` builds a
``fuzzy_cfg`` whose ``kd`` is ``bd_kd_effective`` and hands it to the same
``pid_update_terms()`` every mode uses, which computes
``pid_d = fuzzy_cfg.kd * state->d_filtered`` -- i.e.
``pid_d == bd_kd_effective * d_filtered`` for a ``PID_FUZZY`` zone. So
``d_filtered = pid_d / bd_kd_effective`` recovers the EXACT rate value the
firmware's own fuzzy layer saw that tick, reconstructed from two fields the
capture already recorded, rather than approximated from a coarser signal.
A sample where ``bd_kd_effective`` is 0 or missing cannot be reconstructed
this way and is skipped (see ``iter_zone_ticks``).
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
from collections import defaultdict
from typing import Optional, Sequence

from kilnctrl.jsonl_util import iter_jsonl

# ---------------------------------------------------------------------------
# Line-for-line port of firmware/KilnFW/App/drivers/control/pid_fuzzy.c.
# Keep this section in lockstep with the C file; see this module's docstring
# and test_fuzzy_band_probe.py's drift-check tests.
# ---------------------------------------------------------------------------

#: Firmware-default band widths (pid_fuzzy.c ERROR_BAND_C_DEFAULT /
#: RATE_BAND_C_PER_S_DEFAULT), used only when a caller passes a non-positive
#: or non-finite band -- same fallback pid_fuzzy_adjust() itself applies.
ERROR_BAND_C_DEFAULT = 20.0
RATE_BAND_C_PER_S_DEFAULT = 0.5

#: pid_fuzzy.c's MAX_NUDGE_FRACTION: the maximum fractional nudge any single
#: gain may receive at strength_pct=100 and full rule membership (1.0).
MAX_NUDGE_FRACTION = 0.5

#: RULE_TABLE[error_bucket][rate_bucket] = (kp_dir, ki_dir, kd_dir), each in
#: {-1, 0, +1}. Bucket order for both axes: 0=NEG/FALLING, 1=ZERO/STEADY,
#: 2=POS/RISING -- transcribed verbatim from pid_fuzzy.c's RULE_TABLE.
RULE_TABLE = (
    # error = NEG (large, overshoot)
    ((1.0, -1.0, 1.0),   # rate FALLING: overshoot still growing -> attack
     (1.0, 0.0, 0.0),    # rate STEADY: steady overshoot -> push harder
     (-1.0, 1.0, -1.0)), # rate RISING: recovering -> ease off
    # error = ZERO (near setpoint)
    ((-1.0, -1.0, 1.0),  # rate FALLING: crossing target fast -> damp
     (-1.0, 1.0, -1.0),  # rate STEADY: settled -> coast on I
     (1.0, -1.0, 1.0)),  # rate RISING: just left target -> catch it
    # error = POS (large, undershoot)
    ((-1.0, 1.0, -1.0),  # rate FALLING: already closing in -> ease off
     (1.0, 0.0, 0.0),    # rate STEADY: steady approach -> push harder
     (1.0, -1.0, 1.0)),  # rate RISING: far and getting worse -> attack
)

BUCKET_NAMES = ("NEG", "ZERO", "POS")


def triangular_memberships(x: float, band: float) -> tuple:
    """Port of pid_fuzzy.c's triangular_memberships(): (neg, zero, pos)
    degrees in [0,1], summing to exactly 1.0 for any finite x. ``band`` must
    already be sanitized (finite, > 0) by the caller, same discipline as the
    C function's own callers."""
    if x <= -band:
        return (1.0, 0.0, 0.0)
    if x >= band:
        return (0.0, 0.0, 1.0)
    if x <= 0.0:
        t = (-x) / band
        return (t, 1.0 - t, 0.0)
    t = x / band
    return (0.0, 1.0 - t, t)


def dominant_bucket_index(degrees: tuple) -> int:
    """Index of the bucket with the largest membership degree (ties broken
    toward the lower index, matching a stable argmax -- only used for the
    human-readable cell-occupancy report, never for the gain math itself,
    which always uses the full blended degrees)."""
    best_i, best_v = 0, degrees[0]
    for i in range(1, len(degrees)):
        if degrees[i] > best_v:
            best_i, best_v = i, degrees[i]
    return best_i


def clamp_gain(g: float) -> float:
    """Port of pid_fuzzy.c's clamp_gain(): never negative, never non-finite."""
    if not math.isfinite(g) or g < 0.0:
        return 0.0
    return g


def sanitize_base(base: float) -> float:
    """Port of pid_fuzzy.c's sanitize_base()."""
    if not math.isfinite(base) or base < 0.0:
        return 0.0
    return base


def pid_fuzzy_adjust(
    error_c: float,
    error_rate_c_per_s: float,
    error_band_c: float,
    rate_band_c_per_s: float,
    base_kp: float,
    base_ki: float,
    base_kd: float,
    strength_pct: int,
) -> tuple:
    """Line-for-line port of pid_fuzzy.c's pid_fuzzy_adjust(). Returns
    (kp, ki, kd). See this module's docstring: this is the pinned-by-test
    reproduction of the firmware's own math, not an approximation."""
    kp = sanitize_base(base_kp)
    ki = sanitize_base(base_ki)
    kd = sanitize_base(base_kd)

    if strength_pct > 100:
        strength_pct = 100
    if strength_pct < 0:
        strength_pct = 0

    # strength_pct == 0: bit-for-bit base gains (sanitized), no fuzzy math.
    if strength_pct == 0:
        return (kp, ki, kd)

    # Faulted-thermocouple defence: non-finite error/rate -> no adjustment.
    if not math.isfinite(error_c) or not math.isfinite(error_rate_c_per_s):
        return (kp, ki, kd)

    error = error_c
    rate = error_rate_c_per_s

    band_e = error_band_c if (math.isfinite(error_band_c) and error_band_c > 0.0) else ERROR_BAND_C_DEFAULT
    band_r = rate_band_c_per_s if (math.isfinite(rate_band_c_per_s) and rate_band_c_per_s > 0.0) else RATE_BAND_C_PER_S_DEFAULT

    e_deg = triangular_memberships(error, band_e)
    r_deg = triangular_memberships(rate, band_r)

    kp_sum = ki_sum = kd_sum = weight_sum = 0.0
    for ei in range(3):
        for ri in range(3):
            firing = e_deg[ei] * r_deg[ri]
            kp_dir, ki_dir, kd_dir = RULE_TABLE[ei][ri]
            kp_sum += firing * kp_dir
            ki_sum += firing * ki_dir
            kd_sum += firing * kd_dir
            weight_sum += firing

    kp_dir = (kp_sum / weight_sum) if weight_sum > 0.0 else 0.0
    ki_dir = (ki_sum / weight_sum) if weight_sum > 0.0 else 0.0
    kd_dir = (kd_sum / weight_sum) if weight_sum > 0.0 else 0.0

    scale = (strength_pct / 100.0) * MAX_NUDGE_FRACTION

    return (
        clamp_gain(kp * (1.0 + scale * kp_dir)),
        clamp_gain(ki * (1.0 + scale * ki_dir)),
        clamp_gain(kd * (1.0 + scale * kd_dir)),
    )


# ---------------------------------------------------------------------------
# Capture reading + control_mode gate.
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class ZoneTick:
    zone: int
    control_mode: int
    error_c: Optional[float]
    rate_c_per_s: Optional[float]


@dataclasses.dataclass
class CaptureZoneSummary:
    """Per-zone accounting for one capture file: what control_mode(s) it
    ever ran under, and how many samples were actually usable for the fuzzy
    math (mode==3 AND a reconstructable rate)."""
    zone: int
    control_modes_seen: "set"
    n_ticks_total: int
    n_mode3_ticks: int
    n_usable_samples: int
    reason_excluded: Optional[str] = None


def iter_zone_ticks(path: str) -> "list[ZoneTick]":
    """Every (line, zone) tick in an HTTP-capture JSONL file.

    ``control_mode`` is read from ``exec.zones[i].control_mode``, which is
    present on EVERY line regardless of whether the capture recorded the
    optional ``"control"`` body -- this is what makes the mode-2 guard work
    even against an older/narrower capture that has no ``pid_d``/
    ``bd_kd_effective`` at all (a mode-2 capture has no fuzzy data to lose by
    this: it is correctly reported as 0 usable samples).

    ``error_c``/``rate_c_per_s`` are ``None`` when the tick's ``control_mode``
    is not 3, or the ``"control"`` body (or fields within it) is unavailable,
    or ``bd_kd_effective`` is 0/missing (rate cannot be reconstructed --
    see module docstring). A caller must check ``control_mode == 3`` AND
    both floats are not ``None`` before using a tick.
    """
    out: "list[ZoneTick]" = []
    for obj in iter_jsonl(path, on_error="skip"):
        if not isinstance(obj, dict):
            continue
        exec_body = obj.get("exec")
        if not isinstance(exec_body, dict):
            continue
        exec_zones = exec_body.get("zones")
        if not isinstance(exec_zones, list):
            continue

        control_body = obj.get("control")
        control_zones_by_idx = {}
        target_c = None
        if isinstance(control_body, dict):
            target_c = control_body.get("target_c")
            for cz in control_body.get("zones", []) or []:
                if isinstance(cz, dict) and "zone" in cz:
                    try:
                        control_zones_by_idx[int(cz["zone"])] = cz
                    except (TypeError, ValueError):
                        continue

        for ez in exec_zones:
            if not isinstance(ez, dict) or "zone" not in ez:
                continue
            try:
                zi = int(ez["zone"])
            except (TypeError, ValueError):
                continue
            control_mode = ez.get("control_mode")
            try:
                control_mode = int(control_mode)
            except (TypeError, ValueError):
                control_mode = -1

            error_c = None
            rate = None
            cz = control_zones_by_idx.get(zi)
            if control_mode == 3 and cz is not None and target_c is not None:
                actual_c = cz.get("actual_c")
                pid_d = cz.get("pid_d")
                bd_kd_effective = cz.get("bd_kd_effective")
                if (
                    isinstance(actual_c, (int, float))
                    and isinstance(pid_d, (int, float))
                    and isinstance(bd_kd_effective, (int, float))
                    and isinstance(target_c, (int, float))
                    and bd_kd_effective != 0.0
                ):
                    error_c = float(target_c) - float(actual_c)
                    rate = float(pid_d) / float(bd_kd_effective)

            out.append(ZoneTick(zone=zi, control_mode=control_mode,
                                 error_c=error_c, rate_c_per_s=rate))
    return out


def summarize_capture(path: str) -> "dict[int, CaptureZoneSummary]":
    """Per-zone summary for one capture file -- the basis of the mode-2
    refusal/loud-flag. A zone whose ``control_modes_seen`` is anything other
    than exactly ``{3}`` gets ``n_usable_samples == 0`` for every tick where
    it wasn't in mode 3, and ``reason_excluded`` is set when NO usable
    samples exist for that zone at all."""
    ticks = iter_zone_ticks(path)
    by_zone: "dict[int, list[ZoneTick]]" = defaultdict(list)
    for t in ticks:
        by_zone[t.zone].append(t)

    out: "dict[int, CaptureZoneSummary]" = {}
    for zone, zone_ticks in by_zone.items():
        modes_seen = {t.control_mode for t in zone_ticks}
        n_mode3 = sum(1 for t in zone_ticks if t.control_mode == 3)
        n_usable = sum(1 for t in zone_ticks
                       if t.control_mode == 3 and t.error_c is not None and t.rate_c_per_s is not None)
        reason = None
        if n_usable == 0:
            if modes_seen == {3} or 3 in modes_seen:
                reason = (
                    f"zone {zone}: control_mode reached 3 but no sample had a reconstructable "
                    "rate (missing \"control\" body, or bd_kd_effective==0/missing throughout) "
                    "-- 0 usable samples")
            else:
                reason = (
                    f"zone {zone}: control_mode never reached 3 (modes seen: "
                    f"{sorted(modes_seen)}) -- the fuzzy layer NEVER RAN in this zone for this "
                    "capture. EXCLUDED, 0 samples contributed.")
        out[zone] = CaptureZoneSummary(
            zone=zone, control_modes_seen=modes_seen, n_ticks_total=len(zone_ticks),
            n_mode3_ticks=n_mode3, n_usable_samples=n_usable, reason_excluded=reason,
        )
    return out


# ---------------------------------------------------------------------------
# The band probe itself.
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class GainDeltaStats:
    """Fractional change of one gain (kp/ki/kd) between the CANDIDATE band's
    output and the REFERENCE band's output, ``abs(candidate - reference) /
    base_gain``, across every usable sample -- NOT just at the dominant cell
    (see module docstring: membership is a continuous blend, so the
    dominant cell can stay ZERO/STEADY while gains move materially).

    This is deliberately a candidate-vs-REFERENCE-band comparison, not a
    candidate-vs-base-gain one: at error exactly 0.0 (a well-settled dwell
    sample, common in a real capture) the ZERO row's FALLING and STEADY
    cells share the same kp direction (-1, see RULE_TABLE), so kp lands at
    the FULL strength ceiling (e.g. -25% at strength=50) independent of the
    rate band's width -- a vs-base metric would report that near-ceiling
    swing for every candidate band including the reference band itself,
    drowning out the actual band-to-band difference this tool exists to
    show. Differencing against the reference band's own output at the same
    sample cancels that shared saturation and isolates what actually moves
    when the band changes."""
    max_abs: float
    mean_abs: float
    n: int


@dataclasses.dataclass
class ZoneBandReport:
    zone: int
    n_samples: int
    cell_occupancy: "dict[str, int]"  # "ERROR_BUCKET/RATE_BUCKET" -> count (dominant-bucket basis, candidate band)
    kp: GainDeltaStats
    ki: GainDeltaStats
    kd: GainDeltaStats


def _pct_occupancy(counts: "dict[str, int]", n: int) -> "dict[str, float]":
    if n == 0:
        return {}
    return {k: v / n for k, v in counts.items()}


def probe_zone(
    samples: "Sequence[tuple[float, float]]",
    base_kp: float, base_ki: float, base_kd: float,
    strength_pct: int,
    error_band_c: float, rate_band_c_per_s: float,
    ref_error_band_c: float, ref_rate_band_c_per_s: float,
) -> ZoneBandReport:
    """Run every (error_c, rate_c_per_s) sample through pid_fuzzy_adjust()
    at both the candidate band and the reference band, report cell
    occupancy (dominant-bucket basis, candidate band) and the fractional
    gain-change stats between the two outputs (see ``GainDeltaStats``)."""
    occ: "dict[str, int]" = defaultdict(int)
    kp_deltas = []
    ki_deltas = []
    kd_deltas = []

    band_e = error_band_c if (math.isfinite(error_band_c) and error_band_c > 0.0) else ERROR_BAND_C_DEFAULT
    band_r = rate_band_c_per_s if (math.isfinite(rate_band_c_per_s) and rate_band_c_per_s > 0.0) else RATE_BAND_C_PER_S_DEFAULT

    for error_c, rate in samples:
        e_deg = triangular_memberships(error_c, band_e)
        r_deg = triangular_memberships(rate, band_r)
        cell = f"{BUCKET_NAMES[dominant_bucket_index(e_deg)]}/{BUCKET_NAMES[dominant_bucket_index(r_deg)]}"
        occ[cell] += 1

        kp_c, ki_c, kd_c = pid_fuzzy_adjust(error_c, rate, error_band_c, rate_band_c_per_s,
                                            base_kp, base_ki, base_kd, strength_pct)
        kp_r, ki_r, kd_r = pid_fuzzy_adjust(error_c, rate, ref_error_band_c, ref_rate_band_c_per_s,
                                            base_kp, base_ki, base_kd, strength_pct)
        if base_kp > 0:
            kp_deltas.append(abs(kp_c - kp_r) / base_kp)
        if base_ki > 0:
            ki_deltas.append(abs(ki_c - ki_r) / base_ki)
        if base_kd > 0:
            kd_deltas.append(abs(kd_c - kd_r) / base_kd)

    def _stats(vals):
        if not vals:
            return GainDeltaStats(max_abs=0.0, mean_abs=0.0, n=0)
        return GainDeltaStats(max_abs=max(vals), mean_abs=sum(vals) / len(vals), n=len(vals))

    return ZoneBandReport(
        zone=-1, n_samples=len(samples), cell_occupancy=dict(occ),
        kp=_stats(kp_deltas), ki=_stats(ki_deltas), kd=_stats(kd_deltas),
    )


@dataclasses.dataclass
class BandProbeReport:
    paths: "list[str]"
    error_band_c: float
    rate_band_c_per_s: float
    ref_error_band_c: float
    ref_rate_band_c_per_s: float
    #: per-file, per-zone loud exclusion notes (the mode-2 guard's output)
    exclusions: "list[str]"
    #: zone -> ZoneBandReport, pooled across every INCLUDED file
    per_zone: "dict[int, ZoneBandReport]"
    total_usable_samples: int


def probe_capture_files(
    paths: Sequence[str],
    base_gains: "dict[int, tuple[float, float, float]]",
    strength_pct: "dict[int, int]",
    error_band_c: float,
    rate_band_c_per_s: float,
    ref_error_band_c: float = ERROR_BAND_C_DEFAULT,
    ref_rate_band_c_per_s: float = RATE_BAND_C_PER_S_DEFAULT,
) -> BandProbeReport:
    """Top-level entry point: refuse/flag inert (non mode-3) data per file
    per zone, then run the surviving samples through ``probe_zone`` at the
    candidate band.

    ``base_gains``/``strength_pct`` are keyed by zone index -- pass the
    values from the zone-config preset that was live for the capture(s)
    being analyzed (e.g. a ``kilnctl_config_preset`` JSON, see
    ``load_base_gains_from_preset``). This module does not guess base gains
    from telemetry: ``bd_kp_effective`` is the FUZZY-ADJUSTED output, not the
    base value, and back-deriving the base from it would silently assume the
    exact band/strength already in effect when the capture was made --
    exactly the kind of confident-but-wrong shortcut this tool exists to
    avoid.
    """
    exclusions: "list[str]" = []
    samples_by_zone: "dict[int, list[tuple[float, float]]]" = defaultdict(list)
    total_usable = 0

    for path in paths:
        zone_summaries = summarize_capture(path)
        ticks = iter_zone_ticks(path)
        by_zone: "dict[int, list[ZoneTick]]" = defaultdict(list)
        for t in ticks:
            by_zone[t.zone].append(t)

        for zone, summary in zone_summaries.items():
            if summary.reason_excluded is not None:
                exclusions.append(f"{path}: {summary.reason_excluded}")
                continue
            if zone not in base_gains:
                exclusions.append(
                    f"{path}: zone {zone} has {summary.n_usable_samples} usable mode-3 samples "
                    "but no base-gain entry was supplied for it -- EXCLUDED (pass --base-gains "
                    "covering every zone in the capture)")
                continue
            for t in by_zone[zone]:
                if t.control_mode == 3 and t.error_c is not None and t.rate_c_per_s is not None:
                    samples_by_zone[zone].append((t.error_c, t.rate_c_per_s))
            total_usable += summary.n_usable_samples

    per_zone: "dict[int, ZoneBandReport]" = {}
    for zone, samples in samples_by_zone.items():
        base_kp, base_ki, base_kd = base_gains[zone]
        pct = strength_pct.get(zone, 0)
        report = probe_zone(samples, base_kp, base_ki, base_kd, pct,
                            error_band_c, rate_band_c_per_s,
                            ref_error_band_c, ref_rate_band_c_per_s)
        report.zone = zone
        per_zone[zone] = report

    return BandProbeReport(
        paths=list(paths), error_band_c=error_band_c, rate_band_c_per_s=rate_band_c_per_s,
        ref_error_band_c=ref_error_band_c, ref_rate_band_c_per_s=ref_rate_band_c_per_s,
        exclusions=exclusions, per_zone=per_zone, total_usable_samples=total_usable,
    )


def load_base_gains_from_preset(path: str):
    """Read a ``kilnctl_config_preset`` JSON (e.g.
    ``tools/PcTools/config_presets/fuzzy_ab_strength50_20260903.json``) and
    return ``(base_gains, strength_pct)``, both ``{zone_index: value}``,
    for every zone the preset lists."""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    base_gains: "dict[int, tuple[float, float, float]]" = {}
    strength_pct: "dict[int, int]" = {}
    for z in data.get("zones", []):
        idx = int(z["index"])
        base_gains[idx] = (float(z["pid_kp"]), float(z["pid_ki"]), float(z["pid_kd"]))
        strength_pct[idx] = int(round(float(z.get("fuzzy_strength_pct", 0))))
    return base_gains, strength_pct


def format_report(report: BandProbeReport) -> str:
    lines = [
        f"fuzzy band probe: {report.paths}",
        f"candidate band: error_band_c={report.error_band_c}, "
        f"rate_band_c_per_s={report.rate_band_c_per_s}",
        f"reference band: error_band_c={report.ref_error_band_c}, "
        f"rate_band_c_per_s={report.ref_rate_band_c_per_s}",
        "",
    ]
    if report.exclusions:
        lines.append("EXCLUDED (loud, not silent):")
        for e in report.exclusions:
            lines.append(f"  - {e}")
        lines.append("")
    if not report.per_zone:
        lines.append("NO USABLE SAMPLES -- every zone in every capture was excluded above. "
                     "Refusing to report a band recommendation from zero mode-3 data.")
        return "\n".join(lines)

    lines.append(f"total usable (mode-3, reconstructable-rate) samples: {report.total_usable_samples}")
    lines.append("")
    for zone in sorted(report.per_zone):
        r = report.per_zone[zone]
        lines.append(f"zone {zone}: n={r.n_samples}")
        occ_pct = _pct_occupancy(r.cell_occupancy, r.n_samples)
        for cell, frac in sorted(occ_pct.items(), key=lambda kv: -kv[1]):
            lines.append(f"    cell {cell}: {frac*100:.1f}% ({r.cell_occupancy[cell]})")
        lines.append(
            f"    fractional gain delta vs reference band, |candidate-reference|/base -- "
            f"kp: max={r.kp.max_abs:.3f} mean={r.kp.mean_abs:.3f}  "
            f"ki: max={r.ki.max_abs:.3f} mean={r.ki.mean_abs:.3f}  "
            f"kd: max={r.kd.max_abs:.3f} mean={r.kd.mean_abs:.3f}")

    kp_over = max((r.kp.max_abs for r in report.per_zone.values()), default=0.0)
    ki_over = max((r.ki.max_abs for r in report.per_zone.values()), default=0.0)
    kd_over = max((r.kd.max_abs for r in report.per_zone.values()), default=0.0)
    lines.append("")
    lines.append(
        f"OVERALL (max across included zones) fractional gain delta vs reference band: "
        f"kp={kp_over:.3f} ki={ki_over:.3f} kd={kd_over:.3f}")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Offline fuzzy-PID membership-band probe: recompute cell occupancy and "
                    "fractional gain deltas for a candidate (error_band_c, rate_band_c_per_s) "
                    "from already-recorded capture(s), with NO kiln time. Refuses to pool "
                    "control_mode != 3 (non-fuzzy) samples -- see module docstring.")
    parser.add_argument("captures", nargs="+", help="one or more HTTP-capture .jsonl files")
    parser.add_argument("--error-band", type=float, required=True, dest="error_band_c")
    parser.add_argument("--rate-band", type=float, required=True, dest="rate_band_c_per_s")
    parser.add_argument("--ref-error-band", type=float, default=ERROR_BAND_C_DEFAULT,
                        dest="ref_error_band_c",
                        help=f"reference band to diff gain deltas against (default: the "
                             f"shipped firmware default, {ERROR_BAND_C_DEFAULT})")
    parser.add_argument("--ref-rate-band", type=float, default=RATE_BAND_C_PER_S_DEFAULT,
                        dest="ref_rate_band_c_per_s",
                        help=f"default: {RATE_BAND_C_PER_S_DEFAULT}")
    parser.add_argument("--base-gains", required=True,
                        help="kilnctl_config_preset JSON providing per-zone pid_kp/ki/kd and "
                             "fuzzy_strength_pct (e.g. the preset live when the capture(s) were made)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    base_gains, strength_pct = load_base_gains_from_preset(args.base_gains)
    report = probe_capture_files(args.captures, base_gains, strength_pct,
                                  args.error_band_c, args.rate_band_c_per_s,
                                  args.ref_error_band_c, args.ref_rate_band_c_per_s)

    if args.json:
        print(json.dumps(dataclasses.asdict(report), indent=2, default=str))
    else:
        print(format_report(report))

    return 0 if report.per_zone else 1


if __name__ == "__main__":
    raise SystemExit(main())
