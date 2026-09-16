#!/usr/bin/env python3
"""pid_fuzzy_drift_check.py -- proves that
``tools/PcTools/src/kilnctrl/fuzzy_band_probe.py``'s ``pid_fuzzy_adjust()``
(the sole Python port of this repo's fuzzy-PID math, since ``plant_sim.py``
was changed to import it rather than carry its own second hand copy) still
agrees NUMERICALLY with the real, unmodified
``firmware/KilnFW/App/drivers/control/pid_fuzzy.c``'s ``pid_fuzzy_adjust()``.

WHY NUMERICAL EQUIVALENCE, NOT A TEXTUAL DIFF (the technique
``approach_rate_cap_mirror_drift_check.py``/``power_diag_flag_mirror_drift_
check.py`` use). Those checks extract a C fragment and a hand-mirrored C (or
near-C) fragment and diff them after normalizing a small, enumerable set of
structurally-required differences -- viable because both sides are
basically the same language shape. Here one side is C using a for-loop over
a 3x3 float array with pointer outputs, and the other is Python using tuple
returns and library math functions; forcing those into a line-by-line
textual match would mean normalizing away almost the entire function body
(loop syntax, output-parameter vs. return, float suffixes, brace
placement), leaving a diff that "passes" by comparing almost nothing. A
textual diff that has been hollowed out to survive its own two
implementations is worse than no diff at all: it would look like a real
drift check while actually validating chosen substrings, not behaviour.
Numerical equivalence checks the thing that actually matters -- do the two
implementations compute the SAME GAINS for the SAME INPUTS -- and is honest
about what class of drift it can and cannot catch (see LIMITS below).

MECHANISM. This script:
  1. Builds ``pid_fuzzy_drift_harness.c`` (this directory), which links the
     REAL, unmodified ``../drivers/control/pid_fuzzy.c`` -- no stub tree needed,
     since pid_fuzzy.c has zero FreeRTOS/ESP-IDF/logging dependencies (see
     pid_fuzzy.h's own header comment). The harness reads 8-float test
     vectors from stdin and prints ``pid_fuzzy_adjust()``'s three output
     gains per line.
  2. Generates a fixed table of test vectors covering: the strength=0
     safety short-circuit (with both legitimate and NaN/negative/inf base
     gains), the non-finite-input short-circuit, every one of the 9
     rule-table cells at default (20.0/0.5) bands, AND -- the entire point
     of this check existing -- the same cells again at the non-default band
     envelope now under consideration for the owner's kiln (6-8 C error
     band, 0.20-0.25 C/s rate band), plus a couple of off-grid/asymmetric
     band values to catch a band-plumbing bug that only shows up when
     error_band_c != rate_band_c_per_s's old default ratio.
  3. Feeds the identical vectors to ``fuzzy_band_probe.pid_fuzzy_adjust()``
     in-process.
  4. Asserts every (kp, ki, kd) triple agrees within ``TOLERANCE`` -- loose
     enough to absorb float32 (C) vs. float64 (Python) rounding, tight
     enough that a wrong band, a swapped rule-table cell, or a mis-scaled
     nudge fraction fails LOUDLY, naming the first offending vector.

LIMITS (own up to what this does not prove): this compares the two
functions' INPUT/OUTPUT behaviour at the sampled vectors, not their source
structure -- a change to pid_fuzzy.c that happens to produce the same
outputs at every sampled vector (extremely unlikely for a rule-table or
band change, given the coverage above, but not logically impossible for an
unrelated no-op refactor) would not be flagged. It also does not prove
fuzzy_band_probe.py's other guarantees (band-sentinel resolution done by
the FIRMWARE'S config-accessor layer, not this function) -- only that
``pid_fuzzy_adjust()`` itself, called with the bands as explicit
parameters, matches.

Usage: python pid_fuzzy_drift_check.py [repo_root]
Exit 0: every sampled vector agreed within tolerance.
Exit 1: the harness failed to build/run, or at least one vector diverged
        (the first divergence is named with both sides' outputs).
"""
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

TOLERANCE = 1e-4  # absolute, on gains that are themselves O(1e-2..1e0)

TEST_DIR = Path(__file__).resolve().parent
HARNESS_SRC = TEST_DIR / "pid_fuzzy_drift_harness.c"
DRIVERS_DIR = TEST_DIR.parent / "drivers"
# Private build dir, NOT the shared "build" directory build_host_tests.ps1
# builds into (docs/audits/ki_refusal_truncation_and_drift_check_lock_2026-
# 09-13.md). build_host_tests.ps1 serializes on tools/build_lock.ps1's named
# Mutex before touching that shared tree; this check instead gets its own
# directory so it never contends for that lock at all -- simpler than
# plumbing a PowerShell-only Mutex helper into a Python subprocess (this
# script has no PowerShell parent to dot-source build_lock.ps1 from, and
# re-implementing a cross-process named Mutex in Python just to take the
# SAME lock would add a second implementation of that primitive for no
# benefit, since this check's own build never needs to interleave with the
# host-test build it would otherwise be contending with). A run of two
# `run_all_checks.ps1` invocations, or this check overlapping a host-test
# build, can now proceed concurrently without either side's cl.exe/.obj
# writes colliding.
# Per-process scratch directory. A private build_pid_fuzzy_drift/ alone is
# NOT enough: run_all_checks.ps1 dispatches ~95 checks in parallel, and two
# concurrent copies of THIS check still collide on the fixed pid_fuzzy.obj /
# pid_fuzzy_drift_harness.exe / _pid_fuzzy_drift_build.bat names inside it
# ("fatal error C1083: Cannot open compiler generated file: ...\\pid_fuzzy.obj: Permission denied", reproduced at A exit=1 B exit=1).
# Same defect and same pid-keyed fix as heater_output_pwm_drift_check.py's
# build_heater_output_pwm_drift/run_<pid>/.
BUILD_DIR = TEST_DIR / "build_pid_fuzzy_drift" / f"run_{os.getpid()}"
HARNESS_EXE = BUILD_DIR / "pid_fuzzy_drift_harness.exe"
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"


def _default_repo_root():
    # firmware/KilnFW/App/test -> repo root is four levels up.
    return TEST_DIR.parent.parent.parent.parent


def build_harness():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    if not os.path.isfile(VCVARS):
        raise RuntimeError(f"vcvarsall.bat not found at {VCVARS} -- update this path if MSVC Build Tools moved.")
    if HARNESS_EXE.exists():
        HARNESS_EXE.unlink()
    pid_fuzzy_c = resolve_driver_file(None, "pid_fuzzy.c", drivers_dir=DRIVERS_DIR)
    bat_path = BUILD_DIR / "_pid_fuzzy_drift_build.bat"
    bat_path.write_text(
        "@echo off\r\n"
        f'call "{VCVARS}" x64 >nul\r\n'
        f'cl /nologo /W3 /std:c11 /Fo:"{BUILD_DIR}\\\\" /Fe:"{HARNESS_EXE}" '
        f'"{HARNESS_SRC}" "{pid_fuzzy_c}"\r\n',
        encoding="utf-8",
    )
    try:
        result = subprocess.run(["cmd.exe", "/c", str(bat_path)],
                                 capture_output=True, text=True, cwd=str(BUILD_DIR))
    finally:
        bat_path.unlink(missing_ok=True)
    if result.returncode != 0 or not HARNESS_EXE.exists():
        sys.stderr.write("PID_FUZZY DRIFT CHECK: harness build failed.\n")
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return False
    return True


def gen_vectors(error_band_c, rate_band_c_per_s, strengths=(0, 25, 50, 100)):
    """9 rule-table cells (error x rate each in {-2*band, 0, 2*band}, which
    always lands past a triangular membership's saturation edge or exactly
    on the zero crossing) x each requested strength, at the given bands."""
    errs = (-2.0 * error_band_c, 0.0, 2.0 * error_band_c, -0.37 * error_band_c, 0.61 * error_band_c)
    rates = (-2.0 * rate_band_c_per_s, 0.0, 2.0 * rate_band_c_per_s, 0.44 * rate_band_c_per_s)
    vecs = []
    for e in errs:
        for r in rates:
            for s in strengths:
                vecs.append((e, r, error_band_c, rate_band_c_per_s, 0.06, 0.0003, 0.01, s))
    return vecs


def build_all_vectors():
    vecs = []
    # Firmware default band (pre-904db54 behaviour, and any zone that never
    # touches the new config).
    vecs += gen_vectors(20.0, 0.5)
    # The exact envelope now being recommended to the owner -- the whole
    # reason this check exists. Both corners of each range, plus asymmetric
    # combinations (tight error band with the loose end of the rate band and
    # vice versa) so a band-swap bug (error_band_c and rate_band_c_per_s fed
    # to the wrong axis) cannot hide behind a coincidentally-symmetric test.
    for eb in (6.0, 7.0, 8.0):
        for rb in (0.20, 0.225, 0.25):
            vecs += gen_vectors(eb, rb)
    # strength_pct == 0 must be bit-exact-in-tolerance regardless of band,
    # including with a NaN/negative/inf base gain (the sanitize-at-zero
    # contract test_pid_fuzzy.c pins).
    for eb, rb in ((20.0, 0.5), (6.0, 0.2)):
        vecs.append((10.0, 0.1, eb, rb, float("nan"), -1.0, float("inf"), 0))
    # Non-finite error/rate short-circuit, at a non-default band.
    vecs.append((float("nan"), 0.1, 7.0, 0.22, 0.02, 0.001, 0.03, 80))
    vecs.append((5.0, float("inf"), 7.0, 0.22, 0.02, 0.001, 0.03, 80))
    return vecs


def fmt(v):
    if v != v:  # NaN
        return "nan"
    if v == float("inf"):
        return "inf"
    if v == float("-inf"):
        return "-inf"
    return repr(v)


def run_harness(vectors):
    stdin_text = "\n".join(
        " ".join(fmt(x) for x in vec) for vec in vectors
    ) + "\n"
    result = subprocess.run([str(HARNESS_EXE)], input=stdin_text, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"harness exited {result.returncode}: {result.stderr}")
    lines = [ln for ln in result.stdout.splitlines() if ln.strip()]
    if len(lines) != len(vectors):
        raise RuntimeError(
            f"harness printed {len(lines)} lines for {len(vectors)} input vectors -- "
            "a parse failure in the harness silently dropped a vector."
        )
    out = []
    for ln in lines:
        parts = ln.split()
        out.append(tuple(float(p) for p in parts))
    return out


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    repo_root = Path(argv[0]).resolve() if argv else _default_repo_root()

    sys.path.insert(0, str(repo_root / "tools" / "PcTools" / "src"))
    try:
        from kilnctrl.fuzzy_band_probe import pid_fuzzy_adjust as py_pid_fuzzy_adjust
    except ImportError as exc:
        sys.stderr.write(f"PID_FUZZY DRIFT CHECK: could not import fuzzy_band_probe: {exc}\n")
        return 1

    if not build_harness():
        return 1

    vectors = build_all_vectors()
    try:
        c_results = run_harness(vectors)
    except RuntimeError as exc:
        sys.stderr.write(f"PID_FUZZY DRIFT CHECK: {exc}\n")
        return 1

    worst = 0.0
    for i, (vec, c_out) in enumerate(zip(vectors, c_results)):
        error_c, error_rate, eb, rb, base_kp, base_ki, base_kd, strength = vec
        py_out = py_pid_fuzzy_adjust(error_c, error_rate, eb, rb, base_kp, base_ki, base_kd, strength)
        for name, cv, pv in zip(("kp", "ki", "kd"), c_out, py_out):
            if math.isnan(cv) or math.isnan(pv):
                if not (math.isnan(cv) and math.isnan(pv)):
                    sys.stderr.write(
                        f"PID_FUZZY DRIFT CHECK: FAILED at vector #{i} {vec}\n"
                        f"  C {name}={cv!r}, Python {name}={pv!r} (NaN mismatch)\n"
                    )
                    return 1
                continue
            diff = abs(cv - pv)
            worst = max(worst, diff)
            if diff > TOLERANCE:
                sys.stderr.write(
                    f"PID_FUZZY DRIFT CHECK: FAILED at vector #{i}\n"
                    f"  inputs: error_c={error_c} error_rate={error_rate} "
                    f"error_band_c={eb} rate_band_c_per_s={rb} "
                    f"base=({base_kp},{base_ki},{base_kd}) strength_pct={strength}\n"
                    f"  C {name}={cv!r}, Python {name}={pv!r}, |diff|={diff!r} > tolerance {TOLERANCE}\n"
                )
                return 1

    print(f"PID_FUZZY DRIFT CHECK: {len(vectors)} vectors agreed within tolerance "
          f"(worst |diff| = {worst!r}, tolerance = {TOLERANCE}).")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    finally:
        # Per-process scratch directory: remove it so repeated runs do not
        # accumulate one run_<pid> tree per invocation.
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
