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
import os
import time
from typing import Iterator

_WAIT_OBJECT_0 = 0x00000000
_WAIT_ABANDONED = 0x00000080
_WAIT_TIMEOUT = 0x00000102
_WAIT_FAILED = 0xFFFFFFFF
_INFINITE = 0xFFFFFFFF


def _slot_count() -> int:
    raw = os.environ.get("KILNCTL_BUILD_GATE_SLOTS")
    if raw is None or not raw.strip():
        return 2
    try:
        return int(raw.strip())
    except ValueError:
        return 2


def _mutex_name(slot_index: int) -> str:
    # Must match tools/build_gate.ps1's Get-KilnBuildGateMutexName exactly --
    # this name IS the shared contract between the two languages.
    return f"Global\\kilnctl_build_slot_{slot_index}"


class _KernelMutex:
    """Thin wrapper around one named Win32 mutex handle."""

    def __init__(self, name: str) -> None:
        self._kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        self.handle = self._kernel32.CreateMutexW(None, False, name)
        if not self.handle:
            raise OSError(f"CreateMutexW({name!r}) failed: {ctypes.get_last_error()}")

    def wait(self, timeout_ms: int) -> int:
        return self._kernel32.WaitForSingleObject(self.handle, timeout_ms)

    def release(self) -> None:
        self._kernel32.ReleaseMutex(self.handle)

    def close(self) -> None:
        self._kernel32.CloseHandle(self.handle)


@contextlib.contextmanager
def kiln_build_gate(
    label: str,
    *,
    timeout_seconds: float = 3600.0,
    poll_interval_seconds: float = 30.0,
    log: "callable" = print,
) -> Iterator[None]:
    """Hold one of the machine-wide heavy-build slots for the ``with`` block.

    ``KILNCTL_BUILD_GATE_SLOTS=0`` disables this gate entirely (single-session
    machine only). Otherwise tries every slot non-blocking first, then polls
    slot 0 with a bounded wait, printing a waiting line every
    ``poll_interval_seconds`` so a slow build reads as gated, not hung --
    matching ``build_gate.ps1``'s own behavior exactly.
    """
    slots = _slot_count()
    if slots <= 0:
        log(f"build gate: disabled (KILNCTL_BUILD_GATE_SLOTS=0) for '{label}'")
        yield
        return

    mutexes = [_KernelMutex(_mutex_name(i)) for i in range(slots)]
    held: "_KernelMutex | None" = None
    try:
        # Non-blocking pass over every slot, round robin.
        for i, m in enumerate(mutexes):
            result = m.wait(0)
            if result in (_WAIT_OBJECT_0, _WAIT_ABANDONED):
                held = m
                log(f"build gate: acquired slot {i} for '{label}' (slots={slots})")
                break

        if held is None:
            # Every slot busy -- poll slot 0 with a bounded wait.
            m0 = mutexes[0]
            elapsed = 0.0
            log(f"build gate: waiting (label={label}, {elapsed:.0f}s, slots={slots})")
            while elapsed < timeout_seconds:
                chunk = min(poll_interval_seconds, timeout_seconds - elapsed)
                started = time.monotonic()
                result = m0.wait(int(chunk * 1000))
                elapsed += time.monotonic() - started
                if result in (_WAIT_OBJECT_0, _WAIT_ABANDONED):
                    held = m0
                    log(f"build gate: acquired slot 0 for '{label}' after {elapsed:.0f}s wait")
                    break
                if result == _WAIT_FAILED:
                    raise OSError(f"build gate: WaitForSingleObject failed: {ctypes.get_last_error()}")
                log(f"build gate: waiting (label={label}, {elapsed:.0f}s, slots={slots})")
            if held is None:
                raise TimeoutError(
                    f"build gate: timed out after {timeout_seconds:.0f}s waiting for a heavy-build "
                    f"slot (label={label}, slots={slots}) -- another run appears stuck holding every slot")

        try:
            yield
        finally:
            held.release()
            log(f"build gate: released slot for '{label}'")
    finally:
        for m in mutexes:
            m.close()
