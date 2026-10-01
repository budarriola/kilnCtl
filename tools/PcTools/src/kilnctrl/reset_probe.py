"""Post-reset reachability probe and durable history for ``debug_reset``.

Why this exists (2026-10-01): ``debug_reset(peer="esp")`` returned "reset esp
(run) OK" because OpenOCD exited 0, yet the board stayed dark on UART and HTTP
for minutes and the Pico's S6b link-dead guard (trip_reason 7, mask 0x0040,
120 s of link silence) latched. Nothing verified the core was running after
``reset run``, and the outcome line only went to a rotating session log.

This module is pure and injectable (clock, sleep, and both channel readers
are parameters) so it is unit-tested with fakes and never touches a board by
itself. It only REPORTS: it never resumes or resets anything.

Channel semantics:
  * HTTP: ``GET /api/boot_guard`` is ROUTE_TIER_ADMIN, so a live board with web
    auth on answers 401 (and the client then tries a login). ANY HTTP status
    response, or an HttpAuthError (which only arises after the board sent a
    401), proves the board is up; it just leaves the recovery state unknown.
  * UART: a fw-version query. It answering does NOT end the probe: the HTTP
    channel keeps being polled until it answers or the window expires, so
    ``recovery_mode`` is actually read when it can be.
"""
from __future__ import annotations

import datetime
import json
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from .http_auth import HttpAuthError
from .ota_http_client import OtaHttpError

DEFAULT_WINDOW_S = 60.0
DEFAULT_INTERVAL_S = 2.0
#: Per-attempt cap for one HTTP request. A request that gets a 401 also does a
#: login inside the same call with the same timeout, so this is deliberately
#: longer than a bare GET needs. Always clamped to the remaining budget.
HTTP_ATTEMPT_TIMEOUT_S = 6.0
UART_ATTEMPT_TIMEOUT_S = 2.0
MAX_ERRORS_REPORTED = 5

HISTORY_REL_PATH = os.path.join("logs", "debug_reset", "history.jsonl")

_HISTORY_LOCK = threading.Lock()


@dataclass
class ProbeResult:
    window_s: float
    elapsed_s: float = 0.0
    http_answered_s: Optional[float] = None
    http_host: Optional[str] = None
    boot_guard: Optional[dict] = None
    #: why boot_guard is None although HTTP answered (e.g. "HTTP 401")
    http_state_note: Optional[str] = None
    uart_answered_s: Optional[float] = None
    uart_fw: Optional[str] = None
    errors: "list[str]" = field(default_factory=list)

    @property
    def any_answered(self) -> bool:
        return self.http_answered_s is not None or self.uart_answered_s is not None

    @property
    def recovery_mode(self) -> bool:
        return bool(self.boot_guard and self.boot_guard.get("recovery_mode"))

    @property
    def recovery_state_known(self) -> bool:
        return self.boot_guard is not None and "recovery_mode" in self.boot_guard

    def add_error(self, msg: str) -> None:
        if msg not in self.errors and len(self.errors) < MAX_ERRORS_REPORTED:
            self.errors.append(msg)

    def to_json(self) -> dict:
        return {
            "window_s": self.window_s,
            "elapsed_s": self.elapsed_s,
            "http_answered_s": self.http_answered_s,
            "http_host": self.http_host,
            "boot_guard": self.boot_guard,
            "http_state_note": self.http_state_note,
            "uart_answered_s": self.uart_answered_s,
            "uart_fw": self.uart_fw,
            "recovery_mode": self.recovery_mode,
            "recovery_state_known": self.recovery_state_known,
            "any_answered": self.any_answered,
            "errors": list(self.errors),
        }


def _answered_note(exc: BaseException) -> Optional[str]:
    """If ``exc`` proves the board answered HTTP (without giving boot_guard),
    return a short reason, else None (transport failure = no answer)."""
    if isinstance(exc, HttpAuthError):
        # Only raised after the board itself returned a 401.
        return f"board answered 401, login did not complete: {exc}"
    if isinstance(exc, OtaHttpError) and exc.status is not None:
        return f"HTTP {exc.status}"
    return None


def probe_after_reset(
    hosts_fn: Callable[[], "list[str]"],
    get_boot_guard: Callable[..., dict],
    get_uart_fw: Optional[Callable[..., Any]] = None,
    window_s: float = DEFAULT_WINDOW_S,
    interval_s: float = DEFAULT_INTERVAL_S,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> ProbeResult:
    """Poll until HTTP answers or ``window_s`` elapses.

    * ``hosts_fn`` is called once (it may do a UART Wi-Fi query) and cached.
    * The deadline is checked before every attempt and each attempt's timeout
      is clamped to the remaining budget, so the window is overshot by at
      most what a reader does beyond its timeout argument.
    * Once a host answered HTTP (even 401) only that host is polled again.
    * UART answering never ends the probe on its own; HTTP is kept polled so
      boot_guard/recovery_mode is read if the board serves it in the window.
      The probe ends when HTTP answered (boot_guard read, or a status such as
      401 that proves the board is up) after UART has had one attempt, or
      when the window runs out.
    """
    res = ProbeResult(window_s=window_s)
    start = clock()
    deadline = start + window_s
    hosts: "Optional[list[str]]" = None

    def remaining() -> float:
        return deadline - clock()

    def try_http() -> None:
        nonlocal hosts
        if res.http_answered_s is not None:
            return
        if hosts is None:
            try:
                hosts = list(hosts_fn())
            except Exception as exc:  # noqa: BLE001
                res.add_error(f"host resolution: {exc}")
                hosts = []
        for host in hosts:
            budget = remaining()
            if budget <= 0:
                return
            try:
                data = get_boot_guard(host, timeout=min(HTTP_ATTEMPT_TIMEOUT_S, budget))
            except Exception as exc:  # noqa: BLE001
                note = _answered_note(exc)
                if note is None:
                    res.add_error(f"{host}: {exc}")
                    continue
                res.http_answered_s = round(clock() - start, 1)
                res.http_host = host
                res.http_state_note = note
                return
            res.http_answered_s = round(clock() - start, 1)
            res.http_host = host
            res.boot_guard = data if isinstance(data, dict) else {"raw": repr(data)}
            return

    def try_uart() -> None:
        if get_uart_fw is None or res.uart_answered_s is not None:
            return
        budget = remaining()
        if budget <= 0:
            return
        try:
            fw = get_uart_fw(timeout=min(UART_ATTEMPT_TIMEOUT_S, budget))
        except Exception as exc:  # noqa: BLE001
            res.add_error(f"uart: {exc}")
            return
        res.uart_answered_s = round(clock() - start, 1)
        res.uart_fw = str(fw)

    try:
        while True:
            try_http()
            try_uart()
            if res.http_answered_s is not None:
                break
            if remaining() <= 0:
                break
            sleep(min(interval_s, max(0.0, remaining())))
    finally:
        res.elapsed_s = round(clock() - start, 1)
    return res


def format_report(peer: str, mode: str, res: ProbeResult) -> str:
    lines = []
    if res.boot_guard is not None:
        bg = res.boot_guard
        lines.append(
            f"HTTP answered at {res.http_host} after {res.http_answered_s}s: "
            f"boot_count={bg.get('boot_count', 'n/a')} "
            f"persisted_count={bg.get('persisted_count', 'not reported')} "
            f"recovery_mode={bg.get('recovery_mode', 'n/a')}"
        )
    elif res.http_answered_s is not None:
        lines.append(
            f"HTTP answered at {res.http_host} after {res.http_answered_s}s but boot_guard "
            f"could not be read ({res.http_state_note}): the board is up, recovery state unknown"
        )
    else:
        lines.append(f"HTTP: no answer after {res.elapsed_s:g}s (recovery state unknown)")
    if res.uart_answered_s is not None:
        lines.append(f"UART link answered after {res.uart_answered_s}s: fw={res.uart_fw}")
    else:
        lines.append(f"UART link: no answer after {res.elapsed_s:g}s")
    if res.errors and res.http_answered_s is None:
        lines.append("probe errors: " + "; ".join(res.errors))
    out = "\n".join(lines)
    if res.recovery_mode:
        out = (
            "WARNING: the board answered but reports recovery_mode=true -- it booted "
            "the recovery image / boot_guard recovery loop, NOT the normal app.\n" + out
        )
    if not res.any_answered:
        out = (
            f"WARNING: {peer} reset ({mode}) returned OK from OpenOCD but the board did NOT "
            f"answer over HTTP or UART within {res.window_s:g}s. The core may be halted, stuck "
            "in ROM, or in recovery mode. No ANNOUNCE_REBOOT was sent, so the Pico will hard-trip "
            "S6b (link dead, trip_reason 7, mask 0x0040) after 120 s of link silence. Do not "
            "assume the board is running: check the PC with debug_read_registers(peer=\"esp\"), "
            "and resume or reset deliberately -- this tool does not do either for you.\n" + out
        )
    elif res.http_answered_s is None:
        out = (
            "NOTE: UART answered but HTTP never did within the window, so recovery_mode was "
            "never read.\n" + out
        )
    return out


def history_path(repo_root: str) -> str:
    return os.path.join(repo_root, HISTORY_REL_PATH)


def append_history(repo_root: str, record: dict) -> Optional[str]:
    """Append one JSON line; returns an error string on failure, else None.

    Never raises (a logging failure must not mask a reset result). Callers
    must not put credentials in ``record``. Serialized by a module lock so
    concurrent MCP calls cannot interleave a line.
    """
    try:
        path = history_path(repo_root)
        rec = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
        rec.update(record)
        line = json.dumps(rec, sort_keys=True, default=str) + "\n"
        with _HISTORY_LOCK:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a", encoding="utf-8") as fh:
                fh.write(line)
        return None
    except Exception as exc:  # noqa: BLE001
        return f"{type(exc).__name__}: {exc}"
