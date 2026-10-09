"""Cross-process bench-board lock.

docs/audits/profile_executor_panic_2026-09-24.md (HP-02/HP-05): two
`bench_test_run(suite="heat")` calls from two different sessions drove the
same board at the same time, each starting a profile in bench slot 7 while
the other's firing was still running. Nothing in the harness stopped this --
`BenchTestRunner` had no notion of "another run already owns the board".

This module is a plain lock FILE (`logs/bench_test/.board_lock` by default),
not an in-process lock, because the two runs that collided went through the
same MCP server process but a bare script invocation of the runner (see
`runner.run_suite()`'s docstring: "NOT what bench_test_run() calls") or a
second `kilnctrl` server process must be refused too -- anything that only
lived in one process's memory would not have caught either of those.

Suite-mutation table
---------------------
Whether a suite needs the lock is decided here, explicitly, per suite --
not inferred from a case's judge function at call time. See the per-suite
comments below for the evidence (grepped from each suite's cases_*.py for
the calls that actually change board state: `srv._profiles.start/stop/
pause/resume`, `srv._autotune.start/abort/accept`, `srv.flash_firmware`,
`srv.safety_clear_trip`, and the config-writing `_post_json` calls in
cases_web_rw.py).
"""
from __future__ import annotations

import dataclasses
import json
import os
import socket
import time
import uuid
from typing import Optional

from . import report as report_mod

LOCK_FILENAME = ".board_lock"

#: Suites confirmed, by reading every cases_*.py module in this package, to
#: only ever READ board state -- never start/stop/pause a profile run,
#: start/abort/accept autotune, flash a processor, clear a safety trip,
#: inject a touch, or POST a config-writing web route. They never CREATE the
#: lock file and may run concurrently with each other (two smoke runs racing
#: is harmless), but `acquire()` still REFUSES them while a live MUTATING
#: run holds the lock: `BenchTestRunner.preflight()` only refuses a profile
#: that is running, or an autotune that is active, at the instant it
#: samples, so it lets a read-only run through between a heat run's cases or
#: during an ota/flash/web/safety/lcd run that never starts a profile --
#: exactly while the state these cases assert about is changing underneath
#: them. The reverse -- a mutating run starting while a read-only run is
#: already in flight -- is blocked too, via a per-run reader marker under
#: READERS_DIRNAME (see `acquire()` for the publish-then-check ordering that
#: keeps the two sides from both slipping through).
#:
#:   smoke  -- cases_smoke.py: reads only (thermo, link stats, ...)
#:   static -- cases_smoke.py's ST-* rows: static/doc-derived checks, no board call
#:   stack  -- cases_smoke.py's SK-* rows: reads GET_STACK_MARGIN; SK-01/02 write
#:             a baseline record under the run's OWN logs/bench_test/<run_id>/
#:             directory, never board state (SK-02 depends on HP-01, which is
#:             not in this suite, so it reports NOT_RUN here)
#:
#: `lcd` is deliberately NOT here -- see MUTATING_SUITES_DOCUMENTED below.
READ_ONLY_SUITES = frozenset({"smoke", "static", "stack"})

#: Directory (a sibling of the lock file, under the same logs root) holding
#: one marker file per LIVE read-only run: `<pid>_<uniq>.json`, same shape as
#: `LockInfo`. A mutating `acquire()` scans this directory before creating
#: the main lock file and refuses if any marker names a live process --
#: closing the second documented gap (a mutating run starting while a
#: read-only run is already in flight). Read-only runs still never touch
#: `LOCK_FILENAME` itself.
READERS_DIRNAME = ".board_readers"

#: Sibling of the lock file, created O_EXCL for the few syscalls a stale
#: reclaim takes (see `_reclaim_stale`). Never the lock itself: nothing but
#: `_reclaim_stale`/`_clear_orphaned_reclaim_mutex` ever reads it.
RECLAIM_MUTEX_SUFFIX = ".reclaiming"

#: An UNPARSEABLE reclaim mutex (a reclaimer died between its O_EXCL create
#: and its write) is treated as orphaned only once it is at least this old;
#: a live reclaimer holds it for milliseconds.
_ORPHAN_MUTEX_MIN_AGE_S = 10.0

#: Suites confirmed to mutate persistent board state, kept here as
#: documentation (see `suite_is_mutating` below for the actual, fail-closed
#: decision rule -- anything NOT in READ_ONLY_SUITES is treated as mutating,
#: including a suite name added later that this table has not been updated
#: for yet).
#:
#:   heat     -- cases_heat.py: srv._profiles.save/start/pause/resume/stop/
#:               delete/ack_last_run -- exactly the HP-02/HP-05 collision class
#:   autotune -- cases_autotune.py: srv._autotune.start/abort/accept, and
#:               accept() writes zone PID gains via srv._control
#:   ota      -- cases_ota.py: can flash both processors, roll back a slot,
#:               reset the safety link (docs/BENCH_TEST_SYSTEM_PLAN.md 3.4)
#:   flash    -- cases_fl.py: FL-10/FL-11 do a JTAG flash round trip when
#:               allow_flash=True; the suite is refused to lock even when a
#:               given run happens not to pass allow_flash, since the lock
#:               decision is per-SUITE, made before cases are filtered
#:   safety   -- cases_safety.py: srv.safety_clear_trip()
#:   web      -- shares suite membership (registry._WEB_IDS) between the
#:               read-only WEB-*-01 render rows (cases_web.py) and the
#:               config-writing rows (cases_web_rw.py: POST /api/unit_pref,
#:               /api/watchdog_cfg, /api/ramp_assist, /api/auth/security,
#:               ...). Classified MUTATING as a whole suite, fail-closed,
#:               since a single `bench_test_run(suite="web")` call can reach
#:               both kinds of case in the same run.
#:   lcd      -- cases_lcd.py: the page cases wake and navigate the panel
#:               with real touch_inject presses (`_wake_and_home()`); LCD-04
#:               calls srv.safety_clear_trip() on ANY latched trip whose
#:               mask matches the formula -- not a trip it induced, so a
#:               concurrent run's genuine guard trip would be cleared under
#:               it; LCD-19 turns `lcd_enabled` on via set_policy(), types
#:               wrong and right PINs on the keypad, then restores the policy.
#:   nightly  -- aggregates heat/ota/autotune/safety/web among others
#:   full     -- same as nightly
MUTATING_SUITES_DOCUMENTED = frozenset({
    "heat", "autotune", "ota", "flash", "safety", "web", "lcd", "nightly", "full",
})


def suite_is_mutating(suite: str) -> bool:
    """Fail closed: a suite is mutating unless it is explicitly listed in
    READ_ONLY_SUITES above. A new suite name nobody has classified yet, or
    a typo, is treated as mutating rather than silently let through."""
    return suite not in READ_ONLY_SUITES


def write_refusal(ctx: dict) -> Optional[str]:
    """Fail-closed write gate shared by every judge that writes to the board.
    Returns a refusal reason, or None when ctx["suite"] is present and names a
    mutating suite. An ABSENT suite is a refusal, never a pass-through."""
    suite = ctx.get("suite")
    if not isinstance(suite, str) or not suite:
        return "ctx['suite'] is absent; refusing to write (fail closed)"
    if not suite_is_mutating(suite):
        return f"suite {suite!r} is not a mutating suite"
    return None


class BoardLockHeld(RuntimeError):
    """Raised when another live process already holds the board lock, or
    when the lock file exists but cannot be read/parsed (refuse rather than
    guess)."""


@dataclasses.dataclass
class LockInfo:
    pid: int
    hostname: str
    suite: str
    started_at: str  # UTC, "%Y-%m-%dT%H:%M:%SZ"
    tag: Optional[str] = None

    def to_json(self) -> str:
        return json.dumps(dataclasses.asdict(self), indent=2, sort_keys=True)

    @classmethod
    def from_json(cls, text: str) -> "LockInfo":
        data = json.loads(text)
        return cls(
            pid=int(data["pid"]),
            hostname=str(data.get("hostname", "")),
            suite=str(data.get("suite", "")),
            started_at=str(data.get("started_at", "")),
            tag=data.get("tag"),
        )

    def describe(self) -> str:
        tag_part = f" tag={self.tag}" if self.tag else ""
        return (
            f"pid={self.pid} host={self.hostname} suite={self.suite!r} "
            f"started_at={self.started_at}{tag_part}"
        )


def _pid_alive(pid: int) -> bool:
    """True if `pid` names a live process on THIS host, on either Windows or
    POSIX. Fails closed: only a positive "no such process" or "that process
    has exited" answer counts as dead, so a live holder's lock is never
    reclaimed -- at worst a genuinely stale lock needs a human to remove it.

    Windows: OpenProcess failing with ERROR_INVALID_PARAMETER (87) is the
    only "no such pid" answer; any other failure (ERROR_ACCESS_DENIED for an
    elevated, protected or other-user process) means the process exists. A
    successful OpenProcess alone is NOT proof of life -- an exited process
    stays openable for as long as anything (a parent shell, a debugger)
    still holds a handle to it -- so the exit code must read STILL_ACTIVE.
    Known, accepted false positive on both platforms: a dead holder's pid
    that the OS has since reused for an unrelated live process reads alive,
    which refuses (fail closed) rather than reclaiming."""
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes  # noqa: PLC0415
        from ctypes import wintypes  # noqa: PLC0415

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        ERROR_INVALID_PARAMETER = 87
        STILL_ACTIVE = 259
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return ctypes.get_last_error() != ERROR_INVALID_PARAMETER
        try:
            code = wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return True  # could not tell -- fail closed
            # A process that really exited with code 259 reads alive: fail
            # closed, same as pid reuse.
            return code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True  # PermissionError (someone else's process) or anything else: fail closed
    else:
        return True


def _holder_alive(existing: "LockInfo") -> bool:
    """A lock written on another host (e.g. through a synced logs/ directory)
    names a pid this host cannot check, so it counts as live -- refuse
    rather than reclaim."""
    if existing.hostname and existing.hostname != socket.gethostname():
        return True
    return _pid_alive(existing.pid)


def _lock_path(logs_root: Optional[str]) -> str:
    root = logs_root or report_mod.default_logs_root()
    return os.path.join(root, LOCK_FILENAME)


def _readers_dir(logs_root: Optional[str]) -> str:
    root = logs_root or report_mod.default_logs_root()
    return os.path.join(root, READERS_DIRNAME)


def _new_info(suite: str, tag: Optional[str]) -> "LockInfo":
    return LockInfo(
        pid=os.getpid(),
        hostname=socket.gethostname(),
        suite=suite,
        started_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        tag=tag,
    )


def _read_info(path: str) -> "LockInfo":
    with open(path, "r", encoding="utf-8") as fh:
        return LockInfo.from_json(fh.read())


def _same_holder(a: "LockInfo", b: "LockInfo") -> bool:
    return (a.pid, a.hostname, a.suite, a.started_at, a.tag) == (
        b.pid, b.hostname, b.suite, b.started_at, b.tag)


def _remove_retrying(path: str, timeout_s: float = 5.0) -> None:
    """`os.remove`, tolerating a brief Windows sharing violation.

    CPython's `open()` on Windows does not pass FILE_SHARE_DELETE, so
    deleting a lock/marker file fails with PermissionError for as long as
    another process has it open to READ it (every acquirer reads these
    files). Those reads last microseconds, so retry briefly rather than
    either leaking a live-pid file (which would block every later run until
    this process exits) or failing a release in a `finally`. The sleep is a
    retry backoff only, never an ordering mechanism."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            os.remove(path)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def _live_readers(logs_root: Optional[str]) -> list:
    """Scan the readers directory with the SAME fail-closed rules as the
    main lock: a marker naming a live process (on this host, or any process
    on another host) is returned; a marker naming a confirmed-dead pid on
    this host is opportunistically deleted and skipped, so a crashed
    read-only run's leftover marker never blocks a mutating run forever; a
    marker that exists but cannot be read or parsed raises `BoardLockHeld`
    rather than being guessed about. Markers are published by an atomic
    rename (see `acquire()`), so a `.json` name never holds a half-written
    body -- only `.json` names are considered; a `.tmp` left by a reader
    that died mid-publish is ignored."""
    d = _readers_dir(logs_root)
    live = []
    try:
        names = os.listdir(d)
    except FileNotFoundError:
        return live
    for name in sorted(names):
        if not name.endswith(".json"):
            continue
        p = os.path.join(d, name)
        try:
            info = _read_info(p)
        except FileNotFoundError:
            continue  # released between listdir and open
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
            raise BoardLockHeld(
                f"reader marker {p} exists but could not be read/parsed ({exc}); refusing "
                "rather than guessing whether a read-only run is in flight -- inspect it by hand"
            ) from exc
        if _holder_alive(info):
            live.append(info)
        else:
            try:
                os.remove(p)
            except OSError:
                pass  # someone else is reading/removing it; it is dead either way
    return live


def _clear_orphaned_reclaim_mutex(mutex: str) -> None:
    """Remove a reclaim mutex whose holder died inside `_reclaim_stale`'s
    few-syscall critical section, so such an orphan cannot block every later
    reclaim forever. Only removed when its recorded pid is confirmed dead on
    this host, or (unparseable: died between create and write) when it is
    older than `_ORPHAN_MUTEX_MIN_AGE_S`. A mutex held by a live or
    unverifiable process is left alone -- the caller just retries/refuses.

    Accepted residual race (documented, not closed): two reclaimers that
    both observe the SAME orphan could both remove it, the second removal
    landing on the first one's fresh mutex. That needs a process to die
    inside a sub-millisecond window AND two new reclaimers to race within
    microseconds of each other afterward."""
    try:
        info = _read_info(mutex)
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError, KeyError, ValueError):
        try:
            age = time.time() - os.path.getmtime(mutex)
        except OSError:
            return
        if age < _ORPHAN_MUTEX_MIN_AGE_S:
            return
    else:
        if _holder_alive(info):
            return
    try:
        os.remove(mutex)
    except OSError:
        pass


def _reclaim_stale(path: str, expected: "LockInfo") -> Optional["LockInfo"]:
    """Remove the lock file at `path` iff it is STILL the dead-holder lock
    `expected` the caller just read. Returns the removed holder's
    `LockInfo` on success, `None` when this call removed nothing (the
    caller loops and re-reads).

    The original implementation reclaimed with a plain `os.remove(path)`
    after an earlier read decided the holder was dead: a second reclaimer
    whose `os.remove` ran after a first reclaimer had already removed the
    stale file and created its own live lock deleted that live lock by name
    alone, so both believed they held the board. A first fix (rename the
    file aside with `os.replace`, re-read it, rename it back if live) still
    moved a LIVE lock out of `path` for a moment: a third acquirer could
    O_EXCL-create in that gap, and the put-back `os.replace` then silently
    overwrote the third acquirer's lock -- two holders again; a reclaimer
    killed between the two renames also lost a live lock outright.

    This version never moves or deletes anything it has not just re-verified
    under exclusion. Reclaimers serialize on an O_EXCL-created sibling mutex
    (`path + RECLAIM_MUTEX_SUFFIX`); while it is held, the file at `path`
    can only be changed by (a) another reclaimer -- excluded, (b) an O_EXCL
    create -- impossible while the file exists, or (c) its owner's
    `release()` -- the owner is confirmed dead. So once the re-read still
    shows `expected` with a dead holder, removing `path` can only remove
    that stale file. `path` is never empty while a live lock exists, and a
    reclaimer killed mid-way leaves at worst the mutex, which
    `_clear_orphaned_reclaim_mutex` removes once its holder reads dead."""
    mutex = path + RECLAIM_MUTEX_SUFFIX
    try:
        fd = os.open(mutex, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except (FileExistsError, PermissionError):
        # Another reclaimer is mid-reclaim (or its orphan, or on Windows a
        # delete-pending mutex, is in the way). Never wait it out here.
        _clear_orphaned_reclaim_mutex(mutex)
        return None
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(_new_info("reclaim", None).to_json())
        try:
            current = _read_info(path)
        except (OSError, json.JSONDecodeError, KeyError, ValueError):
            return None  # gone, or changed into something the caller must re-read
        if not _same_holder(current, expected) or _holder_alive(current):
            return None
        try:
            os.remove(path)
        except (FileNotFoundError, PermissionError):
            return None  # vanished, or a reader has it open this instant: retry
        return current
    finally:
        _remove_retrying(mutex)


class BoardLock:
    """A held lock. Release exactly once, in a `finally` -- releasing twice
    is a harmless no-op. Also usable as a context manager."""

    def __init__(self, path: str, info: LockInfo, reclaimed_from: Optional[LockInfo] = None):
        self._path = path
        self.info = info
        self.reclaimed_from = reclaimed_from
        self._released = False

    def release(self) -> None:
        if self._released:
            return
        self._released = True
        try:
            with open(self._path, "r", encoding="utf-8") as fh:
                current = LockInfo.from_json(fh.read())
        except (FileNotFoundError, json.JSONDecodeError, KeyError, ValueError, OSError):
            return
        # Only remove the file if it is still the one we wrote -- if a
        # stale-reclaim by some OTHER process replaced it after we somehow
        # lost track (should not happen under normal use, but never delete
        # a lock we do not recognize as our own).
        if current.pid == self.info.pid and current.started_at == self.info.started_at:
            _remove_retrying(self._path)

    def __enter__(self) -> "BoardLock":
        return self

    def __exit__(self, *exc_info) -> bool:
        self.release()
        return False


def acquire(suite: str, tag: Optional[str] = None, logs_root: Optional[str] = None) -> Optional[BoardLock]:
    """Acquire the board lock for `suite`.

    READ_ONLY_SUITES never create or remove the main lock file: raises
    `BoardLockHeld` if a live mutating run holds it (or it is unreadable),
    otherwise registers a marker under `READERS_DIRNAME` (so a mutating
    `acquire()` started while this read-only run is in flight refuses too)
    and returns a `BoardLock` handle over that marker -- release it exactly
    like a mutating lock. A stale main-lock file is left untouched for the
    next mutating run to reclaim.

    Any other suite raises `BoardLockHeld`, naming the current holder, if a
    live process already holds the lock, or if a live read-only run's
    reader marker is registered. A lock file whose holder is confirmed dead
    is reclaimed under exclusion (see `_reclaim_stale`) with a logged notice
    (the returned `BoardLock.reclaimed_from` carries the prior holder's
    `LockInfo`) -- reclaiming is NEVER attempted while the holder pid is
    alive or unverifiable, and at most one of several racing reclaimers can
    win a given stale file."""
    path = _lock_path(logs_root)

    # Ordering between the two sides (a reader registering while a mutating
    # run starts): each side PUBLISHES its own claim first and only then
    # CHECKS for the other's -- a reader writes its marker, then reads the
    # main lock; a mutating run O_EXCL-creates the main lock, then scans the
    # markers -- and backs its own claim out if it finds the other. Whichever
    # side publishes second is therefore guaranteed to see the first, so the
    # two can never both proceed; the worst case is that both refuse (both
    # published before either checked), which is fail-closed and retryable.

    if not suite_is_mutating(suite):
        readers_dir = _readers_dir(logs_root)
        os.makedirs(readers_dir, exist_ok=True)
        info = _new_info(suite, tag)
        base = f"{info.pid}_{uuid.uuid4().hex}"
        tmp_path = os.path.join(readers_dir, base + ".tmp")
        reader_path = os.path.join(readers_dir, base + ".json")
        with open(tmp_path, "w", encoding="utf-8") as fh:
            fh.write(info.to_json())
        os.replace(tmp_path, reader_path)  # publish atomically: never a half-written .json
        marker = BoardLock(reader_path, info)
        try:
            _refuse_reader_if_mutating_holder(path, suite)
        except BaseException:
            marker.release()
            raise
        return marker

    lock_dir = os.path.dirname(path) or "."
    os.makedirs(lock_dir, exist_ok=True)

    info = _new_info(suite, tag)

    reclaimed_from: Optional[LockInfo] = None
    # A live holder never loops here; the extra attempts cover a stale
    # reclaim (reclaim, then create) plus one lost reclaim race or orphaned
    # reclaim-mutex cleanup.
    for _attempt in range(3):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                existing = _read_info(path)
            except FileNotFoundError:
                continue  # released/reclaimed between our create and read
            except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
                raise BoardLockHeld(
                    f"board lock file {path} exists but could not be read/parsed ({exc}); "
                    "refusing rather than guessing which run owns it -- inspect it by hand "
                    "before removing it"
                ) from exc
            if _holder_alive(existing):
                raise BoardLockHeld(
                    f"board lock held by {existing.describe()}; refusing to start suite "
                    f"{suite!r} concurrently -- wait for that run to finish"
                )
            # Stale: the holder pid is dead. Reclaim under exclusion (see
            # `_reclaim_stale`); `None` means we removed nothing -- loop and
            # re-read whatever is there now.
            claimed = _reclaim_stale(path, existing)
            if claimed is not None:
                reclaimed_from = claimed
            continue
        except PermissionError as exc:
            # Windows: the lock file is delete-pending (its holder is
            # releasing it this instant). Refuse rather than spin.
            raise BoardLockHeld(
                f"board lock file {path} is being removed by another process ({exc}); "
                f"refusing suite {suite!r} for now -- retry shortly"
            ) from exc
        else:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(info.to_json())
            lock = BoardLock(path, info, reclaimed_from=reclaimed_from)
            try:
                live_readers = _live_readers(logs_root)
            except BaseException:
                lock.release()
                raise
            if live_readers:
                lock.release()
                raise BoardLockHeld(
                    f"a read-only run is in progress ({live_readers[0].describe()}); refusing "
                    f"to start mutating suite {suite!r} concurrently -- wait for it to finish"
                )
            return lock
    raise BoardLockHeld(
        f"could not acquire board lock at {path}: a stale-lock reclaim did not settle "
        f"(another reclaimer may hold {path + RECLAIM_MUTEX_SUFFIX}) -- retry shortly"
    )


def _refuse_reader_if_mutating_holder(path: str, suite: str) -> None:
    """Read-only side of `acquire()`: raise `BoardLockHeld` if a live
    mutating run holds the main lock, or if the lock file exists but cannot
    be read/parsed (a mutating run may be mid-write). A stale main-lock file
    is left untouched for the next mutating run to reclaim."""
    try:
        existing = _read_info(path)
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
        raise BoardLockHeld(
            f"board lock file {path} exists but could not be read/parsed ({exc}); "
            f"refusing read-only suite {suite!r} rather than guessing whether a "
            "mutating run owns the board -- inspect it by hand"
        ) from exc
    if _holder_alive(existing):
        raise BoardLockHeld(
            f"board lock held by {existing.describe()}; refusing read-only suite "
            f"{suite!r} while a mutating run owns the board -- wait for it to finish"
        )
