"""Machine-wide heavy-build admission gate, shared with the PowerShell side.

``tools/build_gate.ps1`` is the PowerShell half of this: N named kernel
mutexes (``Global\\kilnctl_build_slot_<i>``) admit at most N heavy builds at
once, machine-wide, across every session -- not just within one build
directory the way :mod:`mcpkit.buildlock` (a *different* lock, scoped to a
resource key such as a build directory) already does. Both exist because this
machine hard-froze repeatedly under 7-11 agents each running a full ESP-IDF
target build or MSVC host-test build concurrently in separate worktrees --
``mcpkit.buildlock`` only ever serializes two writers of the SAME directory,
so it does nothing when every agent builds its own isolated tree at once.

This module talks to the SAME kernel mutex objects the PowerShell script
creates, via ``ctypes`` (``CreateMutexW``/``WaitForSingleObject``/
``ReleaseMutex``), rather than shelling out to PowerShell for every build --
the mutex names are the only shared contract, and Windows named kernel
objects are visible across processes (and across the Python/PowerShell
language boundary) as long as both sides use the same name and the
``Global\\`` prefix.

A Mutex, not a Semaphore, for the same reason ``build_gate.ps1`` documents:
a semaphore's count is never restored if the holding process is killed, but
a mutex is released by the kernel the instant its owner dies, and a wait that
returns ``WAIT_ABANDONED`` still means the caller now owns it.

Mirror of tools/build_gate.ps1 (keep behavior identical; tests/test_buildgate.py
has a PS/Python parity test). Rules: slot held ONLY around the compile; waiters
queue by ticket file (FIFO) and wait on ALL slots; abandoned = acquired + logged;
holder record per slot (``<dir>/<lane>/slot<i>.json``) removed on release;
holder-side max hold (``KILNCTL_BUILD_GATE_MAX_HOLD_SEC``, default 2700) kills
only the holder's own descendants, releases, and raises; no waiter ever kills
anything; slot counts are machine-wide (env > ``<dir>/config.json`` > 4);
re-entrant per thread, and across processes via ``child_env()`` /
``KILNCTL_BUILD_GATE_HELD``.
"""

from __future__ import annotations

import contextlib
import ctypes
import ctypes.wintypes as wintypes
import json
import os
import re
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Iterator

DEFAULT_SLOTS = 4
DEFAULT_MAX_HOLD_SEC = 2700.0
_TICKET_STALE_SEC = 120.0
_COMPILER_RE = re.compile(
    r"^(cl|link|ninja|cmake|cc1|cc1plus|ccache|xtensa-.+|arm-none-eabi-.+)(\.exe)?$", re.I)

_WAIT_OBJECT_0 = 0x00000000
_WAIT_ABANDONED_0 = 0x00000080
_WAIT_TIMEOUT = 0x00000102
_WAIT_FAILED = 0xFFFFFFFF
_INFINITE = 0xFFFFFFFF

# The MCP server (workbench.py) talks to its client over stdio -- a stray
# print() to stdout from inside a build tool call would corrupt that
# framing. Every gate log line goes to stderr by default for that reason
# (opus review finding A2); a caller with its own transport can still pass
# a different `log`.
def _log_to_stderr(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


class GateWaitResult:
    """Returned alongside the held gate: how long acquisition actually took."""

    __slots__ = ("waited_seconds",)

    def __init__(self, waited_seconds: float = 0.0) -> None:
        self.waited_seconds = waited_seconds


def gate_dir() -> Path:
    return Path(os.environ.get("KILNCTL_BUILD_GATE_DIR") or r"C:\wt\.buildgate")


def _lane_dir(lane: str) -> Path:
    return gate_dir() / lane


def _read_config() -> "dict | None":
    path = gate_dir() / "config.json"
    try:
        if not path.exists():
            gate_dir().mkdir(parents=True, exist_ok=True)
            path.write_text('{"heavy_slots": 4, "light_slots": 4}\n')
        return json.loads(path.read_text())
    except Exception:
        return None


def _configured_slot_count(lane: str = "heavy") -> int:
    env_name = "KILNCTL_LIGHT_GATE_SLOTS" if lane == "light" else "KILNCTL_BUILD_GATE_SLOTS"
    raw = os.environ.get(env_name)
    if raw is not None and raw.strip():
        try:
            return int(raw.strip())
        except ValueError:
            print(f"build gate: {env_name}={raw!r} is not an integer, ignoring", file=sys.stderr)
    cfg = _read_config()
    if cfg is not None:
        try:
            n = int(cfg.get("light_slots" if lane == "light" else "heavy_slots"))
            if n >= 0:
                return n
        except (TypeError, ValueError):
            pass
    return DEFAULT_SLOTS


def gate_dir() -> Path:
    return Path(os.environ.get("KILNCTL_BUILD_GATE_DIR") or r"C:\wt\.buildgate")


def _lane_dir(lane: str) -> Path:
    return gate_dir() / lane


def _read_config() -> "dict | None":
    path = gate_dir() / "config.json"
    try:
        if not path.exists():
            gate_dir().mkdir(parents=True, exist_ok=True)
            path.write_text('{"heavy_slots": 4, "light_slots": 4}\n')
        return json.loads(path.read_text())
    except Exception:
        return None


def _configured_slot_count(lane: str = "heavy") -> int:
    env_name = "KILNCTL_LIGHT_GATE_SLOTS" if lane == "light" else "KILNCTL_BUILD_GATE_SLOTS"
    raw = os.environ.get(env_name)
    if raw is not None and raw.strip():
        try:
            return int(raw.strip())
        except ValueError:
            print(f"build gate: {env_name}={raw!r} is not an integer, ignoring", file=sys.stderr)
    cfg = _read_config()
    if cfg is not None:
        try:
            n = int(cfg.get("light_slots" if lane == "light" else "heavy_slots"))
            if n >= 0:
                return n
        except (TypeError, ValueError):
            pass
    return DEFAULT_SLOTS


def _slot_count(lane: str = "heavy") -> int:
    """Configured count widened to cover any slot with a live record."""
    n = _configured_slot_count(lane)
    if n <= 0:
        return n
    for r in read_records(lane):
        if r["alive"] and r["slot"] + 1 > n:
            n = r["slot"] + 1
    return n


def max_hold_seconds() -> float:
    try:
        v = float(os.environ.get("KILNCTL_BUILD_GATE_MAX_HOLD_SEC", "").strip())
        if v > 0:
            return v
    except ValueError:
        pass
    return DEFAULT_MAX_HOLD_SEC


_DEFAULT_MUTEX_PREFIX = "Global\\kilnctl_build_slot_"
# Light lane: a separate pool for genuinely small compiles, see
# tools/build_gate.ps1's "LIGHT LANE" header. Must match that file's names.
_DEFAULT_LIGHT_MUTEX_PREFIX = "Global\\kilnctl_build_light_"


def _mutex_name(slot_index: int, lane: str = "heavy") -> str:
    # Must match tools/build_gate.ps1's Get-KilnBuildGateMutexName exactly --
    # this name IS the shared contract between the two languages. The prefix
    # is overridable via KILNCTL_BUILD_GATE_MUTEX_PREFIX so tests can point
    # at a private Local\ namespace instead of contending with a real build
    # holding the machine-wide Global\ slots (opus review of 171cc5bc,
    # advisory 3) -- production code never sets this env var, so it always
    # gets the real Global\ prefix below.
    #
    # This is a test/diagnostics knob only, not a normal operator setting: if
    # it is set at all, it MUST be set to the exact same value on both the
    # Python side (here) and the PowerShell side
    # (tools/build_gate.ps1's Get-KilnBuildGateMutexName), or the two sides
    # silently gate on different mutexes and stop admission-controlling each
    # other for the same real build. Leave it unset on both sides for every
    # real build.
    if lane == "light":
        prefix = os.environ.get("KILNCTL_LIGHT_GATE_MUTEX_PREFIX") or _DEFAULT_LIGHT_MUTEX_PREFIX
        return f"{prefix}{slot_index}"
    prefix = os.environ.get("KILNCTL_BUILD_GATE_MUTEX_PREFIX") or _DEFAULT_MUTEX_PREFIX
    return f"{prefix}{slot_index}"


# ctypes.windll.kernel32 with no restype/argtypes silently treats every
# return as a plain (signed, on most builds) C int and never populates the
# real last-error code -- WaitForSingleObject's failure sentinel
# (0xFFFFFFFF) can never be observed that way (it prints as -1), and
# ctypes.get_last_error() reads 0 unless the DLL was opened with
# use_last_error=True. Both are opus review finding A1's root cause; fixed
# here by opening kernel32 explicitly and giving every entry point its real
# Win32 signature.
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_kernel32.CreateMutexW.restype = wintypes.HANDLE
_kernel32.CreateMutexW.argtypes = (wintypes.LPCVOID, wintypes.BOOL, wintypes.LPCWSTR)

_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)

_kernel32.WaitForMultipleObjects.restype = wintypes.DWORD
_kernel32.WaitForMultipleObjects.argtypes = (
    wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE), wintypes.BOOL, wintypes.DWORD)

_kernel32.ReleaseMutex.restype = wintypes.BOOL
_kernel32.ReleaseMutex.argtypes = (wintypes.HANDLE,)

_kernel32.CloseHandle.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)


class _KernelMutex:
    """Thin wrapper around one named Win32 mutex handle."""

    def __init__(self, name: str) -> None:
        self.handle = _kernel32.CreateMutexW(None, False, name)
        if not self.handle:
            raise OSError(f"CreateMutexW({name!r}) failed: {ctypes.get_last_error()}")

    def wait(self, timeout_ms: int) -> int:
        return _kernel32.WaitForSingleObject(self.handle, timeout_ms)

    def release(self) -> bool:
        return bool(_kernel32.ReleaseMutex(self.handle))

    def close(self) -> None:
        _kernel32.CloseHandle(self.handle)


def _wait_any(mutexes: "list[_KernelMutex]", timeout_ms: int) -> int:
    """WaitForMultipleObjects(bWaitAll=False) over every handle.

    Returns the signaled/abandoned slot index (0..n-1), or _WAIT_TIMEOUT.
    Watching only slot 0 (the original implementation) meant a queued
    waiter never noticed slot 1..n-1 freeing up -- opus review finding A2/#2:
    the timeout could be reached while another slot sat idle the whole time.
    """
    n = len(mutexes)
    handles = (wintypes.HANDLE * n)(*(m.handle for m in mutexes))
    result = _kernel32.WaitForMultipleObjects(n, handles, False, timeout_ms)
    if _WAIT_OBJECT_0 <= result < _WAIT_OBJECT_0 + n:
        return result - _WAIT_OBJECT_0
    if _WAIT_ABANDONED_0 <= result < _WAIT_ABANDONED_0 + n:
        return result - _WAIT_ABANDONED_0
    if result == _WAIT_FAILED:
        raise OSError(f"build gate: WaitForMultipleObjects failed: {ctypes.get_last_error()}")
    return _WAIT_TIMEOUT


def _wait_any_ex(mutexes: "list[_KernelMutex]", timeout_ms: int) -> "tuple[int, bool]":
    """Like _wait_any but also says whether the acquired mutex was abandoned."""
    n = len(mutexes)
    handles = (wintypes.HANDLE * n)(*(m.handle for m in mutexes))
    result = _kernel32.WaitForMultipleObjects(n, handles, False, timeout_ms)
    if _WAIT_OBJECT_0 <= result < _WAIT_OBJECT_0 + n:
        return result - _WAIT_OBJECT_0, False
    if _WAIT_ABANDONED_0 <= result < _WAIT_ABANDONED_0 + n:
        return result - _WAIT_ABANDONED_0, True
    if result == _WAIT_FAILED:
        raise OSError(f"build gate: WaitForMultipleObjects failed: {ctypes.get_last_error()}")
    return _WAIT_TIMEOUT, False


# ---- process helpers --------------------------------------------------------

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_PROCESS_TERMINATE = 0x0001
_TH32CS_SNAPPROCESS = 0x2


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]


_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_kernel32.GetProcessTimes.restype = wintypes.BOOL
_kernel32.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
_kernel32.GetExitCodeProcess.restype = wintypes.BOOL
_kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
_kernel32.TerminateProcess.restype = wintypes.BOOL
_kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
_kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
_kernel32.Process32FirstW.restype = wintypes.BOOL
_kernel32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W))
_kernel32.Process32NextW.restype = wintypes.BOOL
_kernel32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W))


def process_start_epoch(pid: int) -> "int | None":
    h = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        code = wintypes.DWORD()  # only STILL_ACTIVE (259) is running; a held handle can outlive exit
        if not _kernel32.GetExitCodeProcess(h, ctypes.byref(code)) or code.value != 259:
            return None
        c, e, k, u = (wintypes.FILETIME() for _ in range(4))
        if not _kernel32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
            return None
        ticks = (c.dwHighDateTime << 32) | c.dwLowDateTime
        return int(ticks / 10_000_000 - 11644473600)
    finally:
        _kernel32.CloseHandle(h)


def pid_alive(pid: int, start_epoch: "int | None" = None) -> bool:
    actual = process_start_epoch(pid)
    if actual is None:
        return False
    if start_epoch is not None and abs(actual - int(start_epoch)) > 2:
        return False
    return True


def process_snapshot() -> "dict[int, tuple[int, str]]":
    """pid -> (parent pid, exe name)."""
    out: "dict[int, tuple[int, str]]" = {}
    snap = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if not snap or snap == wintypes.HANDLE(-1).value:
        return out
    try:
        e = _PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = _kernel32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            out[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile)
            ok = _kernel32.Process32NextW(snap, ctypes.byref(e))
    finally:
        _kernel32.CloseHandle(snap)
    return out


def descendants(root: int, snapshot: "dict[int, tuple[int, str]] | None" = None) -> "list[tuple[int, str]]":
    """Descendants of ``root``, deepest first."""
    snap = snapshot if snapshot is not None else process_snapshot()
    order: "list[tuple[int, str]]" = []
    queue, seen = [root], {root}
    while queue:
        cur = queue.pop(0)
        for pid, (parent, name) in snap.items():
            if parent == cur and pid not in seen:
                seen.add(pid)
                order.append((pid, name))
                queue.append(pid)
    order.reverse()
    return order


def _kill_descendants(root: int) -> None:
    for pid, _ in descendants(root):
        h = _kernel32.OpenProcess(_PROCESS_TERMINATE, False, pid)
        if h:
            try:
                _kernel32.TerminateProcess(h, 1)
            finally:
                _kernel32.CloseHandle(h)


# ---- records / tickets (formats identical to build_gate.ps1) -----------------

def _record_path(lane: str, slot: int) -> Path:
    return _lane_dir(lane) / f"slot{slot}.json"


def _write_record(lane: str, slot: int, label: str, phase: str) -> None:
    d = _lane_dir(lane)
    d.mkdir(parents=True, exist_ok=True)
    now = time.time()
    rec = {
        "pid": os.getpid(), "proc_start": process_start_epoch(os.getpid()),
        "cmdline": " ".join(sys.argv)[:400], "label": label, "lane": lane, "slot": slot,
        "phase": phase, "started_epoch": round(now, 3),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
        "worktree": os.getcwd(),
    }
    _record_path(lane, slot).write_text(json.dumps(rec, separators=(",", ":")))


def _remove_record(lane: str, slot: int) -> None:
    path = _record_path(lane, slot)
    try:
        if path.exists() and int(json.loads(path.read_text())["pid"]) == os.getpid():
            path.unlink()
    except Exception:
        pass


def read_records(lane: str = "heavy") -> "list[dict]":
    out = []
    d = _lane_dir(lane)
    if not d.is_dir():
        return out
    for f in d.glob("slot*.json"):
        try:
            r = json.loads(f.read_text())
            out.append({
                "slot": int(r["slot"]), "pid": int(r["pid"]),
                "alive": pid_alive(int(r["pid"]), r.get("proc_start")),
                "cmd": str(r.get("cmdline", "")), "label": str(r.get("label", "")),
                "phase": str(r.get("phase", "")), "started_epoch": float(r["started_epoch"]),
                "worktree": str(r.get("worktree", "")),
            })
        except Exception:
            continue
    return out


def _queue_dir(lane: str) -> Path:
    return _lane_dir(lane) / "queue"


def _ticket_body(lane: str, label: str) -> str:
    return json.dumps({"pid": os.getpid(), "proc_start": process_start_epoch(os.getpid()),
                       "label": label, "lane": lane}, separators=(",", ":"))


def _new_ticket(lane: str, label: str) -> str:
    d = _queue_dir(lane)
    d.mkdir(parents=True, exist_ok=True)
    name = f"{int(time.time() * 1000):015d}-{os.getpid()}-{uuid.uuid4().hex[:8]}.ticket"
    (d / name).write_text(_ticket_body(lane, label))
    return name


def live_tickets(lane: str) -> "list[str]":
    """Live ticket names, oldest first; deletes tickets whose pid is dead."""
    d = _queue_dir(lane)
    live: "list[str]" = []
    if not d.is_dir():
        return live
    now = time.time()
    for f in d.glob("*.ticket"):
        try:
            t = json.loads(f.read_text())
            alive = pid_alive(int(t["pid"]), t.get("proc_start"))
        except Exception:
            alive = True
        if not alive:
            try:
                f.unlink()
            except OSError:
                pass
            continue
        try:
            if now - f.stat().st_mtime > _TICKET_STALE_SEC:
                continue
        except OSError:
            continue
        live.append(f.name)
    return sorted(live)


def _remove_ticket(lane: str, name: str) -> None:
    try:
        (_queue_dir(lane) / name).unlink()
    except OSError:
        pass


# ---- re-entrancy -------------------------------------------------------------

_tls = threading.local()


def _covered() -> "dict | None":
    held = getattr(_tls, "held", None)
    if held is not None:
        return held
    raw = os.environ.get("KILNCTL_BUILD_GATE_HELD", "")
    parts = raw.split("|")
    if len(parts) != 3:
        return None
    try:
        lane, slot, pid = parts[0], int(parts[1]), int(parts[2])
        r = json.loads(_record_path(lane, slot).read_text())
        if int(r["pid"]) == pid and pid_alive(pid, r.get("proc_start")):
            return {"lane": lane, "slot": slot, "depth": 0, "external": True}
    except Exception:
        pass
    return None


def child_env(base: "dict | None" = None) -> "dict[str, str]":
    """Environment for a subprocess started while this thread holds a slot, so
    a child that also calls Enter-KilnBuildGate / kiln_build_gate is re-entrant
    instead of taking a second slot. (Per-thread state, so the process-wide
    os.environ is deliberately NOT mutated -- the MCP server is multithreaded.)"""
    env = dict(os.environ if base is None else base)
    held = getattr(_tls, "held", None)
    if held is not None:
        env["KILNCTL_BUILD_GATE_HELD"] = f"{held['lane']}|{held['slot']}|{os.getpid()}"
    return env


# ---- the gate ----------------------------------------------------------------

@contextlib.contextmanager
def kiln_build_gate(
    label: str,
    *,
    timeout_seconds: float = 3600.0,
    poll_interval_seconds: float = 30.0,
    log: "callable" = _log_to_stderr,
    wait_result: "GateWaitResult | None" = None,
    lane: str = "heavy",
    phase: str = "compile",
    max_hold_seconds_override: "float | None" = None,
) -> Iterator[None]:
    """Hold one machine-wide build slot for the ``with`` block.

    Wrap ONLY the compile/link subprocess, never a lock wait, toolchain setup,
    test run or sleep. See the module docstring and tools/build_gate.ps1.
    ``KILNCTL_BUILD_GATE_SLOTS=0`` disables the heavy lane (``..LIGHT_GATE_SLOTS``
    the light one). Past the max hold (``KILNCTL_BUILD_GATE_MAX_HOLD_SEC``) the
    holder's own child processes are killed, the slot is released and
    ``RuntimeError`` is raised on exit.
    """
    if lane not in ("heavy", "light"):
        raise ValueError(f"build gate: unknown lane {lane!r}")

    cov = _covered()
    if cov is not None:
        cov["depth"] += 1
        log(f"build gate: '{label}' already covered by held {cov['lane']} slot {cov['slot']}; "
            f"re-entrant, no second slot")
        try:
            yield
        finally:
            cov["depth"] -= 1
        return

    slots = _slot_count(lane)
    if slots <= 0:
        log(f"build gate: {lane} lane disabled (slot count 0) for '{label}'")
        yield
        return

    mutexes: "list[_KernelMutex]" = []
    try:
        for i in range(slots):
            mutexes.append(_KernelMutex(_mutex_name(i, lane)))
    except Exception:
        for m in mutexes:
            m.close()
        raise

    held: "_KernelMutex | None" = None
    held_index = -1
    abandoned = False
    started_wait = time.monotonic()
    ticket: "str | None" = None
    try:
        if not live_tickets(lane):
            for i, m in enumerate(mutexes):
                result = m.wait(0)
                if result in (_WAIT_OBJECT_0, _WAIT_ABANDONED_0):
                    held, held_index, abandoned = m, i, result == _WAIT_ABANDONED_0
                    log(f"build gate: acquired {lane} slot {i} for '{label}' (slots={slots})")
                    break
                if result != _WAIT_TIMEOUT:
                    raise OSError(f"build gate: WaitForSingleObject on slot {i} failed: {ctypes.get_last_error()}")

        if held is None:
            ticket = _new_ticket(lane, label)
            tpath = _queue_dir(lane) / ticket
            last_log = -poll_interval_seconds
            while True:
                elapsed = time.monotonic() - started_wait
                if elapsed >= timeout_seconds:
                    break
                live = live_tickets(lane)
                try:
                    if tpath.exists():
                        os.utime(tpath)
                    else:
                        tpath.write_text(_ticket_body(lane, label))
                except OSError:
                    pass
                rank = live.index(ticket) if ticket in live else len(live)
                if elapsed - last_log >= poll_interval_seconds:
                    last_log = elapsed
                    log(f"build gate: waiting (label={label}, lane={lane}, {elapsed:.0f}s, "
                        f"queue position {rank + 1} of {len(live)}, slots={slots})")
                if rank != 0:
                    time.sleep(0.5)
                    continue
                idx, ab = _wait_any_ex(mutexes, 1000)
                if idx != _WAIT_TIMEOUT:
                    held, held_index, abandoned = mutexes[idx], idx, ab
                    log(f"build gate: acquired {lane} slot {idx} for '{label}' after {elapsed:.0f}s wait (slots={slots})")
                    break
            if held is None:
                raise TimeoutError(
                    f"build gate: timed out after {timeout_seconds:.0f}s waiting for a {lane}-lane "
                    f"build slot (label={label}, slots={slots}) -- run tools\\build_gate.ps1 -Status to see who holds them")
            if wait_result is not None:
                wait_result.waited_seconds = time.monotonic() - started_wait
        elif wait_result is not None:
            wait_result.waited_seconds = 0.0
    except BaseException:
        if held is not None:
            held.release()
        for m in mutexes:
            m.close()
        raise
    finally:
        if ticket is not None:
            _remove_ticket(lane, ticket)

    if abandoned:
        log(f"build gate: WARNING -- {lane} slot {held_index} was ABANDONED by a dead process; "
            f"reclaimed it for '{label}'")
    limit = max_hold_seconds_override if max_hold_seconds_override else max_hold_seconds()
    try:
        _write_record(lane, held_index, label, phase)
    except Exception as exc:
        log(f"build gate: WARNING -- could not write holder record: {exc}")
    state = {"lane": lane, "slot": held_index, "depth": 1}
    _tls.held = state
    stop = threading.Event()
    expired = {"flag": False, "held": 0.0}
    acquired_at = time.monotonic()

    def _watch() -> None:
        if stop.wait(limit):
            return
        expired["flag"] = True
        expired["held"] = time.monotonic() - acquired_at
        log(f"build gate: MAX HOLD EXCEEDED -- {lane} slot {held_index} ('{label}') held "
            f"{expired['held']:.1f}s > limit {limit}s (KILNCTL_BUILD_GATE_MAX_HOLD_SEC); killing this "
            f"holder's own build children and releasing the slot")
        _kill_descendants(os.getpid())

    watchdog = threading.Thread(target=_watch, name="build-gate-watchdog", daemon=True)
    watchdog.start()
    try:
        yield
    finally:
        stop.set()
        watchdog.join(timeout=5)
        _remove_record(lane, held_index)
        _tls.held = None
        if not held.release():
            log(f"build gate: WARNING -- ReleaseMutex failed for slot {held_index} "
                f"('{label}'): {ctypes.get_last_error()}")
        for m in mutexes:
            m.close()
        held_for = time.monotonic() - acquired_at
        log(f"build gate: released {lane} slot {held_index} for '{label}' (held {held_for:.0f}s)")
    if expired["flag"]:
        raise RuntimeError(
            f"build gate: {lane} slot {held_index} held by '{label}' for {held_for:.0f}s, over the max hold "
            f"of {limit}s (KILNCTL_BUILD_GATE_MAX_HOLD_SEC); build children killed, slot released")


# ---- status ------------------------------------------------------------------

def get_status() -> "list[dict]":
    snap = process_snapshot()
    now = time.time()
    rows = []
    for lane in ("heavy", "light"):
        slots = _slot_count(lane)
        recs = {r["slot"]: r for r in read_records(lane)}
        for i in range(max(slots, 0)):
            row = {"lane": lane, "slot": i, "state": "free", "pid": None, "cmd": None, "label": None,
                   "phase": None, "age_sec": None, "pid_alive": None, "compiling": False,
                   "compilers": [], "worktree": None}
            r = recs.get(i)
            if r is not None:
                row.update(pid=r["pid"], cmd=r["cmd"], label=r["label"], phase=r["phase"],
                           worktree=r["worktree"], pid_alive=r["alive"],
                           age_sec=round(now - r["started_epoch"], 1))
                if r["alive"]:
                    row["state"] = "held"
                    comp = [n for _, n in descendants(r["pid"], snap) if _COMPILER_RE.match(n)]
                    row["compilers"] = comp
                    row["compiling"] = bool(comp)
                else:
                    row["state"] = "stale"
            if row["state"] != "held":
                m = _KernelMutex(_mutex_name(i, lane))
                try:
                    res = m.wait(0)
                    if res in (_WAIT_OBJECT_0, _WAIT_ABANDONED_0):
                        m.release()
                    else:
                        row["state"] = "held-no-record"
                finally:
                    m.close()
            rows.append(row)
    return rows
