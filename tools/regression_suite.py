#!/usr/bin/env python3
"""One-command regression suite: every automated gate this repo has, one
pass/fail summary, one exit code.

Before this script the gates were scattered and each invoked differently --
tools/run_all_checks.ps1, firmware/KilnFW/App/test/build_host_tests.ps1 (which
must run outside the interactive PowerShell tool -- see that script's own
header), firmware/SaftyFW's copy of the same, tools/PcTools' pytest suite, the
JS/browser harnesses (folded into run_all_checks.ps1 as check_js_host_tests.ps1
and check_ui_responsive_sweep.ps1 -- see those files, not reinvented here),
and the two target firmware builds. Nobody ran all of them before every
commit because there was no single command that did.

Usage:
    python tools/regression_suite.py

Exit code is 0 only if every gate passed. Non-zero (1) if any gate failed --
usable directly in CI or a pre-commit hook.

WHAT THIS NEVER DOES: flash a board, start/stop a profile or autotune, or
otherwise touch hardware. Every gate here is a static check, an off-target
host-compiled test, or a target *compile* (no flash). tools/PcTools' pytest
suite includes live-bench tests that need KILNCTRL_BENCH_HOST set; this
script deliberately does not set it, so they self-skip (see
tools/PcTools/README.md "Live-bench test harness") rather than needing to be
excluded by name here -- one exclusion mechanism, not two.

STAGING / CONCURRENCY (see run() below for the actual groups):
  Stage 1 -- tools/run_all_checks.ps1 alone. Cheapest gate (mechanical
    check_*.ps1 scripts plus, via its own discovery, the JS test harnesses
    and the headless-Chrome responsive sweep) and the one most likely to
    catch an obvious regression fast, so it runs first and its own internal
    25-ish checks do not contend with anything else for CPU or a port.
  Stage 2 -- the three off-target test gates (KilnFW host tests, SaftyFW
    host tests, PcTools pytest) run IN PARALLEL. Each owns a build/cache
    directory the others never touch (App/test/build, SaftyFW/test/build,
    PcTools/tests/.pytest_cache), so there is nothing to corrupt by
    overlapping them, and each is single-toolchain (MSVC / MSVC / Python) so
    they do not fight over the same compiler instance either.
  Stage 3 -- the two target *builds* (KilnFW over ESP-IDF/ninja, SaftyFW over
    pico-sdk/ninja) run IN PARALLEL. Different build directories
    (firmware/KilnFW/build vs firmware/SaftyFW/build), different toolchains
    (Xtensa vs ARM), so nothing to serialize there either -- they are put in
    their own stage rather than merged into Stage 2 only because they are
    the slowest gates by far (minutes, not seconds) and a fast Stage 1/2
    failure is worth seeing before committing CPU to a multi-minute compile.

Nothing here calls a kiln_call MCP tool. build_kilnfw's underlying command
(source the Espressif PowerShell profile, then idf.py/ninja) and
build_saftyfw's (cmake --build) are reproduced directly from
tools/PcTools/src/mcpkit/workbench.py so this script has no dependency on the
kilnctrl MCP server being up -- it is meant to double as a CI entry point.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import Callable, Optional, Sequence


def repo_root() -> str:
    here = os.path.abspath(os.path.dirname(__file__))
    while True:
        if (os.path.isdir(os.path.join(here, "firmware"))
                and os.path.isdir(os.path.join(here, "tools", "PcTools"))):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise RuntimeError("could not locate the kilnCtl repo root from " + __file__)
        here = parent


ROOT = repo_root()

# Same leak-guard as mcpkit/workbench.py: Git Bash's MSYSTEM et al, if this
# script is itself launched from a Git Bash shell, make cmake refuse to
# configure ("MSys/Mingw is no longer supported").
_MSYS_ENV_VARS = ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYS2_PATH_TYPE")

_IDF_PROFILE = r"C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

_INTERESTING = re.compile(
    r"\b(error|fatal|failed|FAIL|assert|undefined reference|warning C\d|"
    r"PASS|passed|\d+ of \d+|Verified OK)\b",
    re.IGNORECASE,
)
_MAX_SUMMARY_LINES = 40

#: Same signatures mcpkit/workbench.py checks for its own build tools: this
#: repo is worked by more than one concurrent agent/session, and two builds
#: writing the same build directory at once (ninja's manifest lock, or a
#: half-written generated header) produces an error that reads exactly like a
#: source defect but isn't one. build_kilnfw and build_saftyfw here use the
#: SAME build directories those MCP tools use, so they can hit the same
#: contention -- flagging it here keeps a transient collision with another
#: session from being reported (or acted on) as a code regression.
_CONTENTION_SIGNATURES = (
    (re.compile(r"failed recompaction.*Permission denied", re.IGNORECASE),
     "ninja's build.ninja manifest lock lost a race -- another build was "
     "writing this same build directory at the same time"),
    (re.compile(r"build_info\.h.*unknown type name", re.IGNORECASE | re.DOTALL),
     "build_info.h failed to parse -- signature of a partial/interleaved "
     "write from a concurrent build in the same build directory"),
    (re.compile(r"unknown type name ['\"]by['\"]"),
     "a generated header read back truncated/spliced -- signature of two "
     "builds writing the same generated file at once"),
)


def _contention_note(output: str) -> Optional[str]:
    for pattern, explanation in _CONTENTION_SIGNATURES:
        if pattern.search(output):
            return (f"LIKELY BUILD CONTENTION, not a source defect: {explanation}. "
                     "If another build/agent touched this same build directory during "
                     "this run, re-run this gate alone (--stage) before trusting it as "
                     "a real failure.")
    return None


@dataclass
class GateResult:
    name: str
    stage: int
    status: str  # "PASS" | "FAIL" | "SKIP"
    seconds: float
    detail: str
    log_path: str = ""


@dataclass
class Gate:
    name: str
    stage: int
    build: Callable[[], "tuple[Sequence[str], Optional[str]]"]
    timeout: int = 900
    # If this gate's own toolchain/config is simply absent from this
    # machine, prereq() returns a reason string and the gate is SKIPPED
    # (not FAILED) -- an environment fact, not a code regression.
    prereq: Optional[Callable[[], Optional[str]]] = None


def _powershell(script: str, extra: "Sequence[str]" = ()) -> "list[str]":
    return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, *extra]


def _log_path(tag: str) -> str:
    directory = os.path.join(tempfile.gettempdir(), "kilnctl-regression-suite")
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, f"{tag}-{int(time.time())}.log")


def _run_gate(gate: Gate) -> GateResult:
    if gate.prereq is not None:
        reason = gate.prereq()
        if reason:
            return GateResult(gate.name, gate.stage, "SKIP", 0.0, reason)

    argv, cwd = gate.build()
    started = time.monotonic()
    env = {k: v for k, v in os.environ.items() if k not in _MSYS_ENV_VARS}
    try:
        completed = subprocess.run(
            argv, cwd=cwd or ROOT, capture_output=True, text=True, errors="replace",
            timeout=gate.timeout, env=env, shell=False,
        )
        rc = completed.returncode
        output = (completed.stdout or "") + (completed.stderr or "")
    except FileNotFoundError as exc:
        return GateResult(gate.name, gate.stage, "FAIL", time.monotonic() - started,
                           f"could not launch {argv[0]!r}: {exc}")
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        elapsed = time.monotonic() - started
        path = _log_path(gate.name)
        with open(path, "w", encoding="utf-8", errors="replace") as fh:
            fh.write(f"$ {' '.join(argv)}\n\n{output}")
        return GateResult(gate.name, gate.stage, "FAIL", elapsed,
                           f"TIMEOUT after {gate.timeout}s", path)

    elapsed = time.monotonic() - started
    path = _log_path(gate.name)
    with open(path, "w", encoding="utf-8", errors="replace") as fh:
        fh.write(f"$ {' '.join(argv)}\n\n{output}")

    lines = [ln.rstrip() for ln in output.splitlines() if ln.strip()]
    interesting = [ln for ln in lines if _INTERESTING.search(ln)]
    tail = lines[-_MAX_SUMMARY_LINES // 2:]
    keep = [ln for ln in interesting if ln not in tail][-(_MAX_SUMMARY_LINES // 2):]
    shown = keep + tail
    detail = "\n".join(shown) if shown else "(no output)"

    status = "PASS" if rc == 0 else "FAIL"
    if rc != 0:
        detail = f"exit {rc}\n{detail}"
        note = _contention_note(output)
        if note:
            detail = f"{detail}\n--\n{note}"
    return GateResult(gate.name, gate.stage, status, elapsed, detail, path)


# ---------------------------------------------------------------------------
# gate definitions
# ---------------------------------------------------------------------------

def _gate_repo_checks() -> Gate:
    script = os.path.join(ROOT, "tools", "run_all_checks.ps1")
    return Gate("repo_checks (run_all_checks.ps1: mechanical + JS + UI-sweep)", 1,
                lambda: (_powershell(script), ROOT), timeout=900)


def _gate_kilnfw_host_tests() -> Gate:
    script = os.path.join(ROOT, "firmware", "KilnFW", "App", "test", "build_host_tests.ps1")
    return Gate("kilnfw_host_tests", 2, lambda: (_powershell(script), ROOT), timeout=600)


def _gate_saftyfw_host_tests() -> Gate:
    script = os.path.join(ROOT, "firmware", "SaftyFW", "test", "build_host_tests.ps1")
    return Gate("saftyfw_host_tests", 2, lambda: (_powershell(script), ROOT), timeout=600)


def _pctools_python() -> str:
    """The PcTools venv's interpreter, not whatever `python` resolves to on
    PATH -- pytest and this repo's own test dependencies live there, not in
    a bare system Python (see tools/PcTools/README.md's setup section)."""
    venv_python = os.path.join(ROOT, "tools", "PcTools", ".venv", "Scripts", "python.exe")
    return venv_python if os.path.isfile(venv_python) else sys.executable


def _pytest_prereq() -> Optional[str]:
    python = _pctools_python()
    check = subprocess.run([python, "-c", "import pytest"], capture_output=True)
    if check.returncode != 0:
        return (f"pytest not importable under {python} -- run "
                f"`pip install -r tools/PcTools/requirements*.txt` in that venv first")
    return None


def _gate_pctools_pytest() -> Gate:
    tests_dir = os.path.join(ROOT, "tools", "PcTools", "tests")
    return Gate(
        "pctools_pytest (~1089 tests; live-bench tests self-skip, no KILNCTRL_BENCH_HOST set)",
        2, lambda: ([_pctools_python(), "-m", "pytest", tests_dir, "-q"], ROOT), timeout=600,
        prereq=_pytest_prereq)


def _idf_prereq() -> Optional[str]:
    if not os.path.isfile(_IDF_PROFILE):
        return f"ESP-IDF profile not found at {_IDF_PROFILE} -- update _IDF_PROFILE if Espressif moved"
    return None


def _gate_build_kilnfw() -> Gate:
    def build():
        build_dir = os.path.join(ROOT, "firmware", "KilnFW", "build")
        inner = f"idf.py -C '{os.path.join(ROOT, 'firmware', 'KilnFW')}' build"
        command = f"& '{_IDF_PROFILE}' *>&1 | Out-Null; {inner}; exit $LASTEXITCODE"
        return (["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
                build_dir)
    return Gate("build_kilnfw (ESP-IDF compile, no flash)", 3, build, timeout=1800, prereq=_idf_prereq)


def _saftyfw_build_prereq() -> Optional[str]:
    build_dir = os.path.join(ROOT, "firmware", "SaftyFW", "build")
    if not os.path.isdir(build_dir):
        return (f"{build_dir} not configured -- run "
                f"`cmake -S firmware/SaftyFW -B {build_dir} -G Ninja` once first")
    return None


def _gate_build_saftyfw() -> Gate:
    def build():
        build_dir = os.path.join(ROOT, "firmware", "SaftyFW", "build")
        return (["cmake", "--build", build_dir], build_dir)
    return Gate("build_saftyfw (pico-sdk compile, no flash)", 3, build, timeout=900,
                prereq=_saftyfw_build_prereq)


GATES = [
    _gate_repo_checks(),
    _gate_kilnfw_host_tests(),
    _gate_saftyfw_host_tests(),
    _gate_pctools_pytest(),
    _gate_build_kilnfw(),
    _gate_build_saftyfw(),
]


def run(only_stage: Optional[int] = None) -> "list[GateResult]":
    results: "list[GateResult]" = []
    stages = sorted(set(g.stage for g in GATES))
    for stage in stages:
        if only_stage is not None and stage != only_stage:
            continue
        stage_gates = [g for g in GATES if g.stage == stage]
        print(f"\n=== stage {stage}: {', '.join(g.name for g in stage_gates)} ===")
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(stage_gates)) as pool:
            futures = {pool.submit(_run_gate, g): g for g in stage_gates}
            for fut in concurrent.futures.as_completed(futures):
                r = fut.result()
                marker = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "SKIP"}[r.status]
                print(f"  {marker:4s}  {r.name}  ({r.seconds:.1f}s)")
                results.append(r)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=int, default=None,
                         help="run only this stage (1, 2 or 3) -- for debugging one group")
    args = parser.parse_args()

    started = time.monotonic()
    results = run(only_stage=args.stage)
    elapsed = time.monotonic() - started

    results.sort(key=lambda r: (r.stage, r.name))
    passed = [r for r in results if r.status == "PASS"]
    failed = [r for r in results if r.status == "FAIL"]
    skipped = [r for r in results if r.status == "SKIP"]

    print("\n" + "=" * 78)
    print(f"REGRESSION SUITE SUMMARY -- {len(results)} gates, {elapsed:.1f}s wall time")
    print("=" * 78)
    for r in results:
        print(f"  [{r.status:4s}] stage {r.stage}  {r.name}  ({r.seconds:.1f}s)")
    print()
    print(f"  {len(passed)} passed, {len(failed)} FAILED, {len(skipped)} skipped")

    if failed:
        print("\nFAILURES:")
        for r in failed:
            print(f"\n--- {r.name} ---")
            print(r.detail)
            if r.log_path:
                print(f"(full log: {r.log_path})")

    if skipped:
        print("\nSKIPPED (environment/prereq, not a code failure):")
        for r in skipped:
            print(f"  - {r.name}: {r.detail}")

    print()
    if failed:
        print(f"RESULT: FAIL ({len(failed)} of {len(results)} gates failed)")
        return 1
    print(f"RESULT: PASS (all {len(results)} gates green" +
          (f", {len(skipped)} skipped" if skipped else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
