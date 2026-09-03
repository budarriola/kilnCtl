#!/usr/bin/env python3
"""run_queue.py -- turn-key, repeatable firing queue for coupling-matrix A/B
work (PID_EXPANSION_PLAN.md sec 3.2's "next run's analyst" item).

WHAT THIS AUTOMATES, per queued entry: apply a named config preset -> wait
for every zone to be RESTED (within ``rested_tol_c`` of its own cold
junction) -> refuse to start if the profile's targets exceed any zone's
``max_temp_c`` or if the zones are not rested -> POST /api/profile_exec/start
-> poll and capture telemetry to a named JSONL until the run finishes ->
capture a cooldown tail -> stop on any ``fault_guard != 0`` -> move to the
next queued entry.

WHY HTTP ONLY FOR TELEMETRY, never MCP/UART. See
project_safety_link_consumer_cannot_keep_up and this session's own
directive: polling ``profiles_get_exec_status`` over the UART/MCP path
timed out on ~40% of samples under three-zone load. Every poll in this
module goes over plain HTTP (``urllib.request``, the same stdlib-only
convention as ``dashboard_http_client.py`` / ``zones_http_client.py`` --
mocked in tests, no real socket, no live board), against
``GET /api/profile_exec`` and ``GET /api/status``, matching exactly what
``http_capture_log.py`` / ``pid_ab_compare.py`` already parse:

    {"t": <unix float>, "exec": <verbatim GET /api/profile_exec body>,
     "status": <verbatim GET /api/status body>}

Applying a preset is the one step this module does NOT reinvent: it calls
``config_presets.apply_preset(control, preset, zones_host=...)``, the
existing sanctioned path (PID gains over the UART CONTROL task,
max_temp_c/relay_mask/control_mode/etc over HTTP POST /api/zones) --
callers hand in an already-open ``ControlClient``.

SAFETY, enforced BEFORE any POST reaches the board (the firmware refuses
these too -- profile_executor_run() checks max_temp_c server-side -- but
failing fast locally means a misconfigured queue never gets as far as a
POST, and gives a readable local error instead of a 400 buried in a log):

  * ``RunQueueError`` if the profile's planned peak target_c (read from
    ``GET /api/profile_plan?id=<n>``, no UART needed) exceeds ANY zone's
    ``max_temp_c`` (read from ``GET /api/zones``) -- see
    :func:`check_targets_within_ceiling`.
  * ``RunQueueError`` if any zone is not RESTED -- ``abs(temp_c - cj_c) >
    rested_tol_c`` on any thermocouple channel -- see :func:`is_rested`.
  * The queue STOPS (raises ``RunQueueFaultError``, does not advance to the
    next entry) the moment a poll observes any zone's ``fault_guard != 0``
    -- see :func:`check_no_fault`.

Every check function above is pure (dict in, bool/raise out) specifically so
the test suite can drive them against fixture JSON -- no live board, no
mocked HTTP required for the safety-logic tests themselves.
"""
from __future__ import annotations

import dataclasses
import json
import logging
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Optional, Sequence

from kilnctrl import zones_http_client

log = logging.getLogger(__name__)

DEFAULT_HTTP_TIMEOUT_S = 5.0
DEFAULT_POLL_INTERVAL_S = 5.0
DEFAULT_RESTED_TOL_C = 1.0
DEFAULT_RESTED_TIMEOUT_S = 3600.0
DEFAULT_COOLDOWN_S = 600.0

#: profile_exec state strings that mean "the run is over" (dashboard_http.c /
#: profile_executor.h's PROFILE_EXEC_* names, lower-cased in the JSON body).
_TERMINAL_STATES = frozenset({"done", "faulted", "idle"})


class RunQueueError(RuntimeError):
    """A safety refusal or an HTTP failure -- never silently skipped."""


class RunQueueFaultError(RunQueueError):
    """A running zone reported ``fault_guard != 0``. The queue must stop,
    not advance to the next entry -- a fault on entry N says nothing about
    whether entry N+1's preset/profile pairing is safe."""


# --------------------------------------------------------------------------
# Pure safety checks -- no HTTP, fixture-testable.
# --------------------------------------------------------------------------

def is_rested(status_body: dict, tol_c: float = DEFAULT_RESTED_TOL_C) -> bool:
    """True iff every valid thermocouple channel in a ``GET /api/status``
    body is within ``tol_c`` of its own cold junction. A channel reporting
    ``valid: false`` or a null ``temp_c``/``cj_c`` (MAX31856.c leaves both
    NaN on a faulted/absent channel -- dashboard_http.c line ~461) is
    ignored rather than treated as "not rested": a dead channel should not
    block every other zone's queue forever, and the ceiling/fault checks
    below catch a genuinely unsafe start on their own terms."""
    channels = status_body.get("channels")
    if not channels:
        # No channel data at all is NOT "rested" -- an empty/absent list
        # means the board hasn't told us anything, which is not proof of
        # anything either way, and refusing is the safe default.
        return False
    saw_any = False
    for ch in channels:
        if not ch.get("valid", False):
            continue
        temp_c = ch.get("temp_c")
        cj_c = ch.get("cj_c")
        if temp_c is None or cj_c is None:
            continue
        saw_any = True
        if abs(float(temp_c) - float(cj_c)) > tol_c:
            return False
    return saw_any


def check_no_fault(exec_body: dict) -> None:
    """Raise :class:`RunQueueFaultError` if any zone in a
    ``GET /api/profile_exec`` body has ``fault_guard != 0``."""
    for z in exec_body.get("zones", []):
        fg = int(z.get("fault_guard", 0))
        if fg != 0:
            raise RunQueueFaultError(
                f"zone {z.get('zone')}: fault_guard={fg} -- stopping the queue, "
                "not advancing to the next entry")


def profile_peak_target_c(plan_body: dict) -> float:
    """Highest ``c`` among a ``GET /api/profile_plan`` body's ``points`` --
    the profile's peak commanded target, independent of which zone(s) it
    actually drives (zone_mask is not reported by this endpoint; comparing
    the peak against every zone's own ceiling, as
    :func:`check_targets_within_ceiling` does, is conservative in the
    direction that matters -- it can refuse a start that would have been
    fine on an uninvolved zone, never the reverse)."""
    points = plan_body.get("points") or []
    if not points:
        raise RunQueueError(
            f"profile_plan body has no points to check a ceiling against: {plan_body!r}")
    return max(float(p["c"]) for p in points)


def check_targets_within_ceiling(plan_body: dict, zones_body: dict) -> None:
    """Raise :class:`RunQueueError` if the profile's peak target exceeds ANY
    configured zone's ``max_temp_c``. Mirrors the firmware's own
    ``profile_executor_run()`` refusal (zones_http.c's
    ``max_temp_c == 0.0`` is itself "no ceiling configured", which this
    treats as a refusal too -- a zone with no ceiling configured is not a
    zone this harness should ever fire into)."""
    peak = profile_peak_target_c(plan_body)
    zones = zones_body.get("zones", [])
    if not zones:
        raise RunQueueError("zones body has no zones -- cannot check a ceiling")
    for z in zones:
        max_c = float(z.get("max_temp_c", 0.0))
        if max_c <= 0.0 or peak > max_c:
            raise RunQueueError(
                f"refusing to start: profile peak target {peak:.2f}C exceeds zone "
                f"{z.get('index')}'s max_temp_c={max_c:.2f}C")


# --------------------------------------------------------------------------
# HTTP transport -- stdlib urllib, same convention as dashboard_http_client.py
# / zones_http_client.py. Kept as free functions (not methods) so tests can
# monkeypatch urllib.request.urlopen exactly the way test_dashboard_http_
# client.py already does, no new mocking pattern introduced.
# --------------------------------------------------------------------------

def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _get_json(host: str, path: str, timeout: float) -> dict:
    req = urllib.request.Request(_url(host, path), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.URLError as exc:
        raise RunQueueError(f"GET {path} failed: {exc}") from exc
    try:
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001
        raise RunQueueError(f"GET {path} response was not valid JSON: {body!r}") from exc


def _post_form(host: str, path: str, fields: dict, timeout: float) -> str:
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host, path), data=data, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
        raise RunQueueError(f"POST {path} failed: HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RunQueueError(f"POST {path} failed: {exc}") from exc


def get_status(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    return _get_json(host, "/api/status", timeout)


def get_exec(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    return _get_json(host, "/api/profile_exec", timeout)


def get_zones(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    return _get_json(host, "/api/zones", timeout)


def get_profile_plan(host: str, profile_id: int, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    return _get_json(host, f"/api/profile_plan?id={profile_id}", timeout)


def start_profile(host: str, profile_id: int, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    body = _post_form(host, "/api/profile_exec/start", {"id": str(profile_id)}, timeout)
    try:
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001
        raise RunQueueError(f"POST /api/profile_exec/start response was not JSON: {body!r}") from exc


def stop_profile(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> None:
    _post_form(host, "/api/profile_exec/stop", {}, timeout)


# --------------------------------------------------------------------------
# Queue entries and the runner.
# --------------------------------------------------------------------------

@dataclasses.dataclass
class QueueEntry:
    """One queued firing: apply ``preset_name``, run ``profile_id``, capture
    to ``log_path``. ``label`` is only for log lines."""
    preset_name: str
    profile_id: int
    log_path: str
    label: str = ""


@dataclasses.dataclass
class RunQueueConfig:
    host: str
    poll_interval_s: float = DEFAULT_POLL_INTERVAL_S
    rested_tol_c: float = DEFAULT_RESTED_TOL_C
    rested_timeout_s: float = DEFAULT_RESTED_TIMEOUT_S
    cooldown_s: float = DEFAULT_COOLDOWN_S
    http_timeout_s: float = DEFAULT_HTTP_TIMEOUT_S
    #: injectable for tests / non-realtime replay; defaults to wall time.
    sleep: Callable[[float], None] = time.sleep
    now: Callable[[], float] = time.time


def load_preset_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def wait_until_rested(cfg: RunQueueConfig, deadline_s: Optional[float] = None) -> None:
    """Block (polling ``GET /api/status``) until :func:`is_rested` is True,
    or raise :class:`RunQueueError` after ``rested_timeout_s`` (or
    ``deadline_s`` if given, for tests)."""
    timeout_s = cfg.rested_timeout_s if deadline_s is None else deadline_s
    start = cfg.now()
    while True:
        status = get_status(cfg.host, cfg.http_timeout_s)
        if is_rested(status, cfg.rested_tol_c):
            return
        if cfg.now() - start > timeout_s:
            raise RunQueueError(
                f"zones did not settle within {cfg.rested_tol_c}C of cold junction "
                f"within {timeout_s:.0f}s")
        cfg.sleep(cfg.poll_interval_s)


def _capture_line(t: float, exec_body: dict, status_body: dict) -> str:
    return json.dumps({"t": t, "exec": exec_body, "status": status_body})


def _poll_capture_until(cfg: RunQueueConfig, fh, stop_predicate: Callable[[dict, dict], bool]) -> None:
    """Poll exec+status once per ``poll_interval_s``, append one capture
    line each time, and check :func:`check_no_fault` on every sample (a
    fault mid-run must stop the queue even though the run has not reached a
    terminal state yet). Returns when ``stop_predicate(exec, status)`` is
    True."""
    while True:
        exec_body = get_exec(cfg.host, cfg.http_timeout_s)
        status_body = get_status(cfg.host, cfg.http_timeout_s)
        check_no_fault(exec_body)
        fh.write(_capture_line(cfg.now(), exec_body, status_body) + "\n")
        fh.flush()
        if stop_predicate(exec_body, status_body):
            return
        cfg.sleep(cfg.poll_interval_s)


def _run_is_terminal(exec_body: dict, _status_body: dict) -> bool:
    return str(exec_body.get("state", "")).lower() in _TERMINAL_STATES


#: Zone fields that have NO HTTP write path -- only the UART CONTROL task's
#: SET_ZONE_MODEL can write them (config_presets.py's own required-field
#: list; zones_http_client.py's _PRESET_ZONE_OVERRIDE_FIELDS has no k_dc/
#: tau_s/dead_time_s entry, on purpose -- see that module's docstring).
_MODEL_ONLY_FIELDS = ("k_dc", "tau_s", "dead_time_s")


def _apply_preset_http_only(control, preset: dict, zones_host: "Optional[str]" = None,
                             timeout: float = zones_http_client.ZONES_HTTP_TIMEOUT_S,
                             verify: bool = True) -> "zones_http_client.ZonesApplyResult":
    """Apply a preset with NO UART link at all -- the path used when
    ``control`` is ``None``, which is the common case: the kilnctrl MCP
    server holds the serial port, so a harness run generally cannot take it.

    Every field a coupling-only preset carries (``pid_kp/ki/kd``,
    ``coupling_coeff``, ``max_temp_c``, ``relay_mask``, ...) IS reachable
    through ``POST /api/zones`` alone -- zones_http_handlers.c's whole-page-
    submit handler parses and applies ``pid_kp/ki/kd`` exactly the same way
    the UART CONTROL task's ``SET_ZONE_PID`` does (see that file's
    ``parse_zone_fields()``, ~line 621). So for such a preset,
    ``config_presets.apply_preset()``'s unconditional UART PID write is
    REDUNDANT, not required -- this function does the equivalent work
    through ``zones_http_client.apply_zone_preset()`` (GET-merge-POST-verify,
    same as that module's own docstring), no serial port touched.

    The one field that genuinely has no HTTP path is the thermal model
    (``k_dc``/``tau_s``/``dead_time_s``). Rather than silently drop it (the
    worst of the three options the coordinator named), this REFUSES up
    front, naming exactly which zone(s) need it, if any zone in the preset
    carries one -- the caller must pass ``--serial-port`` (and free the port
    from the MCP server first) to apply that preset."""
    if control is not None:
        raise RunQueueError(
            "_apply_preset_http_only called with a non-None control -- internal error, "
            "the UART path (config_presets.apply_preset) should have been used instead")
    if not zones_host:
        raise RunQueueError(
            "no serial port AND no zones_host -- there is no path at all to apply this preset")

    model_zones = [z["index"] for z in preset["zones"] if any(k in z for k in _MODEL_ONLY_FIELDS)]
    if model_zones:
        raise RunQueueError(
            f"preset {preset.get('name')!r} carries a thermal model (k_dc/tau_s/dead_time_s) for "
            f"zone(s) {model_zones} -- that field has no HTTP write path, only the UART CONTROL "
            f"task has a setter for it. Pass --serial-port to apply this preset (note: the "
            f"kilnctrl MCP server usually owns the port, so this generally means stopping it "
            f"first).")

    result = zones_http_client.apply_zone_preset(zones_host, preset, timeout=timeout, verify=verify)
    if not result.ok:
        raise RunQueueError(
            f"POST /api/zones for preset {preset.get('name')!r} was ACKed but a read-back "
            f"disagreed: {result.mismatches}")
    return result


def run_entry(entry: QueueEntry, cfg: RunQueueConfig, control=None,
              apply_preset_fn=None) -> None:
    """Run one queue entry start to finish: apply preset, wait rested,
    safety-check, start, capture to ``entry.log_path`` through the run, then
    capture a cooldown tail into ``<log_path>.cooldown.jsonl``.

    ``apply_preset_fn``, when omitted, is chosen from ``control``: when a
    ``ControlClient`` is given, ``config_presets.apply_preset`` (UART PID/
    model write + HTTP zones write); when ``control`` is ``None`` (the
    common case -- the kilnctrl MCP server holds the port),
    :func:`_apply_preset_http_only` (HTTP only, refuses up front if the
    preset needs a field only the UART CONTROL task can write). Passing
    ``apply_preset_fn`` explicitly overrides this selection entirely --
    tests use that to inject a fake with no ``config_presets``/UART
    machinery in scope at all."""
    if apply_preset_fn is None:
        from kilnctrl import config_presets
        apply_preset_fn = config_presets.apply_preset if control is not None else _apply_preset_http_only
        preset = config_presets.load_preset_data(entry.preset_name)
    else:
        # Tests hand apply_preset_fn a fake and entry.preset_name a raw
        # preset dict directly; real callers always pass a preset NAME
        # (config_presets.load_preset_data resolves it) through the
        # apply_preset_fn is None branch above, so this path never sees a
        # bare ".json" path in production -- only a fixture dict.
        preset = entry.preset_name

    log.info("[%s] applying preset %s", entry.label or entry.profile_id, entry.preset_name)
    apply_preset_fn(control, preset, zones_host=cfg.host)

    log.info("[%s] waiting for zones to rest (tol=%.1fC)", entry.label, cfg.rested_tol_c)
    wait_until_rested(cfg)

    log.info("[%s] checking profile %d against zone ceilings", entry.label, entry.profile_id)
    plan = get_profile_plan(cfg.host, entry.profile_id, cfg.http_timeout_s)
    zones = get_zones(cfg.host, cfg.http_timeout_s)
    check_targets_within_ceiling(plan, zones)

    # Re-confirm rested immediately before the POST -- wait_until_rested may
    # have returned a while ago if the ceiling check above was slow.
    status = get_status(cfg.host, cfg.http_timeout_s)
    if not is_rested(status, cfg.rested_tol_c):
        raise RunQueueError("zones drifted out of rested tolerance between the rested "
                             "wait and the start -- refusing to start")

    log.info("[%s] starting profile %d, capturing to %s", entry.label, entry.profile_id, entry.log_path)
    result = start_profile(cfg.host, entry.profile_id, cfg.http_timeout_s)
    if not result.get("ok", False):
        raise RunQueueError(f"POST /api/profile_exec/start refused: {result!r}")

    with open(entry.log_path, "w", encoding="utf-8") as fh:
        _poll_capture_until(cfg, fh, _run_is_terminal)

    cooldown_path = entry.log_path + ".cooldown.jsonl"
    log.info("[%s] capturing cooldown to %s", entry.label, cooldown_path)
    cooldown_start = cfg.now()
    with open(cooldown_path, "w", encoding="utf-8") as fh:
        _poll_capture_until(
            cfg, fh,
            lambda exec_body, status_body: (
                is_rested(status_body, cfg.rested_tol_c)
                or cfg.now() - cooldown_start > cfg.cooldown_s))


def run_queue(entries: Sequence[QueueEntry], cfg: RunQueueConfig, control=None,
              apply_preset_fn=None) -> None:
    """Run every entry in order. Stops (re-raises) on the first
    :class:`RunQueueError` -- in particular a :class:`RunQueueFaultError`
    from mid-run -- rather than continuing to the next entry, since a fault
    or a refusal on entry N says nothing about whether N+1 is safe."""
    for entry in entries:
        run_entry(entry, cfg, control=control, apply_preset_fn=apply_preset_fn)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _parse_run_arg(raw: str) -> QueueEntry:
    parts = raw.split(":", 3)
    if len(parts) < 3:
        raise ValueError(
            f"--run must be PRESET:PROFILE_ID:LOG_PATH[:LABEL], got {raw!r}")
    preset, profile_id, log_path = parts[0], parts[1], parts[2]
    label = parts[3] if len(parts) > 3 else preset
    return QueueEntry(preset_name=preset, profile_id=int(profile_id), log_path=log_path, label=label)


def _numbered_log_path(log_path: str, k: int) -> str:
    """Insert ``_runK`` (1-based) before ``log_path``'s extension, e.g.
    ``noise_floor_p7.jsonl`` -> ``noise_floor_p7_run1.jsonl``. Used by
    :func:`expand_repeat` so N repeats of one entry can NEVER land in the
    same file -- the exact failure mode (a poller left running across a
    cooldown appended a second firing into one JSONL) that
    ``log_analysis.MultiRunError`` had to be added to catch after the fact.
    Guaranteeing distinct files up front is strictly better than detecting
    the collision downstream."""
    import os as _os
    root, ext = _os.path.splitext(log_path)
    return f"{root}_run{k}{ext}"


def expand_repeat(entry: QueueEntry, n: int) -> list:
    """Turn one :class:`QueueEntry` into ``n`` entries, same preset and
    profile, each with its own numbered log path (see
    :func:`_numbered_log_path`) and label -- this is the campaign-queue
    primitive PID_EXPANSION_PLAN.md SS3.3's noise-floor item needs: N
    firings of ONE fixed configuration, each independently waiting for a
    genuinely rested start (``run_entry`` already does that per entry,
    unconditionally -- expanding to N entries gets the "rest between every
    repeat" requirement for free, no new waiting logic needed) and captured
    to a file nothing else can land in."""
    if n < 1:
        raise ValueError(f"expand_repeat: n must be >= 1, got {n}")
    out = []
    for k in range(1, n + 1):
        out.append(QueueEntry(
            preset_name=entry.preset_name,
            profile_id=entry.profile_id,
            log_path=_numbered_log_path(entry.log_path, k),
            label=f"{entry.label or entry.preset_name} run{k}/{n}",
        ))
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Run a queue of preset+profile firings, capturing HTTP telemetry to JSONL.")
    parser.add_argument("--host", required=True, help="board host/IP, e.g. 192.168.1.156")
    parser.add_argument(
        "--run", action="append", required=True, dest="runs",
        metavar="PRESET:PROFILE_ID:LOG_PATH[:LABEL]",
        help="one queued entry; repeat for multiple runs, run in order")
    parser.add_argument(
        "--repeat", type=int, default=1, metavar="N",
        help="repeat the single --run entry N times, same preset+profile, each waiting for "
             "its own genuinely rested start (run_entry always waits rested first -- this "
             "just runs it N times). Requires exactly one --run. log_path gets _run1.._runN "
             "inserted before its extension so no two runs can land in the same file -- the "
             "noise-floor campaign's turn-key form (PID_EXPANSION_PLAN.md SS3.3).")
    parser.add_argument("--poll-interval-s", type=float, default=DEFAULT_POLL_INTERVAL_S)
    parser.add_argument("--rested-tol-c", type=float, default=DEFAULT_RESTED_TOL_C)
    parser.add_argument("--rested-timeout-s", type=float, default=DEFAULT_RESTED_TIMEOUT_S)
    parser.add_argument("--cooldown-s", type=float, default=DEFAULT_COOLDOWN_S)
    parser.add_argument("--serial-port", default=None,
                         help="serial port for the UART CONTROL link. Omit it (the common "
                              "case -- the kilnctrl MCP server usually holds the port) and "
                              "every preset field is written over HTTP alone (POST /api/zones "
                              "writes pid_kp/ki/kd too, not just coupling/max_temp_c/etc). Only "
                              "needed if a queued preset carries a thermal model "
                              "(k_dc/tau_s/dead_time_s) -- that field has no HTTP write path; "
                              "if one does and this is omitted, the run is refused up front, "
                              "naming the zone, not silently skipped.")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    entries = [_parse_run_arg(r) for r in args.runs]
    if args.repeat != 1:
        if len(entries) != 1:
            parser.error("--repeat requires exactly one --run entry")
        entries = expand_repeat(entries[0], args.repeat)

    cfg = RunQueueConfig(
        host=args.host, poll_interval_s=args.poll_interval_s, rested_tol_c=args.rested_tol_c,
        rested_timeout_s=args.rested_timeout_s, cooldown_s=args.cooldown_s)

    control = None
    if args.serial_port:
        from kilnctrl.serial_link import UartLink
        from kilnctrl.control import ControlClient
        link = UartLink(args.serial_port)
        control = ControlClient(link)

    try:
        run_queue(entries, cfg, control=control)
    except RunQueueError as exc:
        log.error("queue stopped: %s", exc)
        return 1
    finally:
        if control is not None:
            control.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
