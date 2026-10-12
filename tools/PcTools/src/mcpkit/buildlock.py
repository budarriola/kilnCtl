"""Cross-process serialization for build/test entry points that share a
build directory.

The defect this exists for: two agents each running ``build_kilnfw`` (or
``build_saftyfw_host_tests``, etc.) at the same time write into the *same*
build directory concurrently. Observed today -- do not re-derive:

* ``ninja: error: failed recompaction: Permission denied`` on ``build.ninja``
  (ninja's own lock file, but two ninja processes still stepped on each
  other rebuilding the manifest).
* A corrupted ``build_info.h`` -- one build's ``file(WRITE)``/``file(APPEND)``
  sequence in ``gen_build_info.cmake`` interleaved with another's, producing
  "unknown type name 'by'" (a spliced-together fragment of the header
  comment). That specific race is now also closed at the source by writing
  the header to a temp file and ``file(RENAME)``-ing it into place (see
  ``firmware/KilnFW/App/drivers/gen_build_info.cmake``), but the build
  directory itself -- ninja's own state, object files, the CMake cache --
  is still one shared mutable resource with no such protection, so this
  lock is still needed independent of that fix.

Both failure modes present as compiler/linker errors in an unrelated file,
which sends whoever hits them chasing a phantom source bug.

Mechanism: a lock *file* created with ``os.O_CREAT | os.O_EXCL``. That flag
combination is atomic on Windows -- ``os.open`` maps it to
``CreateFileW(..., CREATE_NEW, ...)``, which fails outright if the file
already exists, with no window for two processes to both believe they
created it. No extra dependency (no ``msvcrt.locking``, no third-party
``portalocker``): the stdlib primitive already does the job and is what
those wrap internally.

The lock file's content is ``"<pid> <holder start epoch> <token>"``. Age
staleness is judged from the file's mtime, so a lock created by a process
killed before it finished the write is still reclaimable; a dead or reused
PID reclaims at once. Every reclaim goes through an O_EXCL ``.reclaim``
sidecar that re-reads the lock, and release removes the lock only if the
token is still the holder's own.

Stale-lock timeout: a lock older than ``stale_after`` seconds is assumed to
belong to a dead holder (crashed process, killed MCP server, rebooted box)
and is removed so a waiter is never stuck forever. Default is 2100s (35
minutes) -- comfortably longer than ``build_kilnfw``'s own 1800s subprocess
timeout, so a *live*, merely-slow build is never treated as dead and raced
by a second holder; short enough that a genuinely abandoned lock does not
wedge the tool for the rest of the day.

A waiter blocks, polling every ``poll_interval`` seconds, until it acquires
the lock or ``wait_timeout`` elapses, at which point it raises
:class:`BuildLockTimeout` with how long it waited -- never an immediate
failure, never an unbounded hang.
"""

from __future__ import annotations

import contextlib
import os
import re
import tempfile
import time
import uuid
from typing import Iterator


class BuildLockTimeout(Exception):
    """Raised when a build lock could not be acquired within its timeout."""

    def __init__(self, resource: str, waited: float, holder: "str | None") -> None:
        self.resource = resource
        self.waited = waited
        self.holder = holder
        holder_note = f" (held by: {holder})" if holder else ""
        super().__init__(
            f"another build is in progress on {resource!r} -- waited {waited:.1f}s "
            f"for the lock and gave up{holder_note}"
        )


def _lock_dir() -> str:
    directory = os.path.join(tempfile.gettempdir(), "kilnctl-builds", "locks")
    os.makedirs(directory, exist_ok=True)
    return directory


_SAFE = re.compile(r"[^A-Za-z0-9_.-]+")


def _lock_path(resource_key: str) -> str:
    safe = _SAFE.sub("_", resource_key)
    return os.path.join(_lock_dir(), f"{safe}.lock")


def _parse_holder(text: "str | None") -> "tuple[int, float, str | None] | None":
    """(pid, second field, token or None) from a lock file's content, or None.

    New format: ``<pid> <holder process start epoch> <token>``. Legacy format
    ``<pid> <acquired time>`` has no token, and its second field is NOT a
    process start time, so callers only use it for the dead-PID test.
    """
    if not text:
        return None
    parts = text.split()
    if len(parts) < 2:
        return None
    try:
        pid, second = int(parts[0]), float(parts[1])
    except ValueError:
        return None
    return pid, second, (parts[2] if len(parts) >= 3 else None)


def _pid_info(pid: int) -> "tuple[bool, float | None]":
    """(alive, process start unix time or None when unknown)."""
    if pid <= 0:
        return False, None
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        # buildgate declares argtypes/restype for these calls.
        from .buildgate import _kernel32 as k32
        handle = k32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            # 5 = access denied: the process exists. Anything else: gone.
            return ctypes.get_last_error() == 5, None
        try:
            code = wintypes.DWORD()
            if not k32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return True, None
            if code.value != 259:  # STILL_ACTIVE
                return False, None
            c, e, k, u = (wintypes.FILETIME() for _ in range(4))
            if not k32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(e),
                                       ctypes.byref(k), ctypes.byref(u)):
                return True, None
            ticks = (c.dwHighDateTime << 32) | c.dwLowDateTime
            return True, ticks / 1e7 - 11644473600.0
        finally:
            k32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False, None
    except OSError:
        return True, None
    return True, None


# Same tolerance as buildgate.pid_alive(pid, start_epoch).
_START_SLACK_S = 2.0


def _text_is_dead_holder(text: "str | None") -> bool:
    """True when the lock content names a PID that is gone, or alive with a
    process start time different from the one the holder recorded (PID
    reuse). Unparseable content, or an unknown start time: False."""
    parsed = _parse_holder(text)
    if parsed is None:
        return False
    pid, second, token = parsed
    alive, started = _pid_info(pid)
    if not alive:
        return True
    if token is None or second <= 0 or started is None:
        return False
    return abs(started - second) > _START_SLACK_S


_RECLAIM_SIDECAR_STALE_S = 30.0


def _remove_retry(path: str, attempts: int = 400) -> bool:
    """os.remove that rides out Windows sharing violations: a waiter reading
    the file at that instant makes the delete fail with PermissionError, and a
    silently skipped delete would leak the lock. True when the file is gone."""
    for _ in range(attempts):
        try:
            os.remove(path)
            return True
        except FileNotFoundError:
            return True
        except OSError:
            time.sleep(0.005)
    return False


def _try_reclaim(lock_path: str, judged: "str | None", force: bool) -> None:
    """Remove ``lock_path`` only if it still holds the content that was judged
    dead/stale. Serialized through an O_EXCL sidecar so no two waiters can
    both remove, and a freshly created live lock is never deleted
    (REVIEW_BUILDFX3 MED-1). ``force`` is the age-based path."""
    sidecar = lock_path + ".reclaim"
    try:
        fd = os.open(sidecar, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            if time.time() - os.path.getmtime(sidecar) > _RECLAIM_SIDECAR_STALE_S:
                os.remove(sidecar)  # a reclaimer died holding it
        except OSError:
            pass
        return
    except OSError:
        return
    os.close(fd)
    try:
        current = _read_holder(lock_path)
        if current != judged:
            return  # replaced by someone else's lock since we judged
        if not force and not _text_is_dead_holder(current):
            return
        _remove_retry(lock_path)
    finally:
        _remove_retry(sidecar)


def _read_holder(lock_path: str) -> "str | None":
    try:
        with open(lock_path, "r", encoding="ascii", errors="replace") as handle:
            return handle.read().strip() or None
    except OSError:
        return None


@contextlib.contextmanager
def build_lock(
    resource_key: str,
    *,
    wait_timeout: float = 1200.0,
    stale_after: float = 2100.0,
    poll_interval: float = 1.0,
) -> "Iterator[None]":
    """Hold an exclusive, cross-process lock on ``resource_key`` for the
    duration of the ``with`` block.

    ``resource_key`` should name the actual contended resource (e.g. the
    KilnFW build directory path) so unrelated work never queues behind it.
    Blocks up to ``wait_timeout`` seconds; raises :class:`BuildLockTimeout`
    rather than hanging forever or failing on first contention. A lock older
    than ``stale_after`` seconds is reclaimed automatically, so a holder
    that died never wedges every future caller.
    """
    lock_path = _lock_path(resource_key)
    started = time.monotonic()
    acquired = False
    # "<pid> <own process start epoch, 0 if unknown> <unique token>"
    _alive, own_start = _pid_info(os.getpid())
    my_text = f"{os.getpid()} {int(own_start) if own_start else 0} {uuid.uuid4().hex}"
    while True:
        try:
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            try:
                os.write(fd, f"{my_text}\n".encode("ascii"))
            finally:
                os.close(fd)
            acquired = True
            break
        except (FileExistsError, PermissionError):
            # PermissionError: Windows refuses CREATE_NEW while another process
            # has the file open or pending delete -- contention, not a failure.
            try:
                age = time.time() - os.path.getmtime(lock_path)
            except OSError:
                # Lock vanished between our failed open() and this stat --
                # the holder just released it. Retry immediately, no sleep,
                # no timeout charged (bounded below for a PermissionError that
                # is not contention).
                if time.monotonic() - started >= wait_timeout:
                    raise BuildLockTimeout(resource_key, wait_timeout, None)
                time.sleep(0.001)
                continue
            # The recorded PID being dead (or reused by a younger process) also
            # reclaims at once, so a killed holder does not wedge waiters.
            judged = _read_holder(lock_path)
            if age > stale_after or _text_is_dead_holder(judged):
                _try_reclaim(lock_path, judged, force=age > stale_after)
                continue
            waited = time.monotonic() - started
            if waited >= wait_timeout:
                raise BuildLockTimeout(resource_key, waited, _read_holder(lock_path))
            time.sleep(poll_interval)
    try:
        yield
    finally:
        if acquired:
            # Only remove the lock if it is still ours: a stale reclaim may
            # have replaced it with another holder's file.
            if _read_holder(lock_path) == my_text:
                _remove_retry(lock_path)
