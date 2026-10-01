"""Post-reset reachability probe and durable history for ``debug_reset``.

Why this exists (2026-10-01): ``debug_reset(peer="esp")`` returned "reset esp
(run) OK" because OpenOCD exited 0, yet the board stayed dark on UART and HTTP
for minutes and the Pico's S6b link-dead guard (trip_reason 7, mask 0x0040,
120 s of link silence) latched. Nothing verified the core was running after
``reset run``, and the outcome line only went to a rotating session log.

This module is pure and injectable (clock, sleep, and both channel readers
are parameters) so it is unit-tested with fakes and never touches a board by
itself. It only REPORTS: it never resumes or resets anything.
"""
from __future__ import annotations

import datetime
import json
import os
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

DEFAULT_WINDOW_S = 60.0
DEFAULT_INTERVAL_S = 2.0
HTTP_ATTEMPT_TIMEOUT_S = 3.0
UART_ATTEMPT_TIMEOUT_S = 2.0

HISTORY_REL_PATH = os.path.join("logs", "debug_reset", "history.jsonl")


@dataclass
class ProbeResult:
    window_s: float
    http_answered_s: Optional[float] = None
    http_host: Optional[str] = None
    boot_guard: Optional[dict] = None
    uart_answered_s: Optional[float] = None
    uart_fw: Optional[str] = None
    http_errors: "list[str]" = field(default_factory=list)

    @property
    def any_answered(self) -> bool:
        return self.http_answered_s is not None or self.uart_answered_s is not None

    @property
    def recovery_mode(self) -> bool:
        return bool(self.boot_guard and self.boot_guard.get("recovery_mode"))

    def to_json(self) -> dict:
        return {
            "window_s": self.window_s,
            "http_answered_s": self.http_answered_s,
            "http_host": self.http_host,
            "boot_guard": self.boot_guard,
            "uart_answered_s": self.uart_answered_s,
            "uart_fw": self.uart_fw,
            "recovery_mode": self.recovery_mode,
            "any_answered": self.any_answered,
        }


def probe_after_reset(
    hosts_fn: Callable[[], "list[str]"],
    get_boot_guard: Callable[..., dict],
    get_uart_fw: Optional[Callable[[], Any]] = None,
    window_s: float = DEFAULT_WINDOW_S,
    interval_s: float = DEFAULT_INTERVAL_S,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> ProbeResult:
    """Poll until the board answers on any channel or ``window_s`` elapses.

    ``hosts_fn`` is re-evaluated every poll (Wi-Fi may re-associate mid-wait).
    As soon as one channel answers, the other gets one last attempt and the
    probe returns, so a healthy board costs seconds, not the whole window.
    Readers raise on failure; every exception is a "no answer", never fatal.
    """
    res = ProbeResult(window_s=window_s)
    start = clock()
    deadline = start + window_s

    def try_http() -> None:
        if res.http_answered_s is not None:
            return
        try:
            hosts = hosts_fn()
        except Exception as exc:  # noqa: BLE001
            res.http_errors.append(f"host resolution: {exc}")
            hosts = []
        for host in hosts:
            try:
                data = get_boot_guard(host, timeout=HTTP_ATTEMPT_TIMEOUT_S)
            except Exception as exc:  # noqa: BLE001
                msg = f"{host}: {exc}"
                if msg not in res.http_errors:
                    res.http_errors.append(msg)
                continue
            res.http_answered_s = round(clock() - start, 1)
            res.http_host = host
            res.boot_guard = data if isinstance(data, dict) else {"raw": repr(data)}
            return

    def try_uart() -> None:
        if get_uart_fw is None or res.uart_answered_s is not None:
            return
        try:
            fw = get_uart_fw()
        except Exception:  # noqa: BLE001
            return
        res.uart_answered_s = round(clock() - start, 1)
        res.uart_fw = str(fw)

    while True:
        try_http()
        try_uart()
        if res.any_answered:
            # one last try at the other channel, then done
            try_http()
            try_uart()
            return res
        if clock() >= deadline:
            return res
        sleep(min(interval_s, max(0.0, deadline - clock())))


def format_report(peer: str, mode: str, res: ProbeResult) -> str:
    lines = []
    if res.http_answered_s is not None:
        bg = res.boot_guard or {}
        lines.append(
            f"HTTP answered at {res.http_host} after {res.http_answered_s}s: "
            f"boot_count={bg.get('boot_count', 'n/a')} "
            f"persisted_count={bg.get('persisted_count', 'not reported')} "
            f"recovery_mode={bg.get('recovery_mode', 'n/a')}"
        )
    else:
        lines.append(f"HTTP: no answer within {res.window_s:g}s")
    if res.uart_answered_s is not None:
        lines.append(f"UART link answered after {res.uart_answered_s}s: fw={res.uart_fw}")
    else:
        lines.append(f"UART link: no answer within {res.window_s:g}s")
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
    return out


def history_path(repo_root: str) -> str:
    return os.path.join(repo_root, HISTORY_REL_PATH)


def append_history(repo_root: str, record: dict) -> Optional[str]:
    """Append one JSON line; returns an error string on failure, else None.

    Never raises (a logging failure must not mask a reset result). Callers
    must not put credentials in ``record``.
    """
    try:
        path = history_path(repo_root)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        rec = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
        rec.update(record)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, sort_keys=True) + "\n")
        return None
    except Exception as exc:  # noqa: BLE001
        return f"{type(exc).__name__}: {exc}"
