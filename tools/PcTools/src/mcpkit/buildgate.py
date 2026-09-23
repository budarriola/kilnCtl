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
"""

from __future__ import annotations

import contextlib
import ctypes
import ctypes.wintypes as wintypes
import os
import sys
import time
from typing import Iterator

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


def _slot_count() -> int:
    raw = os.environ.get("KILNCTL_BUILD_GATE_SLOTS")
    if raw is None or not raw.strip():
        return 2
    try:
        return int(raw.strip())
    except ValueError:
        return 2


_DEFAULT_MUTEX_PREFIX = "Global\\kilnctl_build_slot_"


def _mutex_name(slot_index: int) -> str:
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


@contextlib.contextmanager
def kiln_build_gate(
    label: str,
    *,
    timeout_seconds: float = 3600.0,
    poll_interval_seconds: float = 30.0,
    log: "callable" = _log_to_stderr,
    wait_result: "GateWaitResult | None" = None,
) -> Iterator[None]:
    """Hold one of the machine-wide heavy-build slots for the ``with`` block.

    ``KILNCTL_BUILD_GATE_SLOTS=0`` disables this gate entirely (single-session
    machine only). Otherwise tries every slot non-blocking first, then waits
    on ALL slots at once (WaitForMultipleObjects, not just slot 0 -- opus
    review finding #2) with a bounded total timeout, printing a waiting line
    every ``poll_interval_seconds`` so a slow build reads as gated, not hung
    -- matching ``build_gate.ps1``'s own behavior. Pass a ``GateWaitResult``
    via ``wait_result`` to read back how long acquisition took, e.g. to fold
    "gate waited Ns" into a build report string (opus review finding A2).
    """
    slots = _slot_count()
    if slots <= 0:
        log(f"build gate: disabled (KILNCTL_BUILD_GATE_SLOTS=0) for '{label}'")
        yield
        return

    mutexes: "list[_KernelMutex]" = []
    try:
        for i in range(slots):
            # If CreateMutexW fails partway through, close what was already
            # opened rather than leaking those handles (opus review A4).
            mutexes.append(_KernelMutex(_mutex_name(i)))
    except Exception:
        for m in mutexes:
            m.close()
        raise

    held: "_KernelMutex | None" = None
    held_index = -1
    try:
        # Non-blocking pass over every slot, round robin.
        for i, m in enumerate(mutexes):
            result = m.wait(0)
            if result in (_WAIT_OBJECT_0, _WAIT_ABANDONED_0):
                held, held_index = m, i
                log(f"build gate: acquired slot {i} for '{label}' (slots={slots})")
                break
            if result not in (_WAIT_TIMEOUT,):
                raise OSError(f"build gate: WaitForSingleObject on slot {i} failed: {ctypes.get_last_error()}")

        if held is None:
            # Every slot busy -- wait on all of them together so a slot other
            # than 0 freeing up is noticed immediately, not just slot 0's.
            elapsed = 0.0
            log(f"build gate: waiting (label={label}, {elapsed:.0f}s, slots={slots})")
            while elapsed < timeout_seconds:
                chunk = min(poll_interval_seconds, timeout_seconds - elapsed)
                started = time.monotonic()
                idx = _wait_any(mutexes, int(chunk * 1000))
                elapsed += time.monotonic() - started
                if idx != _WAIT_TIMEOUT:
                    held, held_index = mutexes[idx], idx
                    log(f"build gate: acquired slot {idx} for '{label}' after {elapsed:.0f}s wait")
                    break
                log(f"build gate: waiting (label={label}, {elapsed:.0f}s, slots={slots})")
            if held is None:
                raise TimeoutError(
                    f"build gate: timed out after {timeout_seconds:.0f}s waiting for a heavy-build "
                    f"slot (label={label}, slots={slots}) -- another run appears stuck holding every slot")
            if wait_result is not None:
                wait_result.waited_seconds = elapsed

        try:
            yield
        finally:
            # A genuine ReleaseMutex failure here strands the slot until this
            # (long-lived, in the MCP server) process exits -- log it rather
            # than swallowing it silently (opus review advisory b).
            if not held.release():
                log(f"build gate: WARNING -- ReleaseMutex failed for slot {held_index} "
                    f"('{label}'): {ctypes.get_last_error()}")
            log(f"build gate: released slot {held_index} for '{label}'")
    finally:
        for m in mutexes:
            m.close()
