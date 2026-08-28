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

Output is summarized, not echoed. A full firmware build is thousands of lines;
what a caller needs is the exit status, the failing lines, and a path to the
rest. :func:`_summarize` keeps the tail plus every line that looks like a
diagnostic, and writes the complete log to the scratch directory.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Optional, Sequence

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
               elapsed: float) -> str:
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
    head = f"{tag}: {status} in {elapsed:.1f}s ({len(lines)} log lines)"
    body = "\n".join(shown) if shown else "(no output)"
    return f"{head}\nfull log: {where}\n--\n{body}"


#: Vars Git Bash sets that leak into this MCP server's environment when it is
#: launched from a Git Bash session. cmake sees MSYSTEM and refuses to
#: configure ("MSys/Mingw is no longer supported"), aborting build_kilnfw in
#: ~3s with no other output -- a false pass, since _summarize still reports
#: exit 0 for the wrapping idf.py invocation that never reached the compiler.
_MSYS_ENV_VARS = ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYS2_PATH_TYPE")


def _run(tag: str, argv: "Sequence[str]", *, cwd: Optional[str] = None,
         timeout: int = 900, env: "Optional[dict[str, str]]" = None) -> str:
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
    return _summarize(tag, argv, completed.returncode, output, time.monotonic() - started)


def _powershell(script: str, extra: "Sequence[str]" = ()) -> "list[str]":
    return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, *extra]


# ---------------------------------------------------------------------------
# the tools themselves
# ---------------------------------------------------------------------------
def build_simfw_host_tests() -> str:
    """Build and run SimFW's off-target host unit tests (MSVC, no hardware).

    ``firmware/SimFW/test/build_host_tests.ps1`` -- everything under
    ``src/sim/`` is pure C and runs on the PC. This is the fastest check that a
    change to the thermal model, the fault engine, or the MAX31856 register
    emulation still holds; it needs no fixture attached.
    """
    root = repo_root()
    return _run("simfw-host-tests",
                _powershell(os.path.join(root, "firmware", "SimFW", "test", "build_host_tests.ps1")))


def build_saftyfw_host_tests() -> str:
    """Build and run SaftyFW's off-target host unit tests (MSVC, no hardware).

    ``firmware/SaftyFW/test/build_host_tests.ps1``. If this reports a failure
    without naming a single failed check, suspect the toolchain wrapper rather
    than the code -- ``vcvarsall.bat`` writes a benign line to stderr that some
    PowerShell hosts promote to a terminating error.
    """
    root = repo_root()
    return _run("saftyfw-host-tests",
                _powershell(os.path.join(root, "firmware", "SaftyFW", "test", "build_host_tests.ps1")))


def build_simfw(jobs: int = 0) -> str:
    """Build the SimFW fixture firmware (``firmware/SimFW/build``, ninja).

    Produces ``SimFW.elf``, which ``debug_program(peer="sim")`` flashes to the
    fixture over SWD. Requires the build directory to have been configured with
    CMake once; this only rebuilds.
    """
    return _cmake_build("simfw", os.path.join(repo_root(), "firmware", "SimFW", "build"), jobs)


def build_saftyfw(jobs: int = 0) -> str:
    """Build the SaftyFW safety-processor firmware (``firmware/SaftyFW/build``).

    Produces the ELF that ``debug_program(peer="pico")`` flashes over SWD.
    """
    return _cmake_build("saftyfw", os.path.join(repo_root(), "firmware", "SaftyFW", "build"), jobs)


def _cmake_build(tag: str, build_dir: str, jobs: int) -> str:
    if not os.path.isdir(build_dir):
        return (f"{tag}: error: {build_dir} does not exist -- configure it once with "
                f"`cmake -S {os.path.dirname(build_dir)} -B {build_dir} -G Ninja` first")
    argv = ["cmake", "--build", build_dir]
    if jobs > 0:
        argv += ["--parallel", str(jobs)]
    return _run(tag, argv, cwd=build_dir)


#: Puts idf.py, cmake, ninja and the Xtensa toolchain on PATH in one step.
#: Everything else is a partial environment that fails later and less clearly.
_IDF_PROFILE = r"C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"


def build_kilnfw(target: str = "build", jobs: int = 0) -> str:
    """Build the ESP32-S3 main firmware (``firmware/KilnFW``) via ESP-IDF.

    ``target`` is an idf.py target -- ``build``, ``fullclean``, ``reconfigure``.
    ``jobs`` above zero bypasses idf.py and calls ninja directly, because
    idf.py's argument parser rejects ``-- -j N``; it therefore only applies to
    an already-configured build directory and is ignored for other targets.

    Flashing is deliberately not offered here: this board is programmed over
    JTAG with OpenOCD (``flash_firmware`` / ``debug_program``), never esptool.
    """
    root = repo_root()
    if not os.path.isfile(_IDF_PROFILE):
        return (f"kilnfw: error: ESP-IDF profile not found at {_IDF_PROFILE} -- "
                f"update _IDF_PROFILE in mcpkit/workbench.py if Espressif moved")
    if jobs > 0 and target == "build":
        inner = f"cd '{os.path.join(root, 'firmware', 'KilnFW', 'build')}'; ninja -j {jobs}"
    else:
        inner = f"idf.py -C '{os.path.join(root, 'firmware', 'KilnFW')}' {target}"
    # powershell.exe does NOT propagate a native command's exit code as its own
    # process exit code unless the script explicitly does so -- without the
    # trailing `exit $LASTEXITCODE`, this always returned 0 even when idf.py
    # or ninja failed, which is exactly the false-pass bug this wrapper exists
    # to avoid. See _MSYS_ENV_VARS above for the sibling false-pass this same
    # command is also guarding against.
    command = f"& '{_IDF_PROFILE}' *>&1 | Out-Null; {inner}; exit $LASTEXITCODE"
    return _run(f"kilnfw-{target}",
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
                timeout=1800)


def run_pctools_tests(pattern: Optional[str] = None) -> str:
    """Run the PC-side pytest suite (``tools/PcTools/tests``).

    ``pattern`` is passed to pytest's ``-k``, so ``pattern="kilnsim"`` runs only
    the fixture-side tests. This is the suite that covers the protocol codecs,
    the scenario runner, and these MCP servers themselves.
    """
    root = repo_root()
    argv = [sys.executable, "-m", "pytest", os.path.join(root, "tools", "PcTools", "tests"), "-q"]
    if pattern:
        argv += ["-k", pattern]
    return _run("pctools-tests", argv, timeout=600)


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
    "simfw": {
        "build_simfw_host_tests": build_simfw_host_tests,
        "build_simfw": build_simfw,
    },
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
