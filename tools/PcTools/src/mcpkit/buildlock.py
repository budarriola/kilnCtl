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
token is still the holder's own. The sidecar holds a per-reclaim token;
``_release_sidecar`` removes it only on a token match and restores a foreign
one. A reclaim deletes the lock by rename-aside and content compare, never
by path.

Known residual windows (accepted; all need a prior eviction or reclaim of
the releaser's own file, or a reclaim that races a release by an
age-reclaimed but alive holder): (1) the put-back vacancy -- between a
rename-aside and the put-back of a foreign file, a third process can O_EXCL
the vacant path. The put-back then refuses to overwrite (``_put_back``): the
grabbed file is deleted only if its text equals the newcomer's, else it is
left as an aside (swept after ``stale_after``) and its owner loses the lock
file. ``_release`` additionally sleeps up to 80 ms while retrying an
unreadable aside. Put-back retries every error except ``FileExistsError``
(400 x 5 ms). (2) putting back a foreign sidecar can resurrect one whose
owner just finished; it blocks reclaims for at most 30 s until
``_break_stale_sidecar`` removes it. (3) sidecar failures (token write,
rename give-up) are silent and cost at most that same 30 s stall. (4) a
``_release`` whose aside stays unreadable puts it back and logs to stderr
(``tools/PcTools/logs/<name>-mcp.log.err``, which no tool shows); the lock
text is recorded in ``_maybe_leaked`` and the next contention in this process
frees it by exact text match. (5) a crash between a rename-aside and its
remove or put-back leaves a ``.rec.``/``.rel.``/``.dead.`` file; ``build_lock``
sweeps those older than ``stale_after`` on acquire.

Stale-lock timeout: a lock older than ``stale_after`` seconds whose holder
is NOT confirmed alive (unparseable/legacy content, unknown start time, or a
dead/reused PID) is removed so a waiter is never stuck forever. A holder
confirmed alive (PID alive AND recorded start time matches) is never
age-reclaimed, however old the lock: ``build_kilnfw`` may legitimately run up
to 7140 s, well past the default 2100 s ``stale_after``.

A waiter blocks, polling every ``poll_interval`` seconds, until it acquires
the lock or ``wait_timeout`` elapses, at which point it raises
:class:`BuildLockTimeout` with how long it waited -- never an immediate
failure, never an unbounded hang.
"""

from __future__ import annotations

import contextlib
import os
import re
import sys
import tempfile
import time
import uuid
from typing import Iterator


def _holder_is_this_process(text: "str | None") -> bool:
    parsed = _parse_holder(text)
    if parsed is None or parsed[0] != os.getpid():
        return False
    _alive, started = _pid_info(os.getpid())
    second = parsed[1]
    if parsed[2] is None or second <= 0 or started is None:
        return False
    return abs(started - second) <= _START_SLACK_S


class BuildLockTimeout(Exception):
    """Raised when a build lock could not be acquired within its timeout."""

    def __init__(self, resource: str, waited: float, holder: "str | None",
                 lock_path: "str | None" = None) -> None:
        self.resource = resource
        self.waited = waited
        self.holder = holder
        self.lock_path = lock_path
        holder_note = f" (held by: {holder})" if holder else ""
        path_note = f" [lock file: {lock_path}]" if lock_path else ""
        own_note = ""
        if _holder_is_this_process(holder):
            own_note = (
                " The holder is THIS server process. Another tool call in this "
                "server may still be building this directory; deleting the lock "
                "while it runs allows two concurrent builds. If no build is "
                "running here, the lock was leaked: restart the MCP server"
                + (f" or delete {lock_path}" if lock_path else " or delete the lock file")
                + "."
            )
        super().__init__(
            f"another build is in progress on {resource!r} -- waited {waited:.1f}s "
            f"for the lock and gave up{holder_note}{path_note}.{own_note}"
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


# Lock texts whose release could not confirm the removal (unique token each).
_maybe_leaked: "set[tuple[str, str]]" = set()


def _put_back(aside: str, path: str, attempts: int = 400) -> bool:
    """Move ``aside`` back to ``path`` without overwriting. Retries every
    error except FileExistsError (sharing violations are transient). On
    FileExistsError a newer file owns the path: the moved file is deleted only
    when its text equals that file's, else it is left as an aside (swept later)
    and logged. True when put back."""
    for _ in range(attempts):
        try:
            os.rename(aside, path)
            return True
        except FileExistsError:
            if _read_holder(aside) == _read_holder(path):
                _remove_retry(aside)
            else:
                print(f"buildlock: could not put {aside} back to {path}: a newer "
                      f"file exists; left the moved file in place",
                      file=sys.stderr, flush=True)
            return False
        except FileNotFoundError:
            return False
        except OSError:
            time.sleep(0.005)
    print(f"buildlock: could not put {aside} back to {path} after {attempts} "
          f"attempts; left the moved file in place", file=sys.stderr, flush=True)
    return False


def _sweep_asides(directory: str, stale_after: float) -> None:
    """Remove moved-aside .rec./.rel./.dead. files older than stale_after."""
    now = time.time()
    try:
        names = os.listdir(directory)
    except OSError:
        return
    for name in names:
        if not re.search(r"\.(rec|rel|dead)\.[0-9a-f]{32}$", name):
            continue
        full = os.path.join(directory, name)
        try:
            if now - os.path.getmtime(full) > stale_after:
                os.remove(full)
        except OSError:
            pass


def _holder_confirmed_alive(text: "str | None") -> bool:
    """True only for a token-bearing lock whose PID is alive and whose start
    time matches the recorded one. Such a lock is never age-reclaimed."""
    parsed = _parse_holder(text)
    if parsed is None:
        return False
    pid, second, token = parsed
    alive, started = _pid_info(pid)
    if not alive or token is None or second <= 0 or started is None:
        return False
    return abs(started - second) <= _START_SLACK_S


def _break_stale_sidecar(sidecar: str) -> None:
    """Remove an abandoned sidecar without deleting a live one: rename it to a
    unique name (atomic), verify the grabbed file is still old, else put it
    back (rename refuses to overwrite on Windows, so a newer sidecar wins)."""
    try:
        if time.time() - os.path.getmtime(sidecar) <= _RECLAIM_SIDECAR_STALE_S:
            return
        grabbed = f"{sidecar}.dead.{uuid.uuid4().hex}"
        os.rename(sidecar, grabbed)
    except OSError:
        return
    try:
        if time.time() - os.path.getmtime(grabbed) > _RECLAIM_SIDECAR_STALE_S:
            _remove_retry(grabbed)
            return
        _put_back(grabbed, sidecar)  # grabbed a live one: restore it
    except OSError:
        pass


def _release_sidecar(sidecar: str, token: str) -> None:
    """Remove the sidecar only if it still holds our token (rename-aside and
    verify); another reclaimer's live sidecar is put back, never deleted."""
    aside = f"{sidecar}.rel.{uuid.uuid4().hex}"
    for _ in range(400):
        try:
            os.rename(sidecar, aside)
            break
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.005)
    else:
        return
    if _read_holder(aside) == token:
        _remove_retry(aside)
        return
    _put_back(aside, sidecar)


def _try_reclaim(lock_path: str, judged: "str | None", force: bool,
                 stale_after: float = 2100.0) -> None:
    """Remove ``lock_path`` only if it still holds the content that was judged
    dead/stale. Serialized through an O_EXCL sidecar so no two waiters can
    both remove, and a freshly created live lock is never deleted
    (REVIEW_BUILDFX3 MED-1). ``force`` is the age-based path."""
    sidecar = lock_path + ".reclaim"
    if _holder_confirmed_alive(judged):
        return  # alive holder: never reclaim, even by age (skip the sidecar)
    token = uuid.uuid4().hex
    try:
        fd = os.open(sidecar, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        _break_stale_sidecar(sidecar)
        return
    except OSError:
        return
    try:
        os.write(fd, (token + "\n").encode("ascii"))
    except OSError:
        pass
    finally:
        os.close(fd)
    try:
        current = _read_holder(lock_path)
        if current != judged:
            return  # replaced by someone else's lock since we judged
        if _holder_confirmed_alive(current):
            return  # alive holder: never reclaim, even by age
        if not force and not _text_is_dead_holder(current):
            return
        if _read_holder(sidecar) != token:
            return  # our sidecar was broken and replaced: no longer exclusive
        _remove_judged(lock_path, judged, stale_after)
    finally:
        _release_sidecar(sidecar, token)


def _remove_judged(lock_path: str, judged: "str | None",
                   stale_after: float = 2100.0) -> None:
    """Delete the lock only if it still holds the judged content: rename it
    aside (atomic, retried through sharing violations), compare, remove on a
    match, rename back on a mismatch. A retry therefore can never delete a
    new holder's lock that took the path meanwhile (REVIEW_BUILDFX6 LOW-1).
    When ``judged`` is None (empty or unreadable), a match also requires the
    aside's mtime (kept by rename) to be older than ``stale_after``, so a new
    holder's not-yet-written lock is never deleted. Residuals: the put-back
    vacancy (module docstring, windows 1 and 5)."""
    aside = f"{lock_path}.rec.{uuid.uuid4().hex}"
    for _ in range(400):
        try:
            os.rename(lock_path, aside)
            break
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.005)
    else:
        return
    if _read_holder(aside) == judged:
        old = True
        if judged is None:
            try:
                old = time.time() - os.path.getmtime(aside) > stale_after
            except OSError:
                old = False
        if old:
            _remove_retry(aside)
            return
    _put_back(aside, lock_path)


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
    _sweep_asides(os.path.dirname(lock_path), stale_after)
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
                    raise BuildLockTimeout(resource_key, wait_timeout, None, lock_path)
                time.sleep(0.001)
                continue
            # The recorded PID being dead (or reused by a younger process) also
            # reclaims at once, so a killed holder does not wedge waiters.
            judged = _read_holder(lock_path)
            if judged is not None and (lock_path, judged) in _maybe_leaked:
                # our own earlier release leaked this exact lock (unique token)
                _maybe_leaked.discard((lock_path, judged))
                _release(lock_path, judged)
                continue
            if age > stale_after or _text_is_dead_holder(judged):
                _try_reclaim(lock_path, judged, force=age > stale_after,
                             stale_after=stale_after)
                # No `continue`: fall through so the timeout is honoured and
                # we never busy-spin when the reclaim declines.
            waited = time.monotonic() - started
            if waited >= wait_timeout:
                raise BuildLockTimeout(resource_key, waited, _read_holder(lock_path), lock_path)
            time.sleep(poll_interval)
    try:
        yield
    finally:
        if acquired:
            _release(lock_path, my_text)


def _release(lock_path: str, my_text: str) -> None:
    """Delete the lock only if it is still ours. Rename-aside first, then
    verify the token in the renamed file, so a lock a reclaimer replaced
    between a read and the delete is put back rather than deleted. Residual
    window: if a third process creates a new lock in the instant between our
    rename-aside and restore, the restore fails and the grabbed lock is lost;
    only reachable after another process already reclaimed ours."""
    aside = f"{lock_path}.rel.{uuid.uuid4().hex}"
    for _ in range(400):
        try:
            os.rename(lock_path, aside)
            break
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.005)
    else:
        print(f"buildlock: LEAKED LOCK -- could not rename {lock_path} aside after "
              f"400 attempts; later builds on this directory will wait until the "
              f"server restarts or the file is deleted", file=sys.stderr, flush=True)
        _maybe_leaked.add((lock_path, my_text))
        return
    held = _read_holder(aside)
    for _ in range(4):
        if held is not None:
            break
        time.sleep(0.02)
        held = _read_holder(aside)
    if held == my_text:
        _remove_retry(aside)
        return
    if held is None:
        restored = _put_back(aside, lock_path)
        if restored:
            _maybe_leaked.add((lock_path, my_text))
        print(f"buildlock: LEAKED LOCK? could not read {aside} after retries; "
              + (f"restored it to {lock_path}. If it was this server's own lock, "
                 f"the next contention in this process frees it"
                 if restored else
                 f"could NOT restore it to {lock_path}; the grabbed file may be lost"),
              file=sys.stderr, flush=True)
        return
    _put_back(aside, lock_path)
