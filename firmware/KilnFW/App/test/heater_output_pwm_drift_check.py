#!/usr/bin/env python3
"""heater_output_pwm_drift_check.py -- checks that
``tools/PcTools/src/kilnctrl/plant_sim.py``'s ``_pwm_render()`` (documented
in its own module as "a line-for-line port of ``heater_output_duty_ex``'s
ordinary, non-``force_new_window`` path ... translated line for line from
``heater_output.c``") agrees NUMERICALLY, tick for tick, with the real,
unmodified ``firmware/KilnFW/App/drivers/control/heater_output.c``'s
``heater_output_duty()`` -- ON THE VECTORS SAMPLED. This is a finite sample
of a continuous input space (duty in particular), not a proof of agreement
everywhere: it is only as strong as ``build_all_vectors()``'s coverage of
that space, which is why that function mixes hand-picked boundary cases,
a seeded adversarial search for float32-vs-double truncation mismatches, and
seeded random fuzzing rather than round-duty hand-picks alone (see below --
an earlier version of this check used only round duties and could not have
caught the truncation-precision bug that motivated the adversarial block).

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
Two have already been found and fixed this way: a missing truncating cast
(``on_ms`` was never truncated to whole milliseconds in Python), and --
found by review after the truncation fix landed -- a precision mismatch:
``heater_output.c`` computes ``on_ms = (uint32_t)(duty * (float)cfg->window_ms)``
with BOTH operands narrowed to float32 and the product evaluated at float32
precision, while the fixed ``_pwm_render`` still truncated a product computed
in C-double precision. The two only disagree within about one float32 ULP of
an integer -- invisible to hand-picked round duties (0.5, 0.9666, ...), which
is why this check's vectors now include a seeded adversarial search that
manufactures duties landing in that ULP band (see ``_adversarial_duties``
below), plus seeded random fuzzing, rather than round duties alone.

CONSEQUENCE IF THIS DRIFTS UNCAUGHT. ``_pwm_render`` feeds
``plant_sim.run_profile``'s PWM-window model, which the gain search
(PID_EXPANSION_PLAN.md sec 3.4) uses to score candidate gains on relay
chatter/cycle life -- a plausible-but-wrong relay schedule would silently
bias which gains look safe, the same "produces plausible-but-wrong output"
danger class the fuzzy mirror was in, not a class that fails obviously.

MECHANISM (same shape as pid_fuzzy_drift_check.py):
  1. Builds ``heater_output_pwm_drift_harness.c`` (this directory), which
     links the REAL, unmodified ``../drivers/control/heater_output.c`` (no
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
import random
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

TEST_DIR = Path(__file__).resolve().parent
HARNESS_SRC = TEST_DIR / "heater_output_pwm_drift_harness.c"
DRIVERS_DIR = TEST_DIR.parent / "drivers"
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
    heater_output_c = resolve_driver_file(None, "heater_output.c", drivers_dir=DRIVERS_DIR)
    bat_path = BUILD_DIR / "_heater_output_pwm_drift_build.bat"
    bat_path.write_text(
        "@echo off\r\n"
        f'call "{VCVARS}" x64 >nul\r\n'
        f'cl /nologo /W3 /std:c11 /Fo:"{BUILD_DIR}\\\\" /Fe:"{HARNESS_EXE}" '
        f'"{HARNESS_SRC}" "{heater_output_c}"\r\n',
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

    # --- Adversarial + randomized duties -----------------------------------
    # The hand-picked duties above are all round-ish (0.5, 0.9666, ...) and
    # cannot see a precision mismatch: heater_output.c computes
    # ``on_ms = (uint32_t)(duty * (float)cfg->window_ms)`` entirely in
    # float32 (both operands narrowed, product evaluated at float32
    # precision), then truncates. A Python port that truncates the same
    # product computed in C-double precision agrees on every round duty --
    # 0.5*60000 is exactly 30000 in both precisions -- and only disagrees
    # when duty*window_ms lands within about one float32 ULP of an integer,
    # which is a near-measure-zero, adversarially-findable set. A reviewer
    # found one by hand: duty=0.5000166666666666, window_ms=60000 truncates
    # to 30001 in float32 and 30000 in double -- a whole tick's difference in
    # the on/off decision. The block below finds a seeded batch of such
    # duties by construction (so it reproduces exactly on failure) rather
    # than relying on hand-picked round numbers to stumble onto one.
    rng = random.Random(20260904)  # fixed seed: failures must reproduce

    def _f32(x: float) -> float:
        return struct.unpack("<f", struct.pack("<f", float(x)))[0]

    def _adversarial_duties(window_ms_: float, dt_ms_: float, count: int) -> list[float]:
        """Duties where truncating ``duty*window_ms`` computed in float32
        (matching the C side, including narrowing duty itself to float32
        the way the ``float duty`` parameter does) disagrees with truncating
        the same product computed in plain double precision -- exactly the
        class of input the old, round-duty-only vector set could not see.

        Disagreeing on ``on_ms`` alone is not enough to fail a tick-by-tick
        replay: ``want_on = window_elapsed_ms < on_ms_this_window`` is only
        sampled at ``dt_ms`` multiples, so a 1 ms difference in ``on_ms``
        only flips an actual tick's relay decision when a ``dt_ms`` multiple
        falls between the two candidate values. Require that too, or the
        vector can "diverge" on paper while every sampled tick still
        agrees."""
        found: list[float] = []
        attempts = 0
        dt_i = int(dt_ms_)
        n_boundaries = max(1, int(window_ms_) // dt_i - 1)
        while len(found) < count and attempts < count * 4000:
            attempts += 1
            # Target a duty whose EXACT (double) on_ms sits right next to a
            # dt_ms-multiple boundary -- that is the only place a 1 ms
            # truncation difference can flip a *sampled* tick's relay
            # decision, per the straddle requirement above. The perturbation
            # has to be sized in DUTY units comparable to duty's own float32
            # ULP (roughly duty * 2**-23, not a tiny fixed ms offset divided
            # by window_ms) -- that is what actually changes which float32
            # value duty rounds to and lets the two precisions disagree on
            # which side of the boundary they land.
            m = rng.randint(1, n_boundaries)
            boundary = m * dt_i
            duty_exact = boundary / window_ms_
            rel = rng.uniform(1e-8, 2e-4) * rng.choice((1.0, -1.0))
            duty = duty_exact + rel
            if not (0.0 < duty < 1.0):
                continue
            duty32 = _f32(duty)
            f32_prod = _f32(duty32 * _f32(window_ms_))
            double_prod = duty * window_ms_
            f32_i, double_i = int(f32_prod), int(double_prod)
            if f32_i == double_i:
                continue
            lo, hi = (f32_i, double_i) if f32_i < double_i else (double_i, f32_i)
            # A sampled tick's want_on = (elapsed < on_ms) only flips between
            # the two precisions if some dt_ms-multiple T satisfies
            # lo <= T < hi. Since hi - lo is typically 1, that requires lo
            # ITSELF to be a dt_ms multiple -- floor-dividing lo and hi-1 by
            # dt_ms and comparing is the exact, off-by-one-safe test (a
            # naive ``lo // dt_i != hi // dt_i`` over-reports: it also fires
            # when the only dt-multiple in range is hi itself, which the
            # half-open interval excludes).
            next_mult = -(-lo // dt_i) * dt_i  # ceil(lo / dt_i) * dt_i
            if next_mult >= hi:
                continue  # no dt-multiple actually falls in [lo, hi)
            found.append(duty)
        return found

    for w in (window_ms, window_ms2):
        for d in _adversarial_duties(w, dt_ms, 8):
            n_ticks = int(3 * w / dt_ms)
            vecs.append((w, min_on_ms if w == window_ms else min_on_ms2,
                         min_off_ms if w == window_ms else min_off_ms2,
                         d, dt_ms, n_ticks))

    # Plain randomized fuzzing, seeded for reproducibility: covers the
    # ordinary (non-adversarial) part of the input space the targeted block
    # above deliberately skips, including odd window/min_on/min_off/dt
    # combinations no one thought to hand-pick.
    for _ in range(40):
        # window_ms/min_on_ms/min_off_ms/dt_ms all cross the harness as C
        # ``uint32_t`` (see heater_output_pwm_drift_harness.c's
        # ``(uint32_t)window_ms`` etc.) -- keep them whole-millisecond
        # integers here too, or the harness silently truncates a fractional
        # value the Python side never truncates, which is a mismatch in
        # what the two sides were even asked to compute, not a mirror bug.
        w = float(rng.randint(5000, 120000))
        mn = float(rng.randint(0, int(w * 0.4)))
        mf = float(rng.randint(0, int(w * 0.4)))
        d = rng.uniform(0.0, 1.0)
        dtm = rng.choice((100.0, 250.0, 500.0, 1000.0))
        n_ticks = max(1, int(3 * w / dtm))
        vecs.append((w, mn, mf, d, dtm, n_ticks))

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

    print(f"HEATER_OUTPUT PWM DRIFT CHECK: {len(vectors)} sampled vectors / {n_ticks_checked} "
          "ticks agreed exactly (relay_on and cycle_count, every tick) -- including a seeded "
          "adversarial + randomized batch, not just hand-picked round duties. Not a proof over "
          "the whole duty range; see module docstring.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
