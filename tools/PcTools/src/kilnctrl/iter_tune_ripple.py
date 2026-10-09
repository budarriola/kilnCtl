"""Offline, diagnostic-only sensor-tau estimator from PWM-window ripple.

Reads a recorded firing capture (the JSONL format firing_score_from_capture
reads: one object per line with "t" and "exec": {"dwelling","segment_index",
"zones":[{"actual_c","actual_valid","duty"}]}), and for each dwell segment and
zone: detrends the measured temperature, measures ripple amplitude at the PWM
fundamental, compares it with the amplitude a no-lag first-order model
predicts from the duty cycle, and reports the implied first-order sensor tau

    tau = (1/omega) * sqrt((A_model/A_meas)^2 - 1)

with a 95% confidence interval across windows.

A_model = G * (2/pi) * sin(pi*D): the fundamental of a 0/1 PWM of duty D times
G, the steady rise in C per unit duty (estimated from the dwell unless given).

DIAGNOSTIC ONLY: never writes any tune or config. Refuses (exit 4) any capture
whose id is in the sec 6.5 gate set
(docs/audits/credibility_gate_dwell_peak_2026-10-07.md): fitting a parameter
to the gate captures is circular.
"""
from __future__ import annotations

import json
import math
import os
from typing import List, Optional, Sequence, Tuple

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_GATE_REFUSED = 4

SNR_MIN = 4.0
# two-sided 95% Student-t critical values by degrees of freedom
_T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
        8: 2.306, 9: 2.262, 10: 2.228, 15: 2.131, 20: 2.086, 30: 2.042}


def _t95(df: int) -> float:
    if df <= 0:
        return float("nan")
    if df in _T95:
        return _T95[df]
    for k in sorted(_T95):
        if df < k:
            return _T95[k]
    return 1.96


def capture_id(path: str) -> str:
    return os.path.splitext(os.path.basename(path))[0]


def is_gate_capture(path: str, gate_ids: Sequence[str]) -> bool:
    base = os.path.basename(path)
    cid = capture_id(path)
    ids = set(gate_ids)
    return cid in ids or base in ids or cid.split(".")[0] in ids


def load_dwell_segments(path: str) -> List[dict]:
    """Dwell runs: {"segment_index", "t":[...], "zones":{z:{"temp","duty","valid"}}}."""
    segs: List[dict] = []
    cur: Optional[dict] = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                ex = d["exec"]
                t = float(d["t"])
            except (ValueError, KeyError, TypeError):
                continue
            if not ex.get("dwelling"):
                cur = None
                continue
            idx = ex.get("segment_index")
            if cur is None or cur["segment_index"] != idx:
                cur = {"segment_index": idx, "t": [], "zones": {}}
                segs.append(cur)
            cur["t"].append(t)
            for z in ex.get("zones", []):
                zn = z.get("zone")
                if zn is None:
                    continue
                zd = cur["zones"].setdefault(zn, {"temp": [], "duty": [], "valid": []})
                zd["temp"].append(float(z.get("actual_c", 0.0)))
                zd["duty"].append(float(z.get("duty", 0.0)))
                zd["valid"].append(bool(z.get("actual_valid", False)))
    return segs


def _solve(a: List[List[float]], b: List[float]) -> Optional[List[float]]:
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def ripple_amplitude(ts: Sequence[float], xs: Sequence[float], omega: float
                     ) -> Optional[Tuple[float, float]]:
    """Least-squares fit [1, sin, cos] over one window; returns (amplitude, se).

    Call on an already-detrended series (see linear_detrend): a free linear
    term inside a single-period window is degenerate with the fundamental.
    """
    n = len(ts)
    if n < 8:
        return None
    t0 = ts[0]
    rows = []
    for t in ts:
        tt = t - t0
        rows.append([1.0, math.sin(omega * tt), math.cos(omega * tt)])
    ata = [[sum(r[i] * r[j] for r in rows) for j in range(3)] for i in range(3)]
    atb = [sum(r[i] * x for r, x in zip(rows, xs)) for i in range(3)]
    beta = _solve(ata, atb)
    if beta is None:
        return None
    res = [x - sum(bk * rk for bk, rk in zip(beta, r)) for r, x in zip(rows, xs)]
    sigma = math.sqrt(sum(e * e for e in res) / max(n - 3, 1))
    amp = math.hypot(beta[1], beta[2])
    se = sigma * math.sqrt(2.0 / n)
    return amp, se


def linear_detrend(ts: Sequence[float], xs: Sequence[float]) -> List[float]:
    """Remove a least-squares line fitted over the whole dwell segment."""
    n = len(ts)
    mt = sum(ts) / n
    mx = sum(xs) / n
    sxx = sum((t - mt) ** 2 for t in ts)
    slope = sum((t - mt) * (x - mx) for t, x in zip(ts, xs)) / sxx if sxx > 0 else 0.0
    return [x - mx - slope * (t - mt) for t, x in zip(ts, xs)]


def model_amplitude(duty: float, gain_c: float) -> float:
    return gain_c * (2.0 / math.pi) * math.sin(math.pi * duty)


def tau_from_amplitudes(a_model: float, a_meas: float, omega: float) -> float:
    r = a_model / a_meas
    return math.sqrt(max(r * r - 1.0, 0.0)) / omega


def estimate_zone(ts: Sequence[float], temp: Sequence[float], duty: Sequence[float],
                  window_s: float, gain_c: Optional[float] = None,
                  ambient_c: Optional[float] = None) -> dict:
    omega = 2.0 * math.pi / window_s
    wins: List[Tuple[int, int]] = []
    s = 0
    for i, t in enumerate(ts):
        if t - ts[s] >= window_s - 1e-9:
            wins.append((s, i))
            s = i
    detr = linear_detrend(ts, temp)
    taus: List[float] = []
    rejected = 0
    for a, b in wins:
        tw, xw, dw = ts[a:b], temp[a:b], duty[a:b]
        d = sum(dw) / len(dw)
        if not 0.05 <= d <= 0.95:
            rejected += 1
            continue
        g = gain_c
        if g is None:
            amb = ambient_c if ambient_c is not None else 25.0
            g = (sum(xw) / len(xw) - amb) / d
        am = ripple_amplitude(tw, detr[a:b], omega)
        if am is None or g <= 0:
            rejected += 1
            continue
        amp, se = am
        if se <= 0 or amp < SNR_MIN * se:
            rejected += 1
            continue
        taus.append(tau_from_amplitudes(model_amplitude(d, g), amp, omega))
    out = {"windows_total": len(wins), "windows_used": len(taus),
           "windows_rejected": rejected}
    if len(taus) < 3:
        out.update(status="INCONCLUSIVE", tau_s=None, ci95_s=None,
                   reason="fewer than 3 windows with ripple above the noise floor")
        return out
    m = sum(taus) / len(taus)
    sd = math.sqrt(sum((x - m) ** 2 for x in taus) / (len(taus) - 1))
    h = _t95(len(taus) - 1) * sd / math.sqrt(len(taus))
    out.update(status="OK", tau_s=m, ci95_s=[m - h, m + h])
    return out


def analyse(path: str, gate_ids: Sequence[str] = (), window_s: float = 60.0,
            gain_c: Optional[float] = None,
            ambient_c: Optional[float] = None) -> Tuple[int, dict]:
    cid = capture_id(path)
    if is_gate_capture(path, gate_ids):
        return EXIT_GATE_REFUSED, {
            "capture_id": cid, "status": "REFUSED_GATE_SET",
            "reason": "capture is in the sec 6.5 credibility-gate set; identifying a "
                      "parameter from it is circular "
                      "(docs/audits/credibility_gate_dwell_peak_2026-10-07.md)"}
    segs = load_dwell_segments(path)
    report: dict = {"capture_id": cid, "window_s": window_s, "diagnostic_only": True,
                    "segments": []}
    if not segs:
        report["status"] = "NO_DWELL"
        return EXIT_OK, report
    for sg in segs:
        entry = {"segment_index": sg["segment_index"], "zones": {}}
        for zn, zd in sorted(sg["zones"].items()):
            if not all(zd["valid"]):
                entry["zones"][str(zn)] = {"status": "INCONCLUSIVE", "tau_s": None,
                                           "ci95_s": None,
                                           "reason": "invalid sensor samples"}
                continue
            entry["zones"][str(zn)] = estimate_zone(
                sg["t"], zd["temp"], zd["duty"], window_s, gain_c, ambient_c)
        report["segments"].append(entry)
    ok = [z for s in report["segments"] for z in s["zones"].values()
          if z["status"] == "OK"]
    report["status"] = "OK" if ok else "INCONCLUSIVE"
    return EXIT_OK, report
