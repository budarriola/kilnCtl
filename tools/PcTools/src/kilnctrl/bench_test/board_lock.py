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
from typing import Optional

from . import report as report_mod

LOCK_FILENAME = ".board_lock"

#: Suites confirmed, by reading every cases_*.py module in this package, to
#: only ever READ board state -- never start/stop/pause a profile run,
#: start/abort/accept autotune, flash a processor, clear a safety trip, or
#: POST a config-writing web route. These may run concurrently with each
#: other (two smoke runs racing is harmless), but are still refused while a
#: MUTATING suite below holds the lock, since a mutating run can change the
#: very state a read-only case is asserting about.
#:
#:   smoke  -- cases_smoke.py: reads only (thermo, link stats, ...)
#:   static -- cases_smoke.py's ST-* rows: static/doc-derived checks, no board call
#:   stack  -- cases_smoke.py's SK-* rows: reads GET_STACK_MARGIN; SK-01/02 write
#:             a baseline record under the run's OWN logs/bench_test/<run_id>/
#:             directory, never board state
#:   lcd    -- cases_lcd.py: taps/reads the LCD over the UI-test channel.
#:             LCD-19's PIN-keypad flow DOES call srv.safety_clear_trip() once
#:             it has independently verified the trip mask matches the formula
#:             for the exact trip it itself induced -- board-side this clears
#:             a latch a moment after inducing it, not a lasting change a
#:             concurrent read-only case would observe as divergent, so it
#:             stays in this bucket rather than promoting the whole suite.
READ_ONLY_SUITES = frozenset({"smoke", "static", "stack", "lcd"})

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
#:   nightly  -- aggregates heat/ota/autotune/safety/web among others
#:   full     -- same as nightly
MUTATING_SUITES_DOCUMENTED = frozenset({
    "heat", "autotune", "ota", "flash", "safety", "web", "nightly", "full",
})


def suite_is_mutating(suite: str) -> bool:
    """Fail closed: a suite is mutating unless it is explicitly listed in
    READ_ONLY_SUITES above. A new suite name nobody has classified yet, or
    a typo, is treated as mutating rather than silently let through."""
    return suite not in READ_ONLY_SUITES


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
    """True if `pid` names a live process, on either Windows or POSIX. A
    dead pid (or one we cannot even ask about in a way that means "alive")
    returns False -- an unreadable answer is never treated as "alive
    forever", since that would mean a genuinely stale lock could never be
    reclaimed."""
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes  # noqa: PLC0415

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if handle:
            ctypes.windll.kernel32.CloseHandle(handle)
            return True
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, just owned by someone else
    else:
        return True


def _lock_path(logs_root: Optional[str]) -> str:
    root = logs_root or report_mod.default_logs_root()
    return os.path.join(root, LOCK_FILENAME)


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
            try:
                os.remove(self._path)
            except FileNotFoundError:
                pass

    def __enter__(self) -> "BoardLock":
        return self

    def __exit__(self, *exc_info) -> bool:
        self.release()
        return False


def acquire(suite: str, tag: Optional[str] = None, logs_root: Optional[str] = None) -> Optional[BoardLock]:
    """Acquire the board lock for `suite`. Returns `None` immediately (no
    file touched) for a suite in READ_ONLY_SUITES -- there is nothing to
    hold. Raises `BoardLockHeld`, naming the current holder, if a live
    process already holds the lock. A lock file whose pid is confirmed dead
    is reclaimed with a logged notice (the returned `BoardLock.reclaimed_from`
    carries the prior holder's `LockInfo` so the caller can log it) --
    reclaiming is NEVER attempted while the holder pid is alive."""
    if not suite_is_mutating(suite):
        return None

    path = _lock_path(logs_root)
    lock_dir = os.path.dirname(path) or "."
    os.makedirs(lock_dir, exist_ok=True)

    info = LockInfo(
        pid=os.getpid(),
        hostname=socket.gethostname(),
        suite=suite,
        started_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        tag=tag,
    )

    reclaimed_from: Optional[LockInfo] = None
    # One retry after a stale-reclaim; a live holder never loops here.
    for _attempt in range(2):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    existing = LockInfo.from_json(fh.read())
            except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
                raise BoardLockHeld(
                    f"board lock file {path} exists but could not be read/parsed ({exc}); "
                    "refusing rather than guessing which run owns it -- inspect it by hand "
                    "before removing it"
                ) from exc
            if _pid_alive(existing.pid):
                raise BoardLockHeld(
                    f"board lock held by {existing.describe()}; refusing to start suite "
                    f"{suite!r} concurrently -- wait for that run to finish"
                )
            # Stale: the holder pid is dead. Reclaim and retry once.
            reclaimed_from = existing
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
            continue
        else:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(info.to_json())
            return BoardLock(path, info, reclaimed_from=reclaimed_from)
    raise BoardLockHeld(f"could not acquire board lock at {path} after a stale-reclaim retry")
