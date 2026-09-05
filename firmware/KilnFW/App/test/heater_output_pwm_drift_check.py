#!/usr/bin/env python3
"""heater_output_pwm_drift_check.py -- proves that
``tools/PcTools/src/kilnctrl/plant_sim.py``'s ``_pwm_render()`` (documented
in its own module as "a line-for-line port of ``heater_output_duty_ex``'s
ordinary, non-``force_new_window`` path ... translated line for line from
``heater_output.c``") still agrees NUMERICALLY, tick for tick, with the
real, unmodified ``firmware/KilnFW/App/drivers/heater_output.c``'s
``heater_output_duty()``.

WHY THIS CHECK DID NOT EXIST UNTIL NOW. ``_pwm_render`` is exactly the same
class of mirror ``pid_fuzzy_drift_check.py`` was written to catch drift in
(see that script's docstring and ``dcc3aca``): a hand-ported, "faithfully
mirrors" Python translation of firmware control logic, with no numerical
tie back to the C it claims to mirror -- only behavioural unit tests in
``tests/test_plant_sim.py`` that assert properties BOTH sides are assumed to
share (e.g. "a below-floor on-time renders as off"), never a byte-for-byte
comparison against the real ``heater_output.c``. Unlike the fuzzy case this
mirror does not (yet) feed a config-schema value that can silently change
out from under it; the risk here is a translation bug in the port itself.
One is already visible on inspection: ``heater_output.c`` computes
``on_ms = (uint32_t)(duty * (float)cfg->window_ms)`` -- a truncating
integer cast every window boundary -- while ``_pwm_render`` computes
``on_ms = duty * window_ms`` in Python float and never truncates. This
check's vectors are chosen specifically to land duty*window_ms on a
non-integer value close to a quantization threshold, so that a real
divergence from the missing truncation would fail LOUDLY rather than by
coincidence agreeing at round numbers.

CONSEQUENCE IF THIS DRIFTS UNCAUGHT. ``_pwm_render`` feeds
``plant_sim.run_profile``'s PWM-window model, which the gain search
(PID_EXPANSION_PLAN.md sec 3.4) uses to score candidate gains on relay
chatter/cycle life -- a plausible-but-wrong relay schedule would silently
bias which gains look safe, the same "produces plausible-but-wrong output"
danger class the fuzzy mirror was in, not a class that fails obviously.

MECHANISM (same shape as pid_fuzzy_drift_check.py):
  1. Builds ``heater_output_pwm_drift_harness.c`` (this directory), which
     links the REAL, unmodified ``../drivers/heater_output.c`` (no
     FreeRTOS/ESP-IDF dependency -- see that file's own header comment) and
     drives ``heater_output_duty()`` tick by tick from stdin vectors.
  2. Generates a table of (window_ms, min_on_ms, min_off_ms, duty, dt_ms,
     n_ticks) vectors covering: default timing at several duties (below
     floor, exactly at a truncation-sensitive threshold, near-symmetric
     high duty, mid duty), a shorter/longer window, a non-default
     min_on_ms/min_off_ms pair, and enough ticks per vector to cross
     several window boundaries (so the RUNNING min-on hold across a
     boundary is exercised, not just one window's quantization).
  3. Feeds the identical vectors, tick by tick, to
     ``plant_sim._pwm_render`` in-process, starting from a fresh
     ``_PwmZoneState`` per vector (matching the harness's fresh
     ``heater_output_state_t`` per vector).
  4. Asserts every tick's (relay_on, cycle_count) pair agrees, failing
     loudly with the vector, tick index, and both sides' values at the
     first divergence.

LIMITS: this compares only ``heater_output_duty()``'s ordinary
(``force_new_window=False``) path, matching what ``_pwm_render`` itself
claims to port -- ``heater_output_duty_relay_step``'s early-window variant
(bang-bang-mode fuzzy relay law) is explicitly out of scope for both sides,
per ``_pwm_render``'s own module comment.

Usage: python heater_output_pwm_drift_check.py [repo_root]
Exit 0: every sampled tick agreed across every vector.
Exit 1: the harness failed to build/run, or at least one tick diverged
        (named with the vector, tick index, and both sides' values).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
HARNESS_SRC = TEST_DIR / "heater_output_pwm_drift_harness.c"
HEATER_OUTPUT_C = TEST_DIR / ".." / "drivers" / "heater_output.c"
BUILD_DIR = TEST_DIR / "build"
HARNESS_EXE = BUILD_DIR / "heater_output_pwm_drift_harness.exe"
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
    bat_path = BUILD_DIR / "_heater_output_pwm_drift_build.bat"
    bat_path.write_text(
        "@echo off\r\n"
        f'call "{VCVARS}" x64 >nul\r\n'
        f'cl /nologo /W3 /std:c11 /Fo:"{BUILD_DIR}\\\\" /Fe:"{HARNESS_EXE}" '
        f'"{HARNESS_SRC}" "{HEATER_OUTPUT_C}"\r\n',
        encoding="utf-8",
    )
    try:
        result = subprocess.run(["cmd.exe", "/c", str(bat_path)],
                                 capture_output=True, text=True, cwd=str(BUILD_DIR))
    finally:
        bat_path.unlink(missing_ok=True)
    if result.returncode != 0 or not HARNESS_EXE.exists():
        sys.stderr.write("HEATER_OUTPUT PWM DRIFT CHECK: harness build failed.\n")
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return False
    return True


def build_all_vectors():
    """(window_ms, min_on_ms, min_off_ms, duty, dt_ms, n_ticks)."""
    vecs = []
    dt_ms = 1000.0  # 1 s tick, matching plant_sim's usual DT-derived tick

    # Default timing (0 => firmware substitutes HEATER_DEFAULT_MIN_ON_MS/
    # HEATER_DEFAULT_MIN_OFF_MS is a CALLER-side convention in plant_sim,
    # not inside heater_output.c itself -- the harness is given the
    # resolved values directly, matching what _pwm_render's own floor
    # inside the function does).
    window_ms = 60000.0
    min_on_ms = 10000.0   # HEATER_MIN_ON_MS_FLOOR
    min_off_ms = 2000.0   # HEATER_DEFAULT_MIN_OFF_MS

    # Duties chosen to land on_ms = duty*window_ms on a non-integer value
    # right at/near the min_on and min_off quantization thresholds, where a
    # missing truncating cast is most likely to flip the on/off decision:
    #   min_on_ms/window_ms   = 10000/60000 = 0.16666...
    #   1 - min_off_ms/window_ms = 58000/60000 = 0.96666...
    duties = (
        0.0, 0.05,                     # below floor -> whole window off
        0.16666, 0.166667, 0.1666668,  # straddle the min_on threshold
        0.5,                           # ordinary mid duty
        0.9666, 0.966667, 0.966668,    # straddle the min_off threshold
        0.97, 1.0,                     # near/at full duty
    )
    for d in duties:
        # 5 windows' worth of ticks so a window boundary (and the running
        # min-on hold across it) is actually exercised, not just window 0.
        n_ticks = int(5 * window_ms / dt_ms)
        vecs.append((window_ms, min_on_ms, min_off_ms, d, dt_ms, n_ticks))

    # Non-default window/timing pair, same duty sweep at coarser sampling.
    window_ms2 = 30000.0
    min_on_ms2 = 12000.0
    min_off_ms2 = 3000.0
    for d in (0.1, 12000.0 / 30000.0, 0.5, 1.0 - 3000.0 / 30000.0, 0.9):
        n_ticks = int(4 * window_ms2 / dt_ms)
        vecs.append((window_ms2, min_on_ms2, min_off_ms2, d, dt_ms, n_ticks))

    # A sub-1s tick (dt_ms=250), which changes how many ticks land exactly
    # on a window boundary -- a different quantization stress than the 1 s
    # cases above.
    for d in (0.16666, 0.5, 0.9666):
        n_ticks = int(3 * window_ms / 250.0)
        vecs.append((window_ms, min_on_ms, min_off_ms, d, 250.0, n_ticks))

    return vecs


def run_harness(vectors):
    stdin_text = "\n".join(
        f"{w!r} {mn!r} {mf!r} {d!r} {dt!r} {n}" for (w, mn, mf, d, dt, n) in vectors
    ) + "\n"
    result = subprocess.run([str(HARNESS_EXE)], input=stdin_text, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"harness exited {result.returncode}: {result.stderr}")
    lines = [ln for ln in result.stdout.splitlines() if ln.strip()]
    total_expected = sum(n for (_, _, _, _, _, n) in vectors)
    if len(lines) != total_expected:
        raise RuntimeError(
            f"harness printed {len(lines)} lines for {total_expected} expected ticks -- "
            "a parse failure in the harness silently dropped ticks."
        )
    idx = 0
    per_vector = []
    for (_, _, _, _, _, n) in vectors:
        block = lines[idx:idx + n]
        idx += n
        parsed = []
        for ln in block:
            on_s, cyc_s = ln.split()
            parsed.append((int(on_s) != 0, int(cyc_s)))
        per_vector.append(parsed)
    return per_vector


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    repo_root = Path(argv[0]).resolve() if argv else _default_repo_root()

    sys.path.insert(0, str(repo_root / "tools" / "PcTools" / "src"))
    try:
        from kilnctrl.plant_sim import _pwm_render, _PwmZoneState
    except ImportError as exc:
        sys.stderr.write(f"HEATER_OUTPUT PWM DRIFT CHECK: could not import plant_sim: {exc}\n")
        return 1

    if not build_harness():
        return 1

    vectors = build_all_vectors()
    try:
        c_results = run_harness(vectors)
    except RuntimeError as exc:
        sys.stderr.write(f"HEATER_OUTPUT PWM DRIFT CHECK: {exc}\n")
        return 1

    n_ticks_checked = 0
    for vi, ((window_ms, min_on_ms, min_off_ms, duty, dt_ms, n_ticks), c_ticks) in enumerate(
        zip(vectors, c_results)
    ):
        state = _PwmZoneState()
        for ti, (c_on, c_cyc) in enumerate(c_ticks):
            py_on = _pwm_render(state, duty, window_ms, min_on_ms, min_off_ms, dt_ms)
            py_cyc = state.cycle_count
            n_ticks_checked += 1
            if py_on != c_on or py_cyc != c_cyc:
                sys.stderr.write(
                    f"HEATER_OUTPUT PWM DRIFT CHECK: FAILED at vector #{vi} tick #{ti}\n"
                    f"  inputs: window_ms={window_ms} min_on_ms={min_on_ms} "
                    f"min_off_ms={min_off_ms} duty={duty} dt_ms={dt_ms}\n"
                    f"  C: relay_on={c_on} cycle_count={c_cyc}\n"
                    f"  Python (_pwm_render): relay_on={py_on} cycle_count={py_cyc}\n"
                )
                return 1

    print(f"HEATER_OUTPUT PWM DRIFT CHECK: {len(vectors)} vectors / {n_ticks_checked} ticks "
          "agreed exactly (relay_on and cycle_count, every tick).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
