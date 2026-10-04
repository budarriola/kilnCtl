"""Build and host-test runners, exposed through the MCP servers' facade.

Everything here is a build step this repo's agents already run by hand every
session, encoded once so the invocation stops being rediscovered:

* ``build_kilnfw`` sources the Espressif PowerShell profile first. Pointing at
  the ESP-IDF python alone is not enough -- it has neither cmake nor ninja nor
  the toolchain on ``PATH`` -- and a missing ``ESP_IDF_VERSION`` fails deep
  inside ``idf_component_manager`` with a ``NoneType`` regex error rather than
  anything that names the real problem.
* The two ``*_host_tests`` runners drive their ``build_host_tests.ps1``
  through ``subprocess``. Those scripts set ``$ErrorActionPreference = "Stop"``
  and ``vcvarsall.bat`` writes a benign ``vswhere.exe`` line to stderr; a host
  PowerShell that wraps native stderr in ErrorRecords turns that into a
  terminating error before a single test runs, which reads exactly like a
  regression that is not there. A plain subprocess does no such wrapping.
* ``build_kilnfw`` takes a ``jobs`` argument that calls ninja directly, because
  ``idf.py build -- -j N`` is rejected by idf.py's own argument parser.
* Every heavy build here also goes through :func:`mcpkit.buildgate.kiln_build_gate`,
  the machine-wide admission gate (at most ``KILNCTL_BUILD_GATE_SLOTS`` heavy
  builds across ALL sessions, default 2) -- a separate concern from
  :mod:`mcpkit.buildlock`'s per-resource-key lock above, which only prevents
  two builds from corrupting the SAME build directory. This machine has hard
  frozen under uncoordinated concurrent full target builds; see
  ``tools/build_gate.ps1``'s header for the incident this exists for.

Output is summarized, not echoed. A full firmware build is thousands of lines;
what a caller needs is the exit status, the failing lines, and a path to the
rest. :func:`_summarize` keeps the tail plus every line that looks like a
diagnostic, and writes the complete log to the scratch directory.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Optional, Sequence

from mcpkit.buildgate import GateWaitResult, kiln_build_gate
from mcpkit.buildlock import BuildLockTimeout, build_lock
from mcpkit.pytest_verdict import PER_TEST_TIMEOUT_S, pytest_output_problems

#: Lines worth surfacing even when they are not near the end of the log.
_INTERESTING = re.compile(
    r"\b(error|fatal|failed|FAIL|assert|undefined reference|warning C\d|"
    r"PASS|passed|\d+ of \d+|Verified OK)\b",
    re.IGNORECASE,
)

#: Ceiling on how much of a build log comes back in one tool result. Past this
#: the caller wants the log file, not more scrollback.
_MAX_LINES = 40


def repo_root() -> str:
    """The kilnCtl checkout root, found by landmark rather than by counting.

    ``__file__``-relative arithmetic breaks the moment this package moves; the
    landmark pair (``firmware/`` beside ``tools/PcTools``) does not.
    """
    here = os.path.abspath(os.path.dirname(__file__))
    while True:
        if (os.path.isdir(os.path.join(here, "firmware"))
                and os.path.isdir(os.path.join(here, "tools", "PcTools"))):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise RuntimeError("could not locate the kilnCtl repo root from " + __file__)
        here = parent


def _log_path(tag: str) -> str:
    directory = os.path.join(tempfile.gettempdir(), "kilnctl-builds")
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, f"{tag}-{int(time.time())}.log")


def _summarize(tag: str, argv: "Sequence[str]", rc: Optional[int], output: str,
               elapsed: float,
               output_check: "Optional[Callable[[str], list[str]]]" = None) -> str:
    """Exit status, the lines that explain it, and where the rest lives."""
    path = _log_path(tag)
    try:
        with open(path, "w", encoding="utf-8", errors="replace") as handle:
            handle.write(f"$ {' '.join(argv)}\n\n{output}")
        where = path
    except OSError as exc:  # pragma: no cover - scratch dir is writable in practice
        where = f"(log not written: {exc})"

    lines = [line.rstrip() for line in output.splitlines() if line.strip()]
    interesting = [line for line in lines if _INTERESTING.search(line)]
    # Tail first, then any diagnostic that scrolled past it, in original order.
    tail = lines[-_MAX_LINES // 2:]
    keep = [line for line in interesting if line not in tail][-(_MAX_LINES // 2):]
    shown = keep + tail

    status = "OK" if rc == 0 else ("TIMEOUT" if rc is None else f"FAILED (exit {rc})")
    problems = output_check(output) if (output_check is not None and rc == 0) else []
    if problems:
        # A clean exit code is not trusted: e.g. a lost xdist worker still
        # exits 0 with "0 failed" (see mcpkit/pytest_verdict.py).
        status = f"FAILED (exit {rc}, but output check failed)"
        shown = [f"OUTPUT CHECK FAILED: {p}" for p in problems] + shown
    head = f"{tag}: {status} in {elapsed:.1f}s ({len(lines)} log lines)"
    body = "\n".join(shown) if shown else "(no output)"
    result = f"{head}\nfull log: {where}\n--\n{body}"
    if rc != 0:
        note = _contention_note(output)
        if note:
            result = f"{result}\n--\n{note}"
    return result


#: Vars Git Bash sets that leak into this MCP server's environment when it is
#: launched from a Git Bash session. cmake sees MSYSTEM and refuses to
#: configure ("MSys/Mingw is no longer supported"), aborting build_kilnfw in
#: ~3s with no other output -- a false pass, since _summarize still reports
#: exit 0 for the wrapping idf.py invocation that never reached the compiler.
_MSYS_ENV_VARS = ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYS2_PATH_TYPE")


#: Known signatures of build-directory *contention*, not a source defect --
#: surfaced today as: (1) ninja's own manifest lock losing a race between two
#: concurrent invocations sharing a build dir, and (2) an interleaved partial
#: write to a generated header (gen_build_info.cmake) reading back as a
#: nonsense C declaration. Checked against every build/test tool's output so
#: a caller who hits either one is told "this is contention", not left to
#: chase a phantom source bug the way the first agent who hit this did.
_CONTENTION_SIGNATURES = (
    (re.compile(r"failed recompaction.*Permission denied", re.IGNORECASE),
     "ninja's build.ninja manifest lock lost a race -- another build was "
     "writing this same build directory at the same time"),
    (re.compile(r"build_info\.h.*unknown type name", re.IGNORECASE | re.DOTALL),
     "build_info.h failed to parse -- classic signature of a partial/"
     "interleaved write from a concurrent build in the same build directory"),
    (re.compile(r"unknown type name ['\"]by['\"]"),
     "a generated header read back with a truncated/spliced declaration -- "
     "classic signature of two builds writing the same generated file at once"),
)


def _contention_note(output: str) -> "Optional[str]":
    for pattern, explanation in _CONTENTION_SIGNATURES:
        if pattern.search(output):
            return (
                "NOTE: this failure matches a known BUILD-CONTENTION signature, "
                f"not a source defect: {explanation}. If two build/test tools ran "
                "at the same time, that -- not the code -- is almost certainly the "
                "cause. This should no longer happen through the MCP build tools "
                "themselves (they now serialize on the shared build directory), "
                "but a build invoked outside them (a bare `idf.py build` in a "
                "second terminal, for instance) is not covered by that lock."
            )
    return None


def _run(tag: str, argv: "Sequence[str]", *, cwd: Optional[str] = None,
         timeout: int = 900, env: "Optional[dict[str, str]]" = None,
         output_check: "Optional[Callable[[str], list[str]]]" = None) -> str:
    argv = list(argv)
    started = time.monotonic()
    if env is None:
        env = {k: v for k, v in os.environ.items() if k not in _MSYS_ENV_VARS}
    try:
        completed = subprocess.run(
            argv,
            cwd=cwd or repo_root(),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            env=env,
            # No shell: these argument lists contain absolute Windows paths with
            # spaces, and a shell would be one more quoting layer to get wrong.
            shell=False,
        )
    except FileNotFoundError:
        return f"{tag}: error: {argv[0]} not found on PATH"
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(partial, bytes):  # pragma: no cover - text=True keeps it str
            partial = partial.decode("utf-8", "replace")
        return _summarize(tag, argv, None, partial, time.monotonic() - started)
    output = (completed.stdout or "") + (completed.stderr or "")
    return _summarize(tag, argv, completed.returncode, output, time.monotonic() - started,
                      output_check=output_check)


def _powershell(script: str, extra: "Sequence[str]" = ()) -> "list[str]":
    return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, *extra]


def _run_locked(tag: str, resource_key: str, argv: "Sequence[str]", *,
                 wait_timeout: "Optional[float]" = None, **kwargs: Any) -> str:
    """``_run``, but serialized against any other caller contending for the
    same ``resource_key`` (typically a build directory).

    This is what turns two concurrent ``build_kilnfw`` calls from "corrupt
    each other's build directory" into "the second one queues and reports
    that it waited." The lock is scoped to ``resource_key``, not global --
    a SaftyFW build and a KilnFW build never wait on each other.

    ``wait_timeout`` should exceed the wrapped run's own ``timeout`` (passed
    through ``kwargs``) by a healthy margin -- otherwise a waiter can give up
    on a lock held by a holder that is still legitimately building, not
    stuck. Defaults to 90s over whatever ``kwargs['timeout']`` is (or 900s if
    that is not set), which is generous next to how long a single build step
    typically takes to notice it should give up.
    """
    if wait_timeout is None:
        wait_timeout = float(kwargs.get("timeout", 900)) + 90.0
    started = time.monotonic()
    try:
        with build_lock(resource_key, wait_timeout=wait_timeout):
            return _run(tag, argv, **kwargs)
    except BuildLockTimeout as exc:
        waited = time.monotonic() - started
        return (
            f"{tag}: FAILED (lock contention) -- another build is in progress on "
            f"{resource_key!r}, waited {waited:.1f}s for it to finish and gave up. "
            f"{exc}"
        )


# ---------------------------------------------------------------------------
# the tools themselves
# ---------------------------------------------------------------------------
def build_saftyfw_host_tests() -> str:
    """Build and run SaftyFW's off-target host unit tests (MSVC, no hardware).

    ``firmware/SaftyFW/test/build_host_tests.ps1``. If this reports a failure
    without naming a single failed check, suspect the toolchain wrapper rather
    than the code -- ``vcvarsall.bat`` writes a benign line to stderr that some
    PowerShell hosts promote to a terminating error.
    """
    root = repo_root()
    out_dir = os.path.join(root, "firmware", "SaftyFW", "test", "build")
    return _run_locked(
        "saftyfw-host-tests", out_dir,
        _powershell(os.path.join(root, "firmware", "SaftyFW", "test", "build_host_tests.ps1")))


def build_saftyfw(jobs: int = 0, saftyfw_root: Optional[str] = None) -> str:
    """Build the SaftyFW safety-processor firmware (``firmware/SaftyFW/build``).

    Produces the ELF that ``debug_program(peer="pico")`` flashes over SWD.

    ``saftyfw_root`` is an optional absolute path to a ``firmware/SaftyFW``-
    shaped directory (mirroring ``flash_firmware``'s ``kiln_fw_root``), for
    building from a clean git worktree instead of the main tree. Defaults to
    this repo's own ``firmware/SaftyFW``.

    If that root has no configured ``build/CMakeCache.txt`` yet, this
    configures it first (``cmake -S <root> -B <root>/build -G Ninja``),
    resolving ``PICO_SDK_PATH`` via :func:`mcpkit.pico_sdk.resolve_pico_sdk_path`
    and injecting it into only that subprocess's environment -- never this
    process's, and never persisted.
    """
    if saftyfw_root is not None:
        if not os.path.isabs(saftyfw_root):
            return f"saftyfw: error: saftyfw_root must be an absolute path, got {saftyfw_root!r}"
        if not os.path.isfile(os.path.join(saftyfw_root, "CMakeLists.txt")):
            return (f"saftyfw: error: saftyfw_root {saftyfw_root!r} has no CMakeLists.txt "
                     f"-- it does not look like a firmware/SaftyFW-shaped directory")
    root = saftyfw_root or os.path.join(repo_root(), "firmware", "SaftyFW")
    build_dir = os.path.join(root, "build")
    return _cmake_build("saftyfw", build_dir, jobs, source_dir=root)


def _cmake_build(tag: str, build_dir: str, jobs: int, source_dir: str) -> str:
    configure_note = ""
    if not os.path.isdir(build_dir) or not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")):
        src = source_dir
        from mcpkit.pico_sdk import PicoSdkNotFoundError, resolve_pico_sdk_path
        try:
            sdk_path = resolve_pico_sdk_path()
        except PicoSdkNotFoundError as exc:
            return f"{tag}: error: {exc}"
        configure_env = {k: v for k, v in os.environ.items() if k not in _MSYS_ENV_VARS}
        configure_env["PICO_SDK_PATH"] = sdk_path
        configure_argv = ["cmake", "-S", src, "-B", build_dir, "-G", "Ninja"]
        result = _run_locked(f"{tag}-configure", build_dir, configure_argv, cwd=src, env=configure_env)
        first_line = result.splitlines()[0] if result else ""
        if not first_line.startswith(f"{tag}-configure: OK"):
            return result
        configure_note = f" (configured from scratch, PICO_SDK_PATH={sdk_path})\n"
    argv = ["cmake", "--build", build_dir]
    if jobs > 0:
        argv += ["--parallel", str(jobs)]
    wait_result = GateWaitResult()
    try:
        with kiln_build_gate(tag, wait_result=wait_result):
            report = _run_locked(tag, build_dir, argv, cwd=build_dir)
    except TimeoutError as exc:
        # Same "...: FAILED (lock contention)" string shape _run_locked's own
        # BuildLockTimeout handler returns, so a caller parsing this tool's
        # output does not need a second failure shape for the gate timing out
        # instead of the per-directory lock (opus review A3).
        return f"{tag}: FAILED (lock contention) -- {exc}"
    if wait_result.waited_seconds > 0:
        report = f"{report}\n(gate waited {wait_result.waited_seconds:.1f}s for a heavy-build slot)"
    return configure_note + report


#: Puts idf.py, cmake, ninja and the Xtensa toolchain on PATH in one step.
#: Everything else is a partial environment that fails later and less clearly.
_IDF_PROFILE = r"C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"


def build_kilnfw(target: str = "build", jobs: int = 0, skip_saftyfw: bool = False,
                  kiln_fw_root: Optional[str] = None) -> str:
    """Build the ESP32-S3 main firmware (``firmware/KilnFW``) via ESP-IDF.

    ``target`` is an idf.py target -- ``build``, ``fullclean``, ``reconfigure``.
    ``jobs`` above zero bypasses idf.py and calls ninja directly, because
    idf.py's argument parser rejects ``-- -j N``; it therefore only applies to
    an already-configured build directory and is ignored for other targets.

    ``kiln_fw_root`` is an optional absolute path to a ``firmware/KilnFW``-
    shaped directory (mirroring ``flash_firmware``'s own ``kiln_fw_root`` and
    ``build_saftyfw``'s ``saftyfw_root``), for building the sanctioned "clean
    git worktree at HEAD" flow through this gated tool instead of a bare
    ``idf.py -C <worktree>\\firmware\\KilnFW build`` invocation outside any
    lock/gate (opus review of 7f6d3db5, finding 1 -- COMMON.md forbade the
    direct call while MCP_SERVERS.md told agents to make it anyway, and this
    tool had no parameter to gate it end to end). When given, the companion
    SaftyFW build (see below) also runs against that worktree's own
    ``firmware/SaftyFW``, not the main tree's, so the embedded slot images
    come from the same worktree being built. On a from-scratch or
    ``fullclean``'d root this also runs ``git submodule update --init
    components/lvgl`` and ``idf.py set-target esp32s3`` first (the two traps
    documented in docs/MCP_SERVERS.md's "Building from a clean worktree for
    `kiln_fw_root`" section) -- neither is a full build, so neither goes
    through the build gate.

    2026-09-20 (docs/PICO_AUTO_UPDATE_PLAN.md): the KilnFW APPLICATION build
    now ``EMBED_FILES`` two SaftyFW slot images
    (``firmware/SaftyFW/build/SaftyFW_slotA.bin``/``slotB.bin``) so the ESP
    can update the Pico automatically at boot, and fails at CMake configure
    time if either is missing or stale relative to the SaftyFW source tree.
    For a ``build``/``reconfigure`` target this tool therefore now builds
    SaftyFW FIRST (``build_saftyfw()``, which produces those slot bins as a
    side effect of its own target build) and reports BOTH build outputs,
    same ordering ``tools/run_all_checks.ps1`` phase 1a/1b now enforces for
    the two ``check_00_*_target_build.ps1`` checks. A SaftyFW build failure
    aborts before the KilnFW build even starts, since a KilnFW build against
    a missing/stale pair would either fail its own configure step or --
    worse -- silently embed whatever slot bins happen to already be on disk.
    ``skip_saftyfw=True`` opts out (e.g. ``fullclean``, or a caller that just
    built SaftyFW itself and wants to avoid a redundant rebuild) -- the
    caller is then responsible for the slot bins being current;
    ``check_embedded_pico_image_fresh.ps1`` is the check that catches it if
    they are not.

    Flashing is deliberately not offered here: this board is programmed over
    JTAG with OpenOCD (``flash_firmware`` / ``debug_program``), never esptool.
    Hand a successful ``kiln_fw_root`` build's directory straight to
    ``flash_firmware(kiln_fw_root=...)``.

    The build gate slot is released and re-acquired BETWEEN the SaftyFW build
    and the KilnFW build below (each goes through its own ``with
    kiln_build_gate(...)`` block, not one shared one) -- a queued waiter can
    take the slot in that gap and run its own build before this call's KilnFW
    half starts. That is intentional, not a bug: holding one slot across both
    builds here would make a 2-slot machine behave like a 1-slot machine for
    every KilnFW build, and the two builds are already ordered by the
    SaftyFW-first data dependency above, not by holding the gate continuously
    (opus review of 7f6d3db5, finding 2).
    """
    root = repo_root()
    if kiln_fw_root is not None:
        if not os.path.isabs(kiln_fw_root):
            return f"kilnfw: error: kiln_fw_root must be an absolute path, got {kiln_fw_root!r}"
        if not os.path.isfile(os.path.join(kiln_fw_root, "CMakeLists.txt")):
            return (f"kilnfw: error: kiln_fw_root {kiln_fw_root!r} has no CMakeLists.txt "
                     f"-- it does not look like a firmware/KilnFW-shaped directory")
    if kiln_fw_root is not None and "'" in kiln_fw_root:
        return (f"kilnfw: error: kiln_fw_root {kiln_fw_root!r} contains a single quote -- "
                f"this path is interpolated into a single-quoted PowerShell -Command string "
                f"and a quote in it would break that quoting")
    kiln_fw_dir = os.path.normpath(kiln_fw_root) if kiln_fw_root is not None else os.path.join(root, "firmware", "KilnFW")
    build_dir = os.path.join(kiln_fw_dir, "build")
    saftyfw_root_arg: Optional[str] = None
    if kiln_fw_root is not None:
        # <worktree>/firmware/KilnFW -> <worktree>/firmware/SaftyFW, so the
        # embedded slot images come from the SAME worktree being built, not
        # the main tree's firmware/SaftyFW. kiln_fw_dir is normpath'd above so
        # a trailing backslash in kiln_fw_root (os.path.dirname("...\\KilnFW\\")
        # otherwise returns "...\\KilnFW" itself, deriving a bogus
        # ...\\KilnFW\\SaftyFW sibling) can't throw this off.
        worktree_firmware_dir = os.path.dirname(kiln_fw_dir)
        saftyfw_root_arg = os.path.join(worktree_firmware_dir, "SaftyFW")
        if not os.path.isfile(os.path.join(saftyfw_root_arg, "CMakeLists.txt")):
            return (f"kilnfw: error: derived saftyfw_root {saftyfw_root_arg!r} (sibling of "
                     f"kiln_fw_root) has no CMakeLists.txt -- kiln_fw_root does not look like "
                     f"it is under a normal firmware/KilnFW + firmware/SaftyFW worktree layout")
    saftyfw_report = None
    if target in ("build", "reconfigure") and not skip_saftyfw:
        saftyfw_report = build_saftyfw(jobs, saftyfw_root=saftyfw_root_arg)
        saftyfw_ok = "saftyfw: OK" in saftyfw_report
        if not saftyfw_ok:
            return (
                f"kilnfw-{target}: ABORTED -- SaftyFW build (which produces the "
                f"slot images this KilnFW build embeds) did not succeed:\n\n"
                f"{saftyfw_report}"
            )
    if not os.path.isfile(_IDF_PROFILE):
        result = (f"kilnfw: error: ESP-IDF profile not found at {_IDF_PROFILE} -- "
                  f"update _IDF_PROFILE in mcpkit/workbench.py if Espressif moved")
        return f"{saftyfw_report}\n\n{result}" if saftyfw_report else result
    setup_note = ""
    if kiln_fw_root is not None:
        # Fresh or fullclean'd worktree: neither of these is a full build, so
        # neither goes through the build gate (docs/MCP_SERVERS.md's "clean
        # worktree" section documents both traps this closes). The lvgl
        # submodule check runs unconditionally -- it's cheap and independent
        # of sdkconfig state.
        lvgl_dir = os.path.join(kiln_fw_dir, "components", "lvgl")
        if not os.path.isfile(os.path.join(lvgl_dir, "CMakeLists.txt")):
            submodule_result = _run_locked(
                "kilnfw-submodule-init", kiln_fw_dir,
                ["git", "submodule", "update", "--init", "components/lvgl"], cwd=kiln_fw_dir)
            if "kilnfw-submodule-init: OK" not in submodule_result:
                return submodule_result
            setup_note += f"{submodule_result}\n"
        # A normal `idf.py build` never writes build/sdkconfig -- only
        # check_00_kilnfw_target_build.ps1's publish step does that. The
        # real "not yet configured" signal is the absence of the ROOT
        # sdkconfig (<kiln_fw_dir>/sdkconfig, gitignored, missing in a fresh
        # worktree) alongside a missing build/CMakeCache.txt -- testing
        # build/sdkconfig instead made set-target run on EVERY call for a
        # kiln_fw_root build, which clears the build dir and regenerates the
        # root sdkconfig from defaults, discarding the caller's config on
        # every single build (opus review of 171cc5bc, required fix). Never
        # run set-target for a fullclean target -- there is nothing to
        # configure yet and set-target after a fullclean just reconfigures
        # the same defaults again for no reason.
        needs_set_target = (
            target != "fullclean"
            and not os.path.isfile(os.path.join(kiln_fw_dir, "sdkconfig"))
            and not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")))
        if needs_set_target:
            set_target_result = _run_locked(
                "kilnfw-set-target", kiln_fw_dir,
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                 f"& '{_IDF_PROFILE}' *>&1 | Out-Null; idf.py -C '{kiln_fw_dir}' set-target esp32s3; "
                 f"exit $LASTEXITCODE"])
            if "kilnfw-set-target: OK" not in set_target_result:
                return f"{setup_note}{set_target_result}"
            setup_note += f"{set_target_result}\n"
    if jobs > 0 and target == "build":
        inner = f"cd '{build_dir}'; ninja -j {jobs}"
    else:
        inner = f"idf.py -C '{kiln_fw_dir}' {target}"
    # powershell.exe does NOT propagate a native command's exit code as its own
    # process exit code unless the script explicitly does so -- without the
    # trailing `exit $LASTEXITCODE`, this always returned 0 even when idf.py
    # or ninja failed, which is exactly the false-pass bug this wrapper exists
    # to avoid. See _MSYS_ENV_VARS above for the sibling false-pass this same
    # command is also guarding against.
    command = f"& '{_IDF_PROFILE}' *>&1 | Out-Null; {inner}; exit $LASTEXITCODE"
    elf_path = os.path.join(build_dir, "KilnCtrl.elf")
    elf_before = _stat_snapshot(elf_path)
    wait_result = GateWaitResult()
    try:
        with kiln_build_gate(f"kilnfw-{target}", wait_result=wait_result):
            kilnfw_report = _run_locked(
                f"kilnfw-{target}", build_dir,
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
                timeout=1800)
    except TimeoutError as exc:
        # Same string shape as _run_locked's own lock-contention failure --
        # opus review A3.
        result = f"kilnfw-{target}: FAILED (lock contention) -- {exc}"
        return f"{saftyfw_report}\n\n{result}" if saftyfw_report else result
    if wait_result.waited_seconds > 0:
        kilnfw_report = (
            f"{kilnfw_report}\n(gate waited {wait_result.waited_seconds:.1f}s for a heavy-build slot)")
    if setup_note:
        kilnfw_report = f"{setup_note}{kilnfw_report}"
    # The sdkconfig-provenance-sibling fix only makes sense for the main
    # tree's own firmware/KilnFW -- an isolated kiln_fw_root worktree has no
    # relationship to check_00_kilnfw_target_build.ps1's published sibling.
    if kiln_fw_root is None and target in ("build", "reconfigure") and f"kilnfw-{target}: OK" in kilnfw_report:
        elf_after = _stat_snapshot(elf_path)
        kilnfw_report = (
            f"{kilnfw_report}\n\n"
            f"{_refresh_build_sdkconfig(root, build_dir, elf_path, elf_before, elf_after)}")
    if saftyfw_report is not None:
        return f"{saftyfw_report}\n\n{kilnfw_report}"
    return kilnfw_report


def _stat_snapshot(path: str) -> Optional[tuple]:
    """(mtime_ns, size) for ``path``, or ``None`` if it does not exist yet.

    Used to tell whether a build actually relinked the ELF, since ninja does
    not reliably relink on an sdkconfig-only change alone (see
    ``check_all_task_stack_budgets.py``'s own comment on
    ``CMAKE_CONFIGURE_DEPENDS`` not repeating a build).
    """
    try:
        st = os.stat(path)
    except OSError:
        return None
    return (st.st_mtime_ns, st.st_size)


def _refresh_build_sdkconfig(
    root: str,
    build_dir: str,
    elf_path: str,
    elf_before: Optional[tuple],
    elf_after: Optional[tuple],
) -> str:
    """Keep ``build/sdkconfig`` from outliving the ELF it describes.

    ``check_00_kilnfw_target_build.ps1`` publishes an isolated checkbuild's
    own sdkconfig into an invoking tree's ``build/`` next to the ELF it also
    publishes there -- a config PUBLISHED alongside the artifact it produced,
    which ``check_all_task_stack_budgets.py``'s sdkconfig resolution treats
    as authoritative. A later plain ``idf.py build`` (i.e. THIS function)
    relinks ``build/KilnCtrl.elf`` from the tree's own
    ``firmware/KilnFW/sdkconfig`` but never touched that published sibling
    before this fix, so the two could silently disagree -- the sibling then
    describing an EARLIER build than the ELF sitting next to it (a "reset one
    side of a pair" bug; see CLAUDE.md). ``check_all_task_stack_budgets.py``
    now refuses to grade against a disagreeing sibling rather than silently
    trusting either one, but the fix belongs here too: after any build this
    tool performs, make the two agree again by overwriting the sibling with
    the tree's live config, so an ordinary ``build_kilnfw()`` call never
    leaves that trap behind for the checker to find later.

    2026-09-23 narrow window (opus review): ``check_00_kilnfw_target_build.ps1``
    publishes the checkbuild worktree's sdkconfig into this tree's ``build/``
    as the provenance record of the ELF it published. If someone edits the
    live ``firmware/KilnFW/sdkconfig`` afterward and calls ``build_kilnfw``
    with ``jobs`` set (the ``ninja -j N`` path) and ninja does NOT relink --
    which it is not guaranteed to do off an sdkconfig-only change -- a blind
    copy here would overwrite the correct published sibling with a config
    that never produced the ELF still sitting in ``build/``, silencing
    ``check_all_task_stack_budgets.py``'s guard instead of tripping it. So:
    only copy when either the live config already matches the sibling (a
    no-op either way) or the ELF actually changed (new mtime/size) during
    this build. When the live config differs and the ELF did NOT change, skip
    the copy and say so explicitly, naming both paths, rather than reporting
    a bland OK.
    """
    live = os.path.join(root, "firmware", "KilnFW", "sdkconfig")
    sibling = os.path.join(build_dir, "sdkconfig")
    if not os.path.isfile(live):
        return f"sdkconfig-refresh: SKIPPED -- no live config at {live}"
    sibling_exists = os.path.isfile(sibling)
    if sibling_exists:
        try:
            live_matches_sibling = _config_lines(live) == _config_lines(sibling)
        except OSError:
            # Sibling vanished or is locked between the isfile() check above
            # and the read (another build racing this one, most likely) --
            # treat that as "not matching" rather than letting the OSError
            # escape and abort the whole build_kilnfw() call.
            live_matches_sibling = False
    else:
        live_matches_sibling = False
    elf_changed = elf_before != elf_after
    if sibling_exists and not live_matches_sibling and not elf_changed:
        return (
            f"sdkconfig-refresh: SKIPPED -- {live} differs from {sibling} but "
            f"{elf_path} did not change during this build, so the sibling may "
            f"still be the correct provenance record for the ELF on disk; NOT "
            f"refreshed to avoid silencing check_all_task_stack_budgets.py's "
            f"sibling-agreement guard"
        )
    if live_matches_sibling:
        return f"sdkconfig-refresh: OK -- {sibling} already matches {live}"
    try:
        shutil.copyfile(live, sibling)
    except OSError as exc:
        return f"sdkconfig-refresh: FAILED -- could not copy {live} -> {sibling}: {exc}"
    verb = "created from" if not sibling_exists else "now matches"
    return f"sdkconfig-refresh: OK -- {sibling} {verb} {live}"


def _config_lines(path: str) -> dict:
    """{CONFIG_KEY: value} for every non-comment ``CONFIG_*=value`` line.

    Mirrors ``_config_lines`` in
    ``firmware/KilnFW/App/test/check_all_task_stack_budgets.py`` exactly --
    that script's ``_check_sibling_pair_agreement`` is the guard this refresh
    exists to keep from tripping, and it compares parsed CONFIG_ lines, not
    raw bytes, so a comment-only or line-ending-only difference between the
    live config and the published sibling must not be reported here as a
    genuine disagreement. Not imported directly: that module pulls in the
    ELF/objdump stack-analysis machinery, which is far heavier than this
    build helper needs. Keep this in sync by hand if the source changes.
    """
    parsed: dict = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            if "=" in line:
                k, _, v = line.strip().partition("=")
                if k.startswith("CONFIG_"):
                    parsed[k] = v
    return parsed


def run_pctools_tests(pattern: Optional[str] = None) -> str:
    """Run the PC-side pytest suite (``tools/PcTools/tests``).

    ``pattern`` is passed to pytest's ``-k`` to scope the run. This is the
    suite that covers the protocol codecs and these MCP servers themselves.

    Locked on the tests directory: two concurrent pytest runs both write
    ``.pytest_cache`` (lastfailed, nodeids) under it, and pytest's own
    handling of that race is best-effort, not atomic -- serializing here is
    cheap and removes a whole class of "why did the cache look wrong" noise.
    """
    root = repo_root()
    tests_dir = os.path.join(root, "tools", "PcTools", "tests")
    # No -q: the "collected N items" header is what pytest_output_problems
    # compares against the summary line. --timeout makes a stuck test fail loud
    # instead of losing an xdist node.
    argv = [sys.executable, "-m", "pytest", tests_dir,
            f"--timeout={PER_TEST_TIMEOUT_S}"]
    if pattern:
        argv += ["-k", pattern]
    return _run_locked("pctools-tests", tests_dir, argv, timeout=600,
                       output_check=pytest_output_problems)


def run_repo_checks(list_only: bool = False) -> str:
    """Run every standing guard script in the repository (``tools/run_all_checks.ps1``).

    These are the ``check_*.ps1`` scripts that enforce invariants no compiler
    can: the HTTP route-table cap recounted from source, one CRC implementation
    only, the safety baud rate matching on both sides, the two protocol versions
    staying independent, ``safety_core.c`` never including the link header.

    ROADMAP.md M10 carried "wire the guard scripts into something that runs
    them" as open for weeks: each script had been proven able to fail, which is
    the hard part, and then nothing ran them. This is that something. The
    aggregator discovers the scripts by globbing rather than from a list, and
    fails loudly if it finds implausibly few -- a runner that silently finds
    nothing reports a green that means the opposite of what it looks like.

    ``list_only`` prints what would run without running it.

    Deliberately NOT lock-serialized: every ``check_*.ps1`` here reads source
    files and git state and writes nothing shared -- there is no build
    directory or generated artifact for two concurrent runs to corrupt, so a
    lock here would only make unrelated callers queue for no reason.
    """
    root = repo_root()
    script = os.path.join(root, "tools", "run_all_checks.ps1")
    if not os.path.isfile(script):
        return f"repo-checks: error: {script} not found"
    argv = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script]
    if list_only:
        argv.append("-ListOnly")
    return _run("repo-checks", argv, timeout=900)


#: Tool name -> function, grouped by which server should carry it. Both servers
#: get the PC test suite; each gets the firmware it is the counterpart to, and
#: nothing gets registered twice on the same server.
BUNDLES: "dict[str, dict[str, Callable[..., str]]]" = {
    "dut": {
        "build_saftyfw_host_tests": build_saftyfw_host_tests,
        "build_saftyfw": build_saftyfw,
        "build_kilnfw": build_kilnfw,
    },
    "common": {
        "run_pctools_tests": run_pctools_tests,
        "run_repo_checks": run_repo_checks,
    },
}


def attach(tool: "Callable[[], Callable[[Any], Any]]", bundles: "Sequence[str]") -> "list[str]":
    """Register the named bundles onto a server through its own ``_tool()``.

    Registering through the caller's decorator rather than ``mcp.tool()``
    directly is deliberate: both servers wrap every tool in a never-raise guard,
    and a build runner that raised into the transport would be the one tool that
    could take the server down.
    """
    registered: "list[str]" = []
    for bundle in bundles:
        for name, fn in BUNDLES[bundle].items():
            tool()(fn)
            registered.append(name)
    return registered
