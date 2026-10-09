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

The lock file's content is ``"<pid> <acquired-unix-time>"`` purely for a
human debugging a stuck lock; staleness is judged from the file's mtime, not
its content, so a lock file created by a process that got killed before it
finished the write is still reclaimable.

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
    while True:
        try:
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            try:
                os.write(fd, f"{os.getpid()} {time.time():.0f}\n".encode("ascii"))
            finally:
                os.close(fd)
            acquired = True
            break
        except FileExistsError:
            try:
                age = time.time() - os.path.getmtime(lock_path)
            except OSError:
                # Lock vanished between our failed open() and this stat --
                # the holder just released it. Retry immediately, no sleep,
                # no timeout charged.
                continue
            if age > stale_after:
                # Holder is presumed dead. Best-effort reclaim: if another
                # waiter wins the race to remove+recreate it first, our next
                # open() attempt just loops back into ordinary contention.
                try:
                    os.remove(lock_path)
                except OSError:
                    pass
                continue
            waited = time.monotonic() - started
            if waited >= wait_timeout:
                raise BuildLockTimeout(resource_key, waited, _read_holder(lock_path))
            time.sleep(poll_interval)
    try:
        yield
    finally:
        if acquired:
            try:
                os.remove(lock_path)
            except OSError:
                pass  # already gone (e.g. reclaimed as stale) -- fine
