"""OT-B01 + SP-04, OT-E01 (self-push refused)/E02/E03/E12, and (Wave 4) OT-P01..05 -- ESP
OTA-over-Wi-Fi update/rollback/corrupt-image handling, the dual
safety-processor reset trip, and Pico OTA relayed over the isolated link
(plan doc section 3.4/3.9), using `ota_http_client` directly (never the
MCP-tool text wrapper, per the task's own instruction) so results stay
structured dicts a pure judge function can compare.

Every case here is gated on the executor being idle before touching OTA or
issuing a dual reset -- CLAUDE.md's "never flash/OTA during a firing" -- and
every case is thin: fetch/act, then delegate to judgments.py.

OT-P* (Wave 4) was blocked until the two Pico OTA defects closed (erase-time
watchdog reset, `e59b0328`; CRC-variant mismatch, `ota_image_crc.c`) -- both
are fixed and confirmed flashed as of `73c1da94` (ROADMAP.md row L,
2026-09-21), so this wave implements the case bodies; it has still only ever
been unit-tested against mocked clients, never against a real Pico relay
(constraint: an attempt on defective firmware watchdog-resets the safety
processor, and no hardware access is in scope for the change that added
these cases). The Pico has no partition/fw_build HTTP surface the way the
ESP does (TODO.md 9.6) -- identity comes from the ESP's cached
GET_FW_VERSION reply (`safety_get_fw_version()`'s text) and boot-reason
comes from `safety_get_diag()`'s text, parsed by the `parse_fw_version_*`/
`parse_diag_boot_reason` helpers in judgments.py.
"""
from __future__ import annotations

import os
from typing import Any, Optional

from .. import web_auth_setup_http_client as _wac
from . import judgments as J
from . import windows
from .registry import CaseResult, Verdict, get_case


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: keeps this module importable with no board/MCP process
    return srv


def _ota_client(ctx: dict):
    client = ctx.get("ota_http_client")
    if client is None:
        from .. import ota_http_client as client  # local import: same reason as _srv
    return client


def _partition_client(ctx: dict):
    client = ctx.get("partition_http_client")
    if client is None:
        from .. import partition_http_client as client
    return client


def _zones_client(ctx: dict):
    client = ctx.get("zones_http_client")
    if client is None:
        from .. import zones_http_client as client
    return client


def _dashboard_client(ctx: dict):
    client = ctx.get("dashboard_http_client")
    if client is None:
        from .. import dashboard_http_client as client
    return client


def _interlock_ok(ctx: dict, host: str) -> "tuple[bool, str]":
    """Plan doc section 6 rule 1: every OTA action must first confirm
    ``GET /api/ota/interlock`` reports ``ok:true`` -- checked immediately
    before the call, never assumed from a run-start snapshot. Any error
    reading it refuses rather than assuming ok, same discipline as
    ``_is_idle`` above. Injectable via ``ctx["_interlock_fn"]`` so this can
    be unit-tested without a real board (this wave is unit/mock only, per
    the standing unacknowledged crash report on the bench board)."""
    ota = _ota_client(ctx)
    interlock_fn = ctx.get("_interlock_fn") or (lambda: ota.get_interlock(host))
    try:
        body = interlock_fn()
    except Exception as exc:
        return False, f"could not read /api/ota/interlock: {type(exc).__name__}: {exc}"
    ok = bool(body.get("ok"))
    return ok, ("" if ok else str(body.get("reason", "")))


def _relays_energized(ctx: dict, host) -> Optional[bool]:
    """``dashboard_status_t.safety_relay_energized`` -- same accessor
    ``cases_heat._read_energized`` uses. Landed-review gap fix for OT-B01:
    the dual reset previously only gated on the executor being idle, which
    does not by itself guarantee no relay is latched on via some other
    path (a stuck relay, a manual bench test, etc.); dual-resetting both
    processors while a relay is still energized is exactly the situation
    CLAUDE.md's dual-reflash procedure exists to avoid surprises around.
    Returns None (not a crash) on any read failure -- callers must treat
    that as "cannot confirm", never as "de-energized"."""
    dashboard = _dashboard_client(ctx)
    try:
        status = dashboard.get_status(host)
    except Exception:
        return None
    return status.get("safety_relay_energized")


def _is_idle(ctx: dict) -> "tuple[bool, str]":
    """CLAUDE.md: never flash/OTA during a firing -- gate on executor
    status idle. Any error reading status refuses rather than assumes
    idle."""
    srv = _srv(ctx)
    try:
        st = srv._profiles.get_exec_status()
    except Exception as exc:
        return False, f"could not read executor status: {type(exc).__name__}: {exc}"
    state = getattr(st, "state_name", None)
    if state != "idle":
        return False, f"executor state is {state!r}, not idle -- refusing to OTA/reset during a firing"
    return True, ""


def _running_partition(ctx: dict, host: str) -> Optional[str]:
    try:
        return _partition_client(ctx).get_partitions(host).get("running")
    except Exception:
        return None


def _fw_build(ctx: dict, host: str) -> Optional[str]:
    try:
        return _dashboard_client(ctx).get_status(host).get("fw_build")
    except Exception:
        return None


def _boot_guard_recovery_mode(ctx: dict, host: str) -> Optional[bool]:
    try:
        return _ota_client(ctx).get_boot_guard_status(host).get("recovery_mode")
    except Exception:
        return None


def _pid_gains(zones_json: dict) -> "Optional[dict]":
    """Extracts the per-zone pid_kp/pid_ki/pid_kd triple keyed by zone
    index, for an exact before/after comparison -- see
    judgments.judge_ota_rollback's ZONES_CFG_VERSION hazard docstring."""
    zones = zones_json.get("zones") if isinstance(zones_json, dict) else None
    if not isinstance(zones, list):
        return None
    out = {}
    for i, z in enumerate(zones):
        if not isinstance(z, dict):
            return None
        out[i] = {"pid_kp": z.get("pid_kp"), "pid_ki": z.get("pid_ki"), "pid_kd": z.get("pid_kd")}
    return out


def _default_clear_trip_fn(ctx: dict):
    """`safety_clear_trip()` is fire-and-forget over UART
    (`_srv._send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())`,
    mcp_server_safety.py) -- no reply to check here, only the status poll
    that follows tells us whether it actually cleared."""
    srv = _srv(ctx)
    from .. import devices_safety as devices
    from ..protocol import UART_TASK_ID_SAFETY
    srv._send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())


def _default_readiness_trip_ok_fn(ctx: dict) -> Optional[bool]:
    """Confirms the trip item on /api/readiness (via capability_preflight's
    same board report) no longer blocks, using the readiness_blocked list
    capability_preflight already exposes."""
    from .. import capability_preflight
    host = ctx.get("host") or capability_preflight.PREFLIGHT_AP_DEFAULT_HOST
    try:
        report = capability_preflight.run_preflight({}, host)
    except Exception:
        return None
    board = report.board
    blocked = getattr(board, "readiness_blocked", None)
    if not blocked:
        return True
    return not any("trip" in str(key).lower() or "trip" in str(label).lower() for key, label, _detail in blocked)


def _is_transport_reset(exc: BaseException, _depth: int = 0) -> bool:
    """True only for a genuine mid-upload socket close: ConnectionResetError,
    BrokenPipeError or http.client.RemoteDisconnected, found on the exception
    itself, a URLError ``.reason``, or the ``__cause__``/``__context__``
    chain. An OtaHttpError carrying an HTTP status is never one. A mistyped
    image path ("no such file"), an empty image, a TypeError or a refused
    connection are NOT transport resets."""
    import http.client
    import urllib.error
    if _depth > 6 or exc is None:
        return False
    if getattr(exc, "status", None) is not None:
        return False
    if isinstance(exc, (ConnectionResetError, BrokenPipeError, http.client.RemoteDisconnected)):
        return True
    if isinstance(exc, urllib.error.URLError) and isinstance(exc.reason, BaseException):
        if _is_transport_reset(exc.reason, _depth + 1):
            return True
    return (_is_transport_reset(exc.__cause__, _depth + 1) if exc.__cause__ is not None else False) or \
           (_is_transport_reset(exc.__context__, _depth + 1) if exc.__context__ is not None else False)


#: How long a post-refusal readback keeps retrying before it is reported
#: unreadable. The firmware answers a refused POST /api/ota/esp with the status
#: line first and then drains the unread request body inside the handler
#: (`ota_http_refusal_drain()`, capped at 30 s). esp_http_server is
#: single-threaded, so for those seconds every other request (GET /api/status,
#: GET /api/partitions, ...) times out even though the board is fine: 30 s cap
#: plus slack.
OTA_REFUSAL_DRAIN_SETTLE_S = 40.0

#: Seconds between attempts inside the settle window.
OTA_REFUSAL_DRAIN_POLL_S = 1.0

#: Per-request timeout for the real HTTP readbacks inside the settle window, so
#: one slow read (the board is still draining) does not stall the poll loop.
OTA_REFUSAL_READ_TIMEOUT_S = 3.0


def _read_kw(ctx: dict, client_key: str) -> dict:
    """``timeout`` kwarg for the real client modules; injected fakes take none."""
    return {"timeout": OTA_REFUSAL_READ_TIMEOUT_S} if ctx.get(client_key) is None else {}


def _settled_read(ctx: dict, fn, window_s: Optional[float] = None) -> "tuple[Any, Optional[float]]":
    """Call ``fn`` until it returns a non-None value without raising, or
    ``window_s`` (default OTA_REFUSAL_DRAIN_SETTLE_S) elapses, polling about
    every OTA_REFUSAL_DRAIN_POLL_S. Returns ``(value, settled_at)``: ``value``
    is None only if nothing came back inside the window; ``settled_at`` is the
    ``ctx["_now"]`` clock reading at the START of the retried attempt that
    finally succeeded (so a caller can include the settle wait in elapsed-time
    maths without counting the read's own latency) and None when the first
    attempt already succeeded. Hooks: ``ctx["_now"]``, ``ctx["_sleep_fn"]``.
    The clock is only read after a first failure. A hard attempt cap of
    ceil(window/poll)+2 bounds the loop even if an injected clock never
    advances."""
    import math
    import time as _time
    now = ctx.get("_now") or _time.monotonic
    sleep = ctx.get("_sleep_fn") or _time.sleep
    window = OTA_REFUSAL_DRAIN_SETTLE_S if window_s is None else window_s
    max_attempts = math.ceil(window / OTA_REFUSAL_DRAIN_POLL_S) + 2
    deadline = None
    attempts = 0
    while True:
        started = None if deadline is None else now()
        try:
            value = fn()
        except Exception:
            value = None
        if value is not None:
            return value, started
        attempts += 1
        if deadline is None:
            deadline = now() + window
        elif now() >= deadline:
            return None, None
        if attempts >= max_attempts:
            return None, None
        sleep(OTA_REFUSAL_DRAIN_POLL_S)


def _settled_interlock_ok(ctx: dict, host) -> Optional[bool]:
    """``_interlock_ok`` with the drain-settle retry. None when the interlock
    could not be read at all inside the window; False only for a real
    "not ok" answer from the board."""
    def _read():
        ok, reason = _interlock_ok(ctx, host)
        if not ok and reason.startswith("could not read /api/ota/interlock"):
            raise RuntimeError(reason)
        return ok
    value, _ = _settled_read(ctx, _read)
    return value


def _settled_running(ctx: dict, host) -> Optional[str]:
    value, _ = _settled_read(ctx, lambda: _partition_client(ctx).get_partitions(host, **_read_kw(ctx, "partition_http_client")).get("running"))
    return value


def _settled_fw_build(ctx: dict, host) -> Optional[str]:
    value, _ = _settled_read(ctx, lambda: _dashboard_client(ctx).get_status(host, **_read_kw(ctx, "dashboard_http_client")).get("fw_build"))
    return value


def _root_error_name(exc: BaseException, _depth: int = 0) -> str:
    """Name of the socket error under a wrapped transport failure
    (ConnectionResetError rather than OtaHttpError), else the exception's own
    type name."""
    import http.client
    import urllib.error
    if _depth <= 6 and exc is not None:
        if isinstance(exc, (ConnectionResetError, BrokenPipeError, http.client.RemoteDisconnected)):
            return type(exc).__name__
        reason = exc.reason if isinstance(exc, urllib.error.URLError) and isinstance(exc.reason, BaseException) else None
        for inner in (reason, exc.__cause__, exc.__context__):
            if inner is not None and _is_transport_reset(inner):
                return _root_error_name(inner, _depth + 1)
    return type(exc).__name__


def _push_refusal_outcome(exc: BaseException, *, reset_is_refusal: bool = False) -> "tuple[bool, Optional[str]]":
    """Classify an exception from a refusal-expected push into
    ``(refused, push_error)``. An HTTP response from the board (``.status``
    set) is a refusal. A transport reset is a refusal only where the caller
    says so (OT-E09, the deliberately unauthenticated push, where the firmware
    closes a large unauthenticated refusal); anywhere else it, and every local
    error (missing file, TypeError, ...), is ``push_error="error:<ExcName>"``
    and is never treated as a refusal."""
    status = getattr(exc, "status", None)
    if any(k.__name__ == "OtaSessionProbeError" for k in type(exc).__mro__):
        # The pre-push session probe was refused (bad/missing/non-admin
        # credentials): nothing was uploaded, so this is not an image refusal.
        return False, f"error:session_probe_{status}"
    if status is not None:
        return True, None
    if reset_is_refusal and _is_transport_reset(exc):
        return True, None
    return False, f"error:{_root_error_name(exc)}"


def _esp_uptime_s(ctx: dict, host) -> Optional[float]:
    """ESP ``uptime_s`` from GET /api/status (None when the field is absent)."""
    return _dashboard_client(ctx).get_status(host, **_read_kw(ctx, "dashboard_http_client")).get("uptime_s")


def _pico_boot_id_known(srv) -> Optional[int]:
    """Pico boot_id from the ESP's cached FW_VERSION, or None while unknown.
    Before the first FW_VERSION frame arrives the ESP reports boot_id=0 with
    protocol_version=0; 0 is also a legal real (random) boot_id, so the
    protocol_version, never the value, decides whether it is known."""
    fw = srv._safety.get_fw_version()
    if not getattr(fw, "protocol_version", 0):
        return None
    return fw.boot_id


def _case_otb01(ctx: dict) -> CaseResult:
    """OT-B01: sw_reset_esp(confirm=True) resets both processors close
    together. S6a (SAFETY_TRIP_MAIN_FAULT) was expected to latch while the
    link handshake is still coming up, but a ``/api/sw_reset`` dual reset
    has been OBSERVED NOT TO latch it (2026-09-30, and 2026-10-01 run
    20261001T072647Z_ota: reset confirmed, trip_reason 0, mask 0); the S6a
    sightings came from JTAG/flash dual resets. This case therefore records
    which outcome happened and accepts either, once the reset itself is
    proven:

    (a) reset confirmed + trip_reason 6 / mask 0x0020 -> clear it (the only
        trip this case ever clears), confirm cleared and readiness: PASS.
    (b) reset confirmed + no trip -> PASS, noting "no S6a latched on
        sw_reset" so the bench log settles the empirical question.
    (c) reset confirmed + any OTHER trip -> FAIL, never cleared.
    (d) reset not confirmed -> INCONCLUSIVE, never judged on stale data.

    Why the confirmation exists: ``POST /api/sw_reset`` returns BEFORE the
    ESP reboots (sw_reset_http.c sleeps 500 ms, announces, then reboots),
    and ``safety_get_status()/get_diag()`` read the ESP's CACHED Pico
    telemetry, so a poll straight after the POST sees the PRE-reset state
    (link up, no trip). A snapshot of the ESP ``uptime_s`` and the Pico
    ``boot_id`` is taken first; the ESP must be seen restarted (uptime
    below baseline) before the link is polled, and the read only counts as
    evidence if the Pico boot_id changed or a trip latched.

    Stashes the observed data into ctx["_otb01"] for SP-04's observer.

    Every board interaction is behind an injectable ctx seam
    (``_esp_uptime_fn``/``_pico_boot_id_fn`` (None = unknown)/``_get_safety_status_fn``/
    ``_get_safety_diag_fn``/``_clear_trip_fn``/``_readiness_trip_ok_fn``) so
    this can be unit tested with fakes rather than a live link."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    srv = _srv(ctx)
    ota = _ota_client(ctx)

    energized = _relays_energized(ctx, host)
    if energized is None:
        return CaseResult(Verdict.SKIP, reason="could not confirm relays de-energized before dual reset")
    if energized:
        return CaseResult(Verdict.SKIP, reason="refusing dual reset: a relay is still energized")

    sw_reset_fn = ctx.get("_sw_reset_fn", lambda: ota.sw_reset(host))
    get_status_fn = ctx.get("_get_safety_status_fn") or (lambda: srv._safety.get_status())
    get_diag_fn = ctx.get("_get_safety_diag_fn") or (lambda: srv._safety.get_diag())
    esp_uptime_fn = ctx.get("_esp_uptime_fn") or (lambda: _esp_uptime_s(ctx, host))
    boot_id_fn = ctx.get("_pico_boot_id_fn") or (lambda: _pico_boot_id_known(srv))
    clear_trip_fn = ctx.get("_clear_trip_fn", lambda: _default_clear_trip_fn(ctx))
    readiness_trip_ok_fn = ctx.get("_readiness_trip_ok_fn", lambda: _default_readiness_trip_ok_fn(ctx))

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    errors = {"count": 0, "last": None}

    def _note_error(where: str, exc: BaseException) -> None:
        errors["count"] += 1
        errors["last"] = f"{where}: {type(exc).__name__}: {exc}"

    link_up = False
    trip_reason = None
    trip_mask = None
    clear_ok = None
    readiness_trip_ok = None
    esp_restarted = False
    boot_id_after = None
    uptime_before = None
    boot_id_before = None

    def _stash(outcome: str, **extra) -> dict:
        data = {
            "outcome": outcome, "link_up": link_up, "trip_reason": trip_reason, "trip_mask": trip_mask,
            "clear_ok": clear_ok, "readiness_trip_ok": readiness_trip_ok,
            "esp_uptime_before": uptime_before, "esp_restart_confirmed": esp_restarted,
            "pico_boot_id_before": boot_id_before, "pico_boot_id_after": boot_id_after,
            "poll_error_count": errors["count"], "last_poll_error": errors["last"],
        }
        data.update(extra)
        ctx["_otb01"] = data
        return data

    # Baseline BEFORE the reset. Without all of it, a later read can never be
    # shown to post-date the reset, so do not reset at all. An UNKNOWN Pico
    # boot_id (None: no FW_VERSION frame yet, protocol_version 0) is not a
    # baseline -- and 0 is a legal real boot_id, never a sentinel.
    try:
        uptime_before = esp_uptime_fn()
    except Exception as exc:
        _note_error("baseline esp uptime", exc)
    try:
        boot_id_before = boot_id_fn()
    except Exception as exc:
        _note_error("baseline pico boot_id", exc)
    diag_before_ok = False
    trip_before = None
    try:
        d0 = get_diag_fn()
        if getattr(d0, "ever_received", False):
            diag_before_ok = True
            trip_before = getattr(d0, "trip_reason", None)
    except Exception as exc:
        _note_error("baseline pico diag", exc)
    if uptime_before is None or boot_id_before is None or not diag_before_ok:
        data = _stash("inconclusive_no_baseline")
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"could not snapshot a pre-reset baseline (esp uptime_s={uptime_before!r}, "
                    f"pico boot_id={boot_id_before!r} (None = not yet known), "
                    f"diag received={diag_before_ok}; last error: {errors['last']}) -- "
                    f"no reset was issued"),
            observed=data,
        )
    if trip_before:
        data = _stash("inconclusive_trip_prelatched", trip_before=trip_before)
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"a safety trip (reason {trip_before}) is already latched before the reset, so a "
                    f"post-reset trip could not be attributed to it -- clear/diagnose it first; "
                    f"no reset was issued"),
            observed=data,
        )

    windows.fire_window(ctx, "otb01_before_reset", once=True)
    try:
        sw_reset_fn()
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"sw_reset_esp raised {type(exc).__name__}: {exc}")
    reset_at = now()

    # Phase 1: wait for the ESP to actually restart (uptime below baseline).
    last_uptime = None
    restart_deadline = reset_at + 60.0
    while now() < restart_deadline:
        try:
            last_uptime = esp_uptime_fn()
            if last_uptime is not None and last_uptime < uptime_before:
                esp_restarted = True
                break
        except Exception as exc:
            _note_error("esp uptime poll", exc)  # expected while the ESP is down
        sleep(1.0)
    if not esp_restarted:
        data = _stash("inconclusive_esp_not_restarted", esp_uptime_last=last_uptime)
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"ESP restart not confirmed within 60 s of sw_reset (uptime_s baseline "
                    f"{uptime_before!r}, last read {last_uptime!r}; poll errors {errors['count']}, "
                    f"last: {errors['last']}) -- not judging possibly pre-reset data"),
            observed=data,
        )

    # Phase 2: link back up, then read diag + Pico boot_id. After the ESP's
    # restart both caches start EMPTY (diag ever_received False; boot_id
    # unknown, reported as 0 with protocol_version 0), so keep polling until
    # they are populated; placeholder zeros are never evidence.
    deadline = now() + 30.0
    diag_received = False
    while now() < deadline:
        try:
            status = get_status_fn()
            if getattr(status, "link_up", False):
                link_up = True
                diag = get_diag_fn()
                if getattr(diag, "ever_received", False):
                    diag_received = True
                    trip_reason = getattr(diag, "trip_reason", None)
                    trip_mask = getattr(diag, "trip_mask", None)
                boot_id_after = boot_id_fn()
                if diag_received and (trip_reason or (boot_id_after is not None and boot_id_after != boot_id_before)):
                    break  # Valid evidence: a trip latched, or the Pico rebooted.
        except Exception as exc:
            _note_error("post-reset link poll", exc)
        sleep(1.0)

    if link_up and not diag_received:
        data = _stash("inconclusive_diag_not_received")
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"link up after ESP restart but no DIAG frame ever arrived (trip_reason is a "
                    f"placeholder) -- poll errors {errors['count']}, last: {errors['last']}"),
            observed=data,
        )
    if link_up and not trip_reason and boot_id_after is None:
        data = _stash("inconclusive_boot_id_unknown")
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"link up after ESP restart but the Pico boot_id never became known "
                    f"(no FW_VERSION frame) -- cannot show the Pico was reset "
                    f"(poll errors {errors['count']}, last: {errors['last']})"),
            observed=data,
        )
    evidence_valid = link_up and (boot_id_after != boot_id_before or bool(trip_reason))
    if link_up and not evidence_valid:
        data = _stash("inconclusive_pico_not_reset")
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(f"ESP restarted but Pico boot_id unchanged ({boot_id_before!r}) and no trip -- "
                    f"cannot show the Pico was reset (last error: {errors['last']})"),
            observed=data,
        )

    # A trip may latch a little after link-up (S6a is debounced 200 ms):
    # before recording "no trip", look a few seconds longer. Only a diag
    # that has actually been received counts as a look.
    if evidence_valid and not trip_reason:
        settle_deadline = now() + 5.0
        settle_received = 0
        while now() < settle_deadline:
            sleep(1.0)
            try:
                d2 = get_diag_fn()
                if not getattr(d2, "ever_received", False):
                    continue
                settle_received += 1
                trip_reason = getattr(d2, "trip_reason", None)
                trip_mask = getattr(d2, "trip_mask", None)
                if trip_reason:
                    break
            except Exception as exc:
                _note_error("post-reset settle poll", exc)
        if not trip_reason and settle_received == 0:
            data = _stash("inconclusive_settle_diag_not_received")
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=(f"no DIAG frame was received during the 5 s settle window, so 'no trip' is "
                        f"unproven (poll errors {errors['count']}, last: {errors['last']})"),
                observed=data,
            )

    if link_up and not trip_reason:
        data = _stash("no_trip")
        return CaseResult(
            Verdict.PASS,
            reason=(f"no S6a latched on sw_reset (reset confirmed: ESP restarted from uptime_s "
                    f"{uptime_before!r}, Pico boot_id {boot_id_before!r} -> {boot_id_after!r})"),
            observed=data,
            evidence=["no S6a latched on sw_reset"],
        )

    # Plan doc section 6 rule 5 / CLAUDE.md: NEVER clear a trip without first
    # confirming, from safety_get_status()/get_diag(), that the link is up and
    # trip_mask == 1 << (trip_reason - 1) with trip_reason == 6 and no other
    # bit set. Anything else stops the run for a human -- the judge's own
    # check runs too late to be that gate, since by then the clear already
    # happened. A non-S6a trip therefore leaves clear_ok None and the judge
    # FAILs on the reason/mask it observed.
    clear_allowed = (
        link_up and trip_reason == 6 and trip_mask == (1 << (6 - 1))
    )
    if clear_allowed:
        windows.fire_window(ctx, "otb01_tripped", once=True)
        try:
            clear_trip_fn()
        except Exception as exc:
            _note_error("clear_trip", exc)
        clear_deadline = now() + 10.0
        while now() < clear_deadline:
            try:
                status = get_status_fn()
                if getattr(status, "enabled", False):
                    clear_ok = True
                    break
            except Exception as exc:
                _note_error("post-clear status poll", exc)
            sleep(1.0)
        if clear_ok is None:
            clear_ok = False
        elif clear_ok:
            windows.fire_window(ctx, "otb01_cleared", once=True)
        try:
            readiness_trip_ok = readiness_trip_ok_fn()
        except Exception as exc:
            _note_error("readiness trip check", exc)
            readiness_trip_ok = None

    _stash("s6a_latched" if clear_allowed else "other_or_no_link")
    result = J.judge_dual_reset_trip(link_up, trip_reason, trip_mask, clear_ok, readiness_trip_ok)
    if errors["count"]:
        result.reason = (result.reason + "; " if result.reason else "") + (
            f"poll errors {errors['count']}, last: {errors['last']}")
    return result


def _case_ote01(ctx: dict) -> CaseResult:
    """OT-E01: a push of a valid ESP image to POST /api/ota/esp on the
    APPLICATION image must be REFUSED -- the single-slot design
    (docs/OTA_SINGLE_SLOT_PLAN.md) runs the app from the only OTA slot, so
    the "next" partition is the running one. Pass: HTTP 409
    (`refusal_form` http_409; elapsed is informational: the firmware drains the body first), or -- because the firmware may close the
    socket while the client is still uploading -- a connection reset with
    the board provably alive afterward (`connection_closed`, PASS-with-note).
    uptime_s must not go backwards, /api/crash_report must not change, and
    the OTA interlock must still read ok. A successful push into the app is
    no longer a thing this suite exercises; ESP images go in through the
    recovery image. This case never stashes `_ote_pre_update`, so OT-E02
    (rollback) reads NOT_RUN: no update happened to roll back."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E01")
    if not (ctx.get("_isfile_fn") or os.path.isfile)(image_path):
        return CaseResult(Verdict.SKIP, reason=f"ota_image_path is not a file: {image_path!r}")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    import time as _time
    now = ctx.get("_now") or _time.monotonic
    uptime_fn = ctx.get("_esp_uptime_fn") or (lambda: _esp_uptime_s(ctx, host))
    crash_fn = ctx.get("_crash_report_fn") or (lambda: _dashboard_client(ctx).get_crash_report(host, **_read_kw(ctx, "dashboard_http_client")))

    def _read(fn):
        try:
            return fn()
        except Exception:
            return None

    uptime_before = _read(uptime_fn)
    crash_before = _read(crash_fn)

    refusal_form: Optional[str] = None
    status_code: Optional[int] = None
    t0 = now()
    try:
        push = ota.push_esp_image(host, image_path)
        status_code = getattr(push, "status_code", None)
        if getattr(push, "ok", False):
            refusal_form = "accepted"
        else:
            refusal_form = "http_409" if status_code == 409 else "http_other"
    except Exception as exc:
        status_code = getattr(exc, "status", None)
        if status_code == 409:
            refusal_form = "http_409"
        elif status_code is not None:
            refusal_form = "http_other"
        elif _is_transport_reset(exc):
            # ota_http_client._push_image() turns a mid-upload reset
            # (ECONNRESET / EPIPE) into OtaHttpError(status=None,
            # "unreachable") caused by a URLError wrapping the socket error.
            # Only that is "connection_closed", and the judge still requires
            # readbacks proving the board is alive and was not rebooted.
            refusal_form = "connection_closed"
        else:
            refusal_form = f"error:{type(exc).__name__}"
    elapsed_s = now() - t0

    # The firmware drains the refused body before serving anything else, so
    # these readbacks retry through the settle window (see _settled_read).
    uptime_after, settled_at = _settled_read(ctx, uptime_fn)
    crash_after, _ = _settled_read(ctx, crash_fn)
    interlock_ok_after = _settled_interlock_ok(ctx, host)
    # Continuity compares uptime against REAL elapsed time, settle wait included.
    uptime_elapsed_s = elapsed_s if settled_at is None else settled_at - t0

    ctx["_ote01"] = {"refusal_form": refusal_form}
    return J.judge_ota_self_push_refused(
        refusal_form, status_code, elapsed_s, uptime_before, uptime_after,
        crash_before, crash_after, interlock_ok_after,
        uptime_elapsed_s=uptime_elapsed_s,
    )


def _case_ote02(ctx: dict) -> CaseResult:
    """OT-E02: rollback, then read back control_get_zones -- the
    ZONES_CFG_VERSION hazard. Requires OT-E01 to have run (a previous image
    to roll back to)."""
    pre = ctx.get("_ote_pre_update")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-E01 did not run in this session")

    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to roll back: {ireason}")

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)
    dashboard = _dashboard_client(ctx)

    pid_gains_before = _pid_gains(pre.get("zones")) if pre.get("zones") else None

    try:
        ota.rollback_esp(host)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"rollback_esp raised {type(exc).__name__}: {exc}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep
    sleep(5.0)  # let the reboot land before polling

    fw_build_after = None
    zones_config_load_fault = None
    deadline = now() + 60.0
    while now() < deadline:
        try:
            status = dashboard.get_status(host)
            fw_build_after = status.get("fw_build")
            zones_config_load_fault = status.get("zones_config_load_fault")
            if fw_build_after:
                break
        except Exception:
            pass
        sleep(2.0)

    pid_gains_after = None
    try:
        pid_gains_after = _pid_gains(zones.get_zones(host))
    except Exception:
        pass

    return J.judge_ota_rollback(
        fw_build_matches_pre_update=(fw_build_after == pre.get("fw_build")),
        pid_gains_before=pid_gains_before, pid_gains_after=pid_gains_after,
        zones_config_load_fault=zones_config_load_fault,
    )


def _case_refused_image_push(ctx: dict, label: str, image_key: str) -> CaseResult:
    """Shared body of OT-E03/E04/E05: push a bad image and require a refusal
    with RUNNING/fw_build unchanged. SKIPs when the configured path is not a
    file. A push exception that is not an HTTP answer from the board is a FAIL
    (``error:<ExcName>``), never a refusal; the post-push readbacks retry
    through the firmware's refusal-drain window (``_settled_read``) and an
    unreadable one is reported as such, never judged as a change."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get(image_key)
    if not image_path:
        return CaseResult(Verdict.SKIP, reason=f"{image_key} not provided for {label}")
    if not (ctx.get("_isfile_fn") or os.path.isfile)(image_path):
        return CaseResult(Verdict.SKIP, reason=f"{image_key} is not a file: {image_path!r}")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _settled_running(ctx, host)
    fw_build_before = _settled_fw_build(ctx, host)

    refused = None
    push_error = None
    try:
        push = ota.push_esp_image(host, image_path)
        refused = not push.ok
    except Exception as exc:
        refused, push_error = _push_refusal_outcome(exc)

    running_after = _settled_running(ctx, host)
    fw_build_after = _settled_fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after,
                                    push_error=push_error)


def _case_ote03(ctx: dict) -> CaseResult:
    """OT-E03: push the ST-04 image with the last 4 KB flipped -- must be
    refused before reboot, RUNNING/fw_build unchanged."""
    return _case_refused_image_push(ctx, "OT-E03", "ota_corrupt_image_path")


def _case_ote04(ctx: dict) -> CaseResult:
    """OT-E04: push the first 60% of the ST-04 image (a truncated file) --
    same refusal contract as OT-E03 (judge_ota_push_refused): refused
    before reboot, RUNNING/fw_build unchanged."""
    return _case_refused_image_push(ctx, "OT-E04", "ota_truncated_image_path")


def _case_ote05(ctx: dict) -> CaseResult:
    """OT-E05: push `recovery.bin` (or any non-KilnCtrl app_desc) to
    /api/ota/esp -- refused naming the project mismatch, or rejected at
    verify without ever changing RUNNING. Same unchanged-state contract as
    OT-E03/E04 (judge_ota_push_refused); this module never inspects the
    board's refusal text for a specific substring, only that RUNNING/
    fw_build never moved. A missing image file SKIPs: a local file error must
    never read as a refusal."""
    return _case_refused_image_push(ctx, "OT-E05", "ota_wrong_build_image_path")


def _case_ote06(ctx: dict) -> CaseResult:
    """OT-E06: power loss mid-write -- an ``--attended`` operator case.
    Needs a human (or a fixture relay a human has wired up) to physically
    cut the ESP's own supply power partway through the transfer and then
    restore it; there is no way to do that from software alone, so this
    case is gated on an injectable ``ctx["attended_prompt"]`` callable
    (``str -> True | False | None``) another concurrent change
    (``bench3b``) is wiring the real ``--attended`` CLI mechanism for.
    ``None`` (no callable at all -- the flag was not passed) or a False/
    None answer from the operator both SKIP/INCONCLUSIVE rather than ever
    touching the board, so this case merges cleanly ahead of that
    mechanism landing.

    push_fn is injectable (``ctx["_push_fn"]``) because the real
    push_esp_image() call blocks synchronously for the whole streamed
    transfer -- in a real run the fixture/operator cuts power to the ESP
    *while* this call is in flight (a background thread or a second
    process, outside this pure case's scope), so the call is expected to
    raise (connection dropped) rather than return normally. The judge
    (judge_ota_power_loss_mid_write) does not require that specific
    failure shape -- what it actually checks is what the plan cares about:
    fw_build/RUNNING/fingerprint all read back exactly as they were before
    the push was ever attempted, once power is restored."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E06")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    prompt = ctx.get("attended_prompt")
    if prompt is None:
        return CaseResult(Verdict.SKIP, reason="requires --attended")

    ready = prompt(
        "OT-E06: be ready to cut power to the ESP's own supply (fixture_set_relay, "
        "or by hand) at roughly 40% OTA progress, then restore it. Confirm ready to proceed."
    )
    if ready is not True:
        reason = "operator did not respond to the power-cut prompt" if ready is None else "operator declined to proceed"
        return CaseResult(Verdict.SKIP, reason=reason)

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)

    fw_build_before = _fw_build(ctx, host)
    try:
        zones_before = zones.get_zones(host)
    except Exception:
        zones_before = None

    push_fn = ctx.get("_push_fn") or (lambda: ota.push_esp_image(host, image_path))
    try:
        push_fn()
    except Exception:
        pass  # expected: the connection drops when the fixture/operator cuts power mid-write

    restored = prompt("OT-E06: confirm ESP supply power has been restored and the board has rebooted.")
    if restored is not True:
        reason = "operator did not confirm power restoration" if restored is None else "operator reported power was not restored"
        return CaseResult(Verdict.INCONCLUSIVE, reason=reason)

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    running_after = None
    fw_build_after = None
    deadline = now() + 60.0
    while now() < deadline:
        running_after = _running_partition(ctx, host)
        fw_build_after = _fw_build(ctx, host)
        if fw_build_after:
            break
        sleep(2.0)

    fingerprint_identical = None
    try:
        zones_after = zones.get_zones(host)
        fingerprint_identical = J.pid_gains_match(_pid_gains(zones_before), _pid_gains(zones_after)) if zones_before else None
    except Exception:
        pass

    return J.judge_ota_power_loss_mid_write(
        fw_build_before, fw_build_after, running_after, "app", fingerprint_identical,
    )


#: Bounds for the self-started heat of OT-E07/OT-E08 (allow_heat=True AND
#: ota_allow_heat=True -- see _heat_opted_in).
OTA_HEAT_START_WAIT_S = 30.0
OTA_HEAT_STOP_WAIT_S = 30.0
OTA_HEAT_POLL_S = 1.0
OTA_HEAT_SKIP_NO_ALLOW = "allow_heat not set; OT-E07/E08/G04 start their own heat"
OTA_HEAT_SKIP_NO_OTA_ALLOW = (
    "ota_allow_heat not set; OT-E07/E08/G04 start their own heat (ota_matrix_run(allow_heat=True) "
    "sets it; bench_test_run needs ota_allow_heat=True as well as allow_heat)"
)


def _heat_opted_in(ctx: dict) -> "tuple[bool, str]":
    """OT-E07/E08 start a real firing/autotune only when BOTH ctx["allow_heat"]
    and the OT-specific, default-False ctx["ota_allow_heat"] are true (same
    shape as LCD-19/LCD-22: allow_heat alone defaults True in bench_test_run)."""
    if not ctx.get("allow_heat"):
        return False, OTA_HEAT_SKIP_NO_ALLOW
    if not ctx.get("ota_allow_heat"):
        return False, OTA_HEAT_SKIP_NO_OTA_ALLOW
    return True, ""


def _bounded_wait(ctx: dict, predicate, timeout_s: float, poll_s: float = OTA_HEAT_POLL_S) -> bool:
    """Poll ``predicate`` (exceptions count as False) until true or
    ``timeout_s``; a hard attempt cap keeps it finite under an injected clock
    that never advances. Hooks: ``ctx["_now"]``, ``ctx["_sleep_fn"]``."""
    import math
    import time as _time
    now = ctx.get("_now") or _time.monotonic
    sleep = ctx.get("_sleep_fn") or _time.sleep
    deadline = now() + timeout_s
    for _ in range(math.ceil(timeout_s / poll_s) + 2):
        try:
            if predicate():
                return True
        except Exception:
            pass
        if now() >= deadline:
            return False
        sleep(poll_s)
    return False


def _heat_stop_problem(ctx: dict, stop_fn, is_stopped_fn) -> str:
    """Run a heat teardown and verify it: returns "" on success or a problem
    description. Never raises. ``is_stopped_fn`` is polled (bounded)."""
    try:
        stop_fn()
    except Exception as exc:
        return f"stop raised {type(exc).__name__}: {exc}"
    if not _bounded_wait(ctx, is_stopped_fn, OTA_HEAT_STOP_WAIT_S):
        return f"executor/autotune not confirmed idle with relays off within {OTA_HEAT_STOP_WAIT_S:.0f}s of stop"
    return ""


OTA_HEAT_SKIP_TAINTED = "run tainted by an earlier unconfirmed heat stop"


class _StartRefused(RuntimeError):
    """The start was refused before anything was sent (or the helper already
    cleaned up after itself): nothing to tear down."""


def _default_ote07_heat(ctx: dict):
    """(start_fn, stop_fn, is_stopped_fn) reusing HP-01's bench-profile
    machinery (`cases_heat._start_bench_profile`/`_cleanup_bench_profile`).
    No rest gate: this is not a measurement, only a run to push against."""
    from . import cases_heat as H
    from . import cases_autotune as A
    srv = _srv(ctx)

    def start():
        ok, reason, _amb = H._start_bench_profile(ctx, zone_mask=0b001)
        if not ok:
            # _start_bench_profile already cleaned up on its own failure paths.
            raise _StartRefused(reason)
        if not _bounded_wait(ctx, lambda: srv._profiles.get_exec_status().state_name == OTE07_DEFAULT_STATE,
                             OTA_HEAT_START_WAIT_S):
            raise RuntimeError(f"executor not {OTE07_DEFAULT_STATE!r} within {OTA_HEAT_START_WAIT_S:.0f}s of start")

    def stop():
        H._cleanup_bench_profile(ctx)

    def is_stopped():
        return (srv._profiles.get_exec_status().state_name == "idle") and A._relays_off(ctx) is True

    def precheck() -> str:
        state = srv._profiles.get_exec_status().state_name
        if state not in ("idle", "done"):
            return f"executor state is {state!r}, not idle/done -- not starting heat over a firing this case did not start"
        ok, reason = A._autotune_not_running(ctx)
        if not ok:
            return f"{reason} -- not starting a firing alongside an autotune this case did not start"
        return ""

    return start, stop, is_stopped, precheck


def _default_ote08_heat(ctx: dict):
    """(start_fn, stop_fn, is_stopped_fn) reusing AT-01's start and
    `_cleanup_autotune`. Ramp assist is handled by the caller via
    `cases_autotune._with_ramp_assist_off`."""
    from . import cases_autotune as A
    srv = _srv(ctx)

    def start():
        ok, reason = A._at_preflight(ctx)
        if not ok:
            raise _StartRefused(reason)
        ok_start, err = srv._autotune.start(zone=0, method=0, step_duty_or_setpoint_c=A.AT_STEP_DUTY)
        if not ok_start:
            raise _StartRefused(f"autotune.start refused: {err}")
        if not _bounded_wait(ctx, lambda: srv._autotune.get_status().state_name in OTE08_ACTIVE_STATES,
                             OTA_HEAT_START_WAIT_S):
            raise RuntimeError(f"autotune not active within {OTA_HEAT_START_WAIT_S:.0f}s of start")

    def stop():
        A._cleanup_autotune(ctx)

    def is_stopped():
        return A._autotune_not_running(ctx)[0] and A._relays_off(ctx) is True

    def precheck() -> str:
        ok, reason = A._autotune_not_running(ctx)
        if not ok:
            return reason
        state = srv._profiles.get_exec_status().state_name
        if state != "idle":
            return f"executor state is {state!r}, not idle -- not starting autotune over a firing this case did not start"
        return ""

    return start, stop, is_stopped, precheck


def _case_update_refused_during_state(
    ctx: dict, get_exec_state_fn, expected_state: str, image_ctx_key: str = "ota_image_path",
    default_heat=None,
) -> CaseResult:
    """Shared body of OT-E07 (during a firing)/OT-E08 (during autotune):
    read the exec/autotune state, read the live interlock, attempt a push,
    then confirm the state is unchanged. Deliberately does NOT gate on
    `_is_idle`/`_interlock_ok` the way every other case in this module
    does -- the entire point of this pair is exercising the busy path, so
    treating "not idle"/"interlock not ok" as a precondition failure would
    make the case SKIP the very thing it exists to test. Both cases start
    their own short HP/AT run per the plan's fixed order footnote
    ("OT-E07/E08 (re-using a short HP/AT)"); the run itself is started and
    torn down by an injectable ``ctx["_start_state_fn"]``/
    ``ctx["_stop_state_fn"]`` pair so this stays unit-testable without
    actually driving a firing or an autotune step from this module (that
    machinery already lives in cases_heat.py/the AT-* cases and is not
    duplicated here)."""
    host = ctx.get("host")
    image_path = ctx.get(image_ctx_key)
    if not image_path:
        return CaseResult(Verdict.SKIP, reason=f"{image_ctx_key} not provided")

    start_fn = ctx.get("_start_state_fn")
    stop_fn = ctx.get("_stop_state_fn")
    is_stopped_fn = None
    state_before = get_exec_state_fn()
    if state_before != expected_state and start_fn is None and default_heat is not None:
        # Injected fns keep precedence; otherwise allow_heat + ota_allow_heat
        # opt into the real HP-01/AT-01 starters.
        opted_in, why = _heat_opted_in(ctx)
        if not opted_in:
            return CaseResult(Verdict.SKIP, reason=why)
        if ctx.get("_tainted"):
            return CaseResult(Verdict.SKIP, reason=OTA_HEAT_SKIP_TAINTED)
        start_fn, stop_fn, is_stopped_fn, precheck = default_heat(ctx)
        try:
            busy = precheck()
        except Exception as exc:
            busy = f"could not read executor state: {type(exc).__name__}: {exc}"
        if busy:
            # No teardown: whatever is running was not started by this case.
            return CaseResult(Verdict.SKIP, reason=busy)

    def _teardown() -> str:
        if stop_fn is None:
            return ""
        return _heat_stop_problem(ctx, stop_fn, is_stopped_fn or (lambda: True))

    started_here = False
    if state_before != expected_state:
        if start_fn is None:
            return CaseResult(
                Verdict.SKIP,
                reason=f"state is {state_before!r}, not {expected_state!r}, and no _start_state_fn was provided to start one",
            )
        # started_here BEFORE start_fn(): the board may have accepted the start
        # before the call raised or the wait was interrupted, so every exit from
        # here on runs the verified teardown.
        started_here = True
        try:
            start_fn()
            state_before = get_exec_state_fn()
        except _StartRefused as exc:
            return CaseResult(Verdict.SKIP, reason=f"could not start {expected_state}: {exc}")
        except Exception as exc:
            problem = _teardown()
            if problem:
                ctx["_tainted"] = True
                return CaseResult(
                    Verdict.FAIL,
                    reason=(f"could not start {expected_state}: {type(exc).__name__}: {exc}; heat could NOT be "
                            f"confirmed stopped afterwards ({problem}) -- run tainted"),
                )
            return CaseResult(Verdict.SKIP, reason=f"could not start {expected_state}: {type(exc).__name__}: {exc}")
        except BaseException:
            if _teardown():
                ctx["_tainted"] = True
            raise

    def _attempt() -> CaseResult:
        if state_before != expected_state:
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=f"state_before={state_before!r} after starting one, expected {expected_state!r}",
            )

        interlock_ok, _ireason = _interlock_ok(ctx, host)

        ota = _ota_client(ctx)
        push_fn = ctx.get("_push_fn") or (lambda: ota.push_esp_image(host, image_path))
        push_refused = None
        try:
            push = push_fn()
            push_refused = not getattr(push, "ok", False)
        except Exception as exc:
            push_refused, push_error = _push_refusal_outcome(exc)
            if push_error:
                return CaseResult(Verdict.FAIL, reason=f"push did not get an HTTP answer from the board: {push_error}",
                                  observed={"push_error": push_error})

        # The refused push leaves the firmware draining the body, during which
        # it answers nothing else: retry the state read through that window.
        state_after, _ = _settled_read(ctx, get_exec_state_fn)

        return J.judge_ota_update_refused_during_state(
            interlock_ok, push_refused, state_before, state_after, expected_state,
        )

    try:
        result = _attempt()
    except BaseException:
        # Stop always runs, even when the push raised; an unconfirmed stop
        # still taints the run while the original exception propagates.
        if _teardown():
            ctx["_tainted"] = True
        raise
    problem = _teardown()
    if problem:
        # Never leave a firing/autotune silently running: taint the run (same
        # flag cases_autotune/cases_heat use for a failed restore) and FAIL.
        ctx["_tainted"] = True
        note = f"heat started by this case could NOT be confirmed stopped ({problem}) -- run tainted"
        return CaseResult(
            Verdict.FAIL, reason=f"{result.reason}; {note}" if result.reason else note,
            observed=result.observed, expected=result.expected, evidence=list(result.evidence) + [note],
        )
    return result


#: `devices_profiles.ProfileExecStatus.STATE_NAMES` -- the real, LOWERCASE
#: vocabulary `state_name` actually returns. OT-E07's default was written as
#: "RUNNING" (the plan document's prose casing), which no real status can
#: ever equal, so the case could only ever report INCONCLUSIVE on a board
#: while its unit tests -- which fed the same fabricated "RUNNING" back in
#: -- stayed green. `test_bench_test_cases_ota.py` now pins both defaults
#: against the device modules' own STATE_NAMES so the two cannot drift
#: apart again.
OTE07_DEFAULT_STATE = "running"

#: Autotune has no single "active" state: `devices_autotune.AutotuneStatus.
#: STATE_NAMES` reports settling/stepping/relay_approach/relay_cycling while
#: a run is in flight, and which one is current legitimately changes DURING
#: the case (settling -> stepping), so comparing a raw state_name before and
#: after the push would fail on an ordinary, correct run. OT-E08 therefore
#: normalizes any in-flight state to this one label and compares that; a run
#: that drops to idle/done/aborted across the refused push still fails the
#: judge's state_after check, which is the thing the case exists to catch.
OTE08_ACTIVE_STATES = frozenset({"settling", "stepping", "relay_approach", "relay_cycling"})
OTE08_ACTIVE_LABEL = "active"


def _normalize_autotune_state(state_name):
    """Map any in-flight autotune state to OTE08_ACTIVE_LABEL, leaving
    idle/done/aborted/None as-is so they still read as "not active"."""
    return OTE08_ACTIVE_LABEL if state_name in OTE08_ACTIVE_STATES else state_name


def _case_ote07(ctx: dict) -> CaseResult:
    """OT-E07: an OTA update attempted while an HP-01-shaped firing is
    running (and, per the plan, again while paused -- pass
    ``ctx["_ote07_expected_state"] = "paused"``) must be refused, and the
    firing must continue unaffected."""
    get_state_fn = ctx.get("_exec_state_fn")
    if get_state_fn is None:
        srv = _srv(ctx)
        get_state_fn = lambda: getattr(srv._profiles.get_exec_status(), "state_name", None)
    expected_state = ctx.get("_ote07_expected_state", OTE07_DEFAULT_STATE)
    return _case_update_refused_during_state(ctx, get_state_fn, expected_state, default_heat=_default_ote07_heat)


def _case_ote08(ctx: dict) -> CaseResult:
    """OT-E08: an OTA update attempted while an AT-01-shaped autotune is
    active must be refused (`ota_interlock.c:50`), and autotune must
    continue unaffected. The raw state_name is normalized to "active"
    first -- see OTE08_ACTIVE_STATES."""
    raw_state_fn = ctx.get("_autotune_state_fn")
    if raw_state_fn is None:
        srv = _srv(ctx)
        raw_state_fn = lambda: getattr(srv._autotune.get_status(), "state_name", None)
    get_state_fn = lambda: _normalize_autotune_state(raw_state_fn())
    expected_state = ctx.get("_ote08_expected_state", OTE08_ACTIVE_LABEL)

    def body(c):
        return _case_update_refused_during_state(c, get_state_fn, expected_state, default_heat=_default_ote08_heat)

    if _heat_opted_in(ctx)[0] and ctx.get("_start_state_fn") is None and get_state_fn() != expected_state:
        if ctx.get("_tainted"):
            return CaseResult(Verdict.SKIP, reason=OTA_HEAT_SKIP_TAINTED)
        # No image: the body would SKIP anyway; do not touch ramp assist for it.
        if not ctx.get("ota_image_path"):
            return CaseResult(Verdict.SKIP, reason="ota_image_path not provided")
        # Precheck BEFORE the ramp-assist wrap: never flip ramp assist on a
        # firing/autotune this case did not start.
        try:
            busy = _default_ote08_heat(ctx)[3]()
        except Exception as exc:
            busy = f"precheck raised {type(exc).__name__}: {exc}"
        if busy:
            return CaseResult(Verdict.SKIP, reason=busy)
        # Self-started autotune: ramp assist must be off for it (as AT-01),
        # restored in finally; an unconfirmed restore taints and FAILs.
        from . import cases_autotune as A
        return A._with_ramp_assist_off(ctx, body)
    return body(ctx)


def _case_ote09(ctx: dict) -> CaseResult:
    """OT-E09: POST /api/ota/esp with no admin session at all -- 401/403,
    nothing written. The AP-password HMAC challenge/response scheme this
    case used to also probe was retired 2026-09-29 (WEB_AUTH_PLAN.md item
    2b); with web auth on, ROUTE_TIER_ADMIN alone is the gate this case now
    exercises. **With web auth OFF, this route is unauthenticated by design
    (same as every other ADMIN route once auth is off) -- an unauthenticated
    push would then actually flash the board instead of being refused, which
    is not what this case is testing.** So this case only runs with web auth
    confirmed on; it SKIPs otherwise rather than risk a real flash."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E09")
    if not (ctx.get("_isfile_fn") or os.path.isfile)(image_path):
        return CaseResult(Verdict.SKIP, reason=f"ota_image_path is not a file: {image_path!r}")

    web_enabled_fn = ctx.get("_web_auth_enabled_fn")
    if web_enabled_fn is None:
        def web_enabled_fn():
            cfg = _wac.get_auth_config(host)
            return bool(cfg.get("web_enabled"))
    try:
        web_enabled = web_enabled_fn()
    except Exception as exc:
        return CaseResult(Verdict.SKIP, reason=f"could not read GET /api/auth/config to confirm web auth is on: {exc}")
    if not web_enabled:
        return CaseResult(Verdict.SKIP, reason="web auth is off -- an unauthenticated push would really flash the "
                                                "board instead of being refused, so OT-E09 does not run")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _settled_running(ctx, host)
    fw_build_before = _settled_fw_build(ctx, host)

    push_fn = ctx.get("_push_no_credential_fn") or (lambda: ota.push_esp_image_unauthenticated(host, image_path))
    refused = None
    push_error = None
    try:
        push = push_fn()
        refused = not push.ok
    except Exception as exc:
        # Deliberately unauthenticated: the firmware closes a large
        # unauthenticated refusal (http_auth_refusal_should_close()), so a
        # transport reset IS this case's expected refusal. Local errors are not.
        refused, push_error = _push_refusal_outcome(exc, reset_is_refusal=True)

    running_after = _settled_running(ctx, host)
    fw_build_after = _settled_fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after,
                                    push_error=push_error)


def _case_ote10(ctx: dict) -> CaseResult:
    """OT-E10: with web auth on (WEB-SEC-03 turned it on earlier in this
    run), the OT-E01 push repeated with an ADMIN session cookie must get
    PAST the auth tier, and again with a ``user``-tier session must be
    refused at it -- ROUTE_TIER_ADMIN is
    this route's only gate now that the AP-password HMAC has been retired
    (2026-09-29, WEB_AUTH_PLAN.md item 2b), so this case exercises that tier
    check directly rather than a scheme replacement. Credentials come only
    from ctx overrides or the
    ``KILNCTL_WEB_*`` environment variables (same discipline as
    cases_web_rw.py's WEB-SEC-03: never hardcoded, never logged) -- SKIP
    if either the admin or the user-tier credential pair is missing,
    since there is no separate operator-visible knob for the second,
    non-admin account this case specifically needs.

    On the application image a push to the running app is refused 409 by
    design (see OT-E01), so "admin accepted" now means "passed the auth
    gate": ok, or HTTP 409 (the handler's own target refusal). A user-tier
    refusal must be a 401/403 -- a 409 there would mean the user session got
    through the tier gate, which is a FAIL. An admin transport error counts
    as not-accepted unless uptime/crash-report readbacks prove the board
    alive (connection closed mid-upload); a user-tier transport error is FAIL."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E10")

    admin_user = ctx.get("web_admin_username") or os.environ.get("KILNCTL_WEB_USERNAME")
    admin_pass = ctx.get("web_admin_password") or os.environ.get("KILNCTL_WEB_PASSWORD")
    user_user = ctx.get("web_user_username") or os.environ.get("KILNCTL_WEB_USER_USERNAME")
    user_pass = ctx.get("web_user_password") or os.environ.get("KILNCTL_WEB_USER_PASSWORD")
    if not all([admin_user, admin_pass, user_user, user_pass]):
        return CaseResult(
            Verdict.SKIP,
            reason=(
                "requires KILNCTL_WEB_USERNAME/PASSWORD (admin) and "
                "KILNCTL_WEB_USER_USERNAME/PASSWORD (a distinct user-tier account) -- credentials not provided"
            ),
        )

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    login_fn = ctx.get("_login_fn")
    if login_fn is None:
        return CaseResult(Verdict.SKIP, reason="no _login_fn provided to establish a web-auth session")

    admin_status, admin_cookie = login_fn(admin_user, admin_pass)
    if admin_status != 200 or not admin_cookie:
        return CaseResult(Verdict.FAIL, reason=f"admin login failed: status={admin_status!r}")

    user_status, user_cookie = login_fn(user_user, user_pass)
    if user_status != 200 or not user_cookie:
        return CaseResult(Verdict.FAIL, reason=f"user login failed: status={user_status!r}")

    ota = _ota_client(ctx)
    push_with_session_fn = ctx.get("_push_with_session_fn") or (
        lambda cookie: ota.push_esp_image_with_session(host, image_path, cookie)
    )

    uptime_fn = ctx.get("_esp_uptime_fn") or (lambda: _esp_uptime_s(ctx, host))
    crash_fn = ctx.get("_crash_report_fn") or (lambda: _dashboard_client(ctx).get_crash_report(host, **_read_kw(ctx, "dashboard_http_client")))

    def _read(fn):
        try:
            return fn()
        except Exception:
            return None

    uptime_before = _read(uptime_fn)
    crash_before = _read(crash_fn)

    import time as _time
    now = ctx.get("_now") or _time.monotonic
    admin_ok = None
    admin_note = None
    t0 = now()
    try:
        admin_result = push_with_session_fn(admin_cookie)
        admin_ok = bool(getattr(admin_result, "ok", False)) or getattr(admin_result, "status_code", None) == 409
    except Exception as exc:
        status = getattr(exc, "status", None)
        if status == 409:
            admin_ok = True
        elif _is_transport_reset(exc):
            # Mid-upload close: no response readable. Accepted past the auth
            # gate only if the board is provably alive and unharmed.
            elapsed_s = now() - t0
            uptime_after, settled_at = _settled_read(ctx, uptime_fn)
            crash_after, _ = _settled_read(ctx, crash_fn)
            if settled_at is not None:
                elapsed_s = settled_at - t0
            alive = (J.uptime_continuous(uptime_before, uptime_after, elapsed_s)
                     and crash_before is not None and crash_after == crash_before)
            admin_ok = alive
            if alive:
                admin_note = "admin push: connection closed mid-upload; uptime not reset, crash report unchanged"
        else:
            admin_ok = False

    user_refused = None
    try:
        user_result = push_with_session_fn(user_cookie)
        user_refused = (not getattr(user_result, "ok", False)
                        and getattr(user_result, "status_code", None) in (401, 403))
    except Exception as exc:
        user_refused = getattr(exc, "status", None) in (401, 403)

    result = J.judge_ota_session_auth_tiers(admin_ok, user_refused)
    if admin_note and isinstance(result.observed, dict):
        result.observed["note"] = admin_note
    return result


def _case_ote12(ctx: dict) -> CaseResult:
    """OT-E12: otadata state recorded after every OT-E case in this
    session. Observer -- NOT_RUN if none of OT-E01/E02/E03 ran."""
    host = ctx.get("host")
    if not any(k in ctx for k in ("_ote01", "_ote_pre_update")):
        return CaseResult(Verdict.NOT_RUN, reason="no OT-E case ran in this session")
    running = _settled_running(ctx, host)
    # OT-E01/OT-E02 both expect the board to be running `app` afterward;
    # OT-E03's refusal case leaves it wherever it already was (checked by
    # OT-E03's own judge), so this observer's job is the app-expectation.
    return J.judge_ota_partitions_state(running, "app")


# ---------------------------------------------------------------------------
# OT-P* -- Pico OTA relayed over the isolated link (Wave 4). See this
# module's docstring for the blocked->unblocked history and why identity is
# read from safety_get_fw_version()/safety_get_diag() text rather than an
# HTTP status surface the Pico doesn't have.
# ---------------------------------------------------------------------------

def _pico_commit_and_boot_reason(ctx: dict) -> "tuple[Optional[str], Optional[str]]":
    srv = _srv(ctx)
    try:
        fw_text = srv.safety_get_fw_version()
    except Exception:
        fw_text = ""
    try:
        diag_text = srv.safety_get_diag()
    except Exception:
        diag_text = ""
    return J.parse_fw_version_commit(fw_text), J.parse_diag_boot_reason(diag_text)


def _pico_boot_id(ctx: dict) -> Optional[int]:
    """Read `boot_id` out of the same GET_FW_VERSION cache
    `_pico_commit_and_boot_reason` uses. Stashed alongside commit/boot
    reason by OT-P01/OT-P02 so a later reviewer (or a future OT-P case)
    can check a Pico reboot actually happened via boot_id changing, the
    same "a real reboot was observed on the wire" signal
    get_pico_rollback_status()'s own "rebooting" status is defined by."""
    srv = _srv(ctx)
    try:
        fw_text = srv.safety_get_fw_version()
    except Exception:
        return None
    return J.parse_fw_version_boot_id(fw_text)


def _pico_trip_reason_mask(ctx: dict) -> "tuple[Optional[int], Optional[int], Optional[str]]":
    """Returns (reason, mask, why). `why` is None whenever `reason` was
    read successfully (even if that reason is 0, "no trip"); it is only
    populated to explain why `reason` is None -- either the transport call
    itself raised, or the diag text came back but didn't parse. Callers
    must not conflate "reason is None" (unreadable) with "reason is 0"
    (readable, no trip pending) -- see OT-P05's history of doing exactly
    that."""
    srv = _srv(ctx)
    try:
        diag_text = srv.safety_get_diag()
    except Exception as exc:
        return None, None, f"safety_get_diag raised {type(exc).__name__}: {exc}"
    reason = J.parse_trip_reason(diag_text)
    mask = J.parse_trip_mask(diag_text)
    why = None if reason is not None else "trip_reason unparseable from diag text"
    return reason, mask, why


def _pico_trip_pending(ctx: dict) -> "tuple[Optional[bool], Optional[str]]":
    """Returns (pending, why). `pending` is None only when the trip state
    could not be determined at all -- distinct from `False`, which means
    the trip state WAS read and no trip is pending. `why` explains a None
    `pending`; it is None whenever `pending` is not None."""
    reason, _mask, why = _pico_trip_reason_mask(ctx)
    if reason is None:
        return None, why
    return reason != 0, None


def _trip_is_the_clearable_s6a(ctx: dict) -> bool:
    """Plan section 6 rule 5: a trip may only be cleared once it is
    confirmed to be S6a and nothing else -- `trip_reason == 6` with
    `trip_mask == 1 << (trip_reason - 1)` (0x0020) and no other bit. Any
    other latched trip stops the run for a human; the harness leaves it
    alone. Anything unreadable is treated as "do not clear"."""
    reason, mask, _why = _pico_trip_reason_mask(ctx)
    if reason != 6 or mask is None:
        return False
    return mask == J.safety_trip_mask_for_reason(reason)


def _commissioning(ctx: dict, host: str):
    from .. import safety_cfg_http_client
    fn = ctx.get("_commissioning_fn") or (lambda: safety_cfg_http_client.get_commissioning(host))
    try:
        return fn()
    except Exception:
        return None


def _poll_pico_phase(ctx: dict, host: str, deadline_s: float = 180.0) -> "tuple[Optional[str], Optional[str]]":
    """Polls GET /api/ota/pico/status to a terminal phase (done/failed),
    same shape as OT-E01's ESP polling loop. Returns (phase, last_error)."""
    ota = _ota_client(ctx)
    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    phase = None
    last_error = None
    deadline = now() + deadline_s
    while now() < deadline:
        try:
            status = ota.get_pico_status(host)
            phase = status.get("phase")
            last_error = status.get("last_error")
        except Exception:
            phase = None
        if phase in ("done", "failed"):
            break
        sleep(2.0)
    return phase, last_error


def _case_otp01(ctx: dict) -> CaseResult:
    """OT-P01: relay a good Pico image into the inactive slot over the
    isolated link, poll to a terminal phase, then confirm identity/boot
    reason/commissioning via the ESP's safety-link cache (see module
    docstring for why there is no Pico-side HTTP status surface)."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_pico_image_path")
    expected_commit = ctx.get("ota_pico_image_commit")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_image_path not provided for OT-P01")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    if trip_pending is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"trip state unreadable: {trip_why}; refusing to push for OT-P01 rather than assuming no trip")
    if trip_pending:
        return CaseResult(Verdict.SKIP, reason="a trip is currently pending, refusing to start OT-P01")

    commit_before, _ = _pico_commit_and_boot_reason(ctx)
    commissioning_before = _commissioning(ctx, host)

    ota = _ota_client(ctx)
    try:
        push = ota.push_pico_image(host, image_path)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"push_pico_image raised {type(exc).__name__}: {exc}")
    if not push.ok:
        return CaseResult(Verdict.FAIL, reason=f"push_pico_image refused: status={push.status_code} body={push.body!r}")

    phase, last_error = _poll_pico_phase(ctx, host)
    commit_after, boot_reason = _pico_commit_and_boot_reason(ctx)
    boot_id_after = _pico_boot_id(ctx)
    commissioning_after = _commissioning(ctx, host)
    commissioning_identical = (
        (commissioning_before == commissioning_after)
        if commissioning_before is not None and commissioning_after is not None
        else None
    )

    ctx["_otp01"] = {
        "commit_before": commit_before, "commit_after": commit_after,
        "phase": phase, "last_error": last_error, "boot_reason": boot_reason,
        "boot_id_after": boot_id_after,
    }
    return J.judge_ota_pico_push_applied(
        phase, commit_after, expected_commit, boot_reason, commissioning_identical,
        last_error=last_error,
    )


def _case_otp02(ctx: dict) -> CaseResult:
    """OT-P02: roll the safety processor's bootloader back to the slot
    OT-P01 relayed away from, and confirm the pre-P01 commit is running
    again. Requires OT-P01 to have run this session (a slot to roll back
    from, and the pre-update commit to compare against)."""
    pre = ctx.get("_otp01")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-P01 did not run in this session")
    # Never ask a Pico to roll back a slot it was never updated into: on a
    # flat, bootloader-less bench unit OT-P01's relay ends at the
    # running-image-overlap refusal (state 9) and there is no previous slot
    # to revert to. Belt-and-braces with the runner's own depends_on gate,
    # which only runs this case when OT-P01 read PASS.
    if pre.get("phase") != "done":
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"OT-P01's relay ended at phase={pre.get('phase')!r}, so there is no new slot to roll back from",
        )

    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to roll back: {ireason}")

    commissioning_before = _commissioning(ctx, host)
    ota = _ota_client(ctx)
    try:
        ota.rollback_pico(host)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"rollback_pico raised {type(exc).__name__}: {exc}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    rollback_status = None
    deadline = now() + 60.0
    while now() < deadline:
        try:
            rollback_status = ota.get_pico_rollback_status(host).get("status")
        except Exception:
            rollback_status = None
        if rollback_status not in (None, "idle", "pending"):
            break
        sleep(2.0)

    commit_after, _ = _pico_commit_and_boot_reason(ctx)
    commissioning_after = _commissioning(ctx, host)
    commissioning_identical = (
        (commissioning_before == commissioning_after)
        if commissioning_before is not None and commissioning_after is not None
        else None
    )
    return J.judge_ota_pico_rollback(
        rollback_status, commit_after, pre.get("commit_before"), commissioning_identical,
    )


def _case_otp03(ctx: dict) -> CaseResult:
    """OT-P03: a CRC-corrupted Pico image must be refused/fail the staged
    check before the bootloader ever switches slots -- same 'refused
    means truly untouched' contract as OT-E03, but for the Pico's commit
    and boot reason rather than the ESP's RUNNING/fw_build."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_pico_corrupt_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_corrupt_image_path not provided for OT-P03")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    if trip_pending is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"trip state unreadable: {trip_why}; refusing to push for OT-P03 rather than assuming no trip")
    if trip_pending:
        return CaseResult(Verdict.SKIP, reason="a trip is currently pending, refusing to start OT-P03")

    commit_before, _ = _pico_commit_and_boot_reason(ctx)

    ota = _ota_client(ctx)
    refused_or_failed = None
    last_error = None
    try:
        push = ota.push_pico_image(host, image_path)
        if not push.ok:
            refused_or_failed = True
        else:
            phase, last_error = _poll_pico_phase(ctx, host)
            refused_or_failed = phase == "failed"
    except Exception:
        refused_or_failed = True  # a raised transport/CRC error is also a refusal

    commit_after, boot_reason = _pico_commit_and_boot_reason(ctx)
    return J.judge_ota_pico_bad_image_fallback(
        refused_or_failed, commit_before, commit_after, boot_reason, last_error=last_error,
    )


def _case_otp04(ctx: dict) -> CaseResult:
    """OT-P04: judges OT-P01's own captured relay specifically for the
    2026-09-18 erase-time watchdog defect's two documented symptoms.
    Observer only -- NOT_RUN if OT-P01 did not run this session."""
    pre = ctx.get("_otp01")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-P01 did not run in this session")
    return J.judge_ota_pico_no_watchdog_signature(pre.get("boot_reason"), pre.get("last_error"))


def _case_otp05(ctx: dict) -> CaseResult:
    """OT-P05: a Pico update attempted while a real trip is latched must be
    refused, with the running commit left exactly unchanged. Requires a
    trip already pending (e.g. FL-11's S6a, before it is cleared) --
    INCONCLUSIVE if none is, per judgments.judge_ota_pico_refused_with_
    trip_pending. The harness clears the trip afterward ONLY when it is
    confirmed to be S6a and nothing else (plan section 6 rule 5), via the
    same clear-trip path SP-08/SP-09 use, injectable as
    ctx['_clear_trip_fn'] for testing."""
    host = ctx.get("host")
    image_path = ctx.get("ota_pico_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_image_path not provided for OT-P05")

    # Plan section 6 rule 1 applies here exactly as to every other OTA
    # action in this module: executor idle and GET /api/ota/interlock ok,
    # checked immediately before the push, never assumed from the fact a
    # trip is latched.
    idle, ireason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=ireason)
    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    commit_before, _ = _pico_commit_and_boot_reason(ctx)

    if trip_pending is None:
        return CaseResult(Verdict.SKIP, reason=f"trip state unreadable: {trip_why}")

    push_refused = None
    if trip_pending:
        ota = _ota_client(ctx)
        try:
            push = ota.push_pico_image(host, image_path)
            push_refused = not push.ok
        except Exception:
            push_refused = True

    commit_after, _ = _pico_commit_and_boot_reason(ctx)
    result = J.judge_ota_pico_refused_with_trip_pending(trip_pending, push_refused, commit_before, commit_after)

    if trip_pending and _trip_is_the_clearable_s6a(ctx):
        clear_fn = ctx.get("_clear_trip_fn") or (lambda: _default_clear_trip_fn(ctx))
        try:
            clear_fn()
        except Exception:
            pass
    return result


# ---------------------------------------------------------------------------
# OT-G01..G06: GitHub-release update cases (docs/GITHUB_RELEASE_UPDATE_PLAN.md
# section 10, docs/BENCH_TEST_SYSTEM_PLAN.md section 3.4). Bodies written
# against the stage/fetch routes only; UNIT-TESTED WITH FAKES, NEVER RUN ON
# HARDWARE. Every case gates itself on executor idle plus the OTA interlock
# immediately before acting (G04 excepted: it exercises the busy path, like
# OT-E07), SKIPs when its parameter is absent, and never turns an unreadable
# or unexpected board answer into a pass.
# ---------------------------------------------------------------------------

#: How long G03/G05/G06 wait for the board's fetch job / reboot, seconds.
OTG_FETCH_WAIT_S = 180.0
#: update_fetch.c bounds cancel latency at about 10 s; generous slack.
OTG_FETCH_CANCEL_WAIT_S = 30.0
OTG_REBOOT_WAIT_S = 90.0
OTG_POLL_S = 2.0


def _update_client(ctx: dict):
    client = ctx.get("update_http_client")
    if client is None:
        from .. import update_http_client as client
    return client


def _release_client(ctx: dict):
    client = ctx.get("update_release_http_client")
    if client is None:
        from .. import update_http_client as client
    return client


def _otg_gate(ctx: dict, label: str) -> "Optional[CaseResult]":
    """Executor idle, then OTA interlock ok, each read immediately before the
    case acts. Returns a SKIP result, or None when clear to proceed."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)
    ok, ireason = _interlock_ok(ctx, ctx.get("host"))
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to run {label}: {ireason}")
    return None


def _stage_key(st: dict) -> tuple:
    return (bool(st.get("staged")), st.get("sha256", ""), st.get("semver", ""),
            st.get("image_length"), st.get("state", ""))


def _read_stage(ctx: dict, host) -> "Optional[dict]":
    """GET /api/update/stage retried through a refusal-drain window; None if
    it never answered."""
    upd = _update_client(ctx)
    value, _ = _settled_read(ctx, lambda: upd.get_stage_status(host, **_read_kw(ctx, "update_http_client")))
    return value


def _read_image(ctx: dict, key: str) -> "tuple[Optional[bytes], str]":
    path = ctx.get(key)
    if not (ctx.get("_isfile_fn") or os.path.isfile)(path):
        return None, f"{key} is not a file: {path!r}"
    try:
        reader = ctx.get("_read_image_fn")
        if reader is not None:
            return reader(path), ""
        with open(path, "rb") as f:
            return f.read(), ""
    except OSError as exc:
        return None, f"cannot read {key}: {exc}"


def _clear_stage_note(ctx: dict, host) -> str:
    """Best-effort stage clear after a case; returns a reason suffix, never raises."""
    try:
        _update_client(ctx).clear_stage(host)
        return ""
    except Exception as exc:
        return f" (stage clear afterwards failed: {type(exc).__name__}: {exc})"


def _clear_stage_verified(ctx: dict, host) -> str:
    """Clear the stage and read it back. Returns "" only when GET
    /api/update/stage then reads ``staged: false``; otherwise a problem
    description (a failed clear, an unreadable read-back, or a stage that is
    still staged). Never raises. A case must not leave an image staged, where
    the recovery image's apply would offer to install it."""
    note = _clear_stage_note(ctx, host)
    st = _read_stage(ctx, host)
    if st is None:
        return f"stage could not be confirmed cleared (status unreadable){note}"
    if st.get("staged") is not False:
        return f"stage still reads staged={st.get('staged')!r} after a clear{note}"
    return ""


def _leftover_stage_note(ctx: dict, host) -> str:
    """For a FAIL path that may have left something staged: read the stage
    and, unless it reads ``staged: false``, clear it with a verified
    read-back. Returns a reason suffix ("" when nothing was left)."""
    st = _read_stage(ctx, host)
    if st is not None and st.get("staged") is False:
        return ""
    problem = _clear_stage_verified(ctx, host)
    return f"; LEFTOVER STAGE NOT CLEARED: {problem}" if problem else "; leftover stage cleared"


def _poll_until(ctx: dict, fn, done_fn, timeout_s: float):
    """Poll ``fn()`` (exceptions count as no answer) until ``done_fn(value)``;
    returns ``(last non-None value or None, whether done_fn was satisfied)``.
    Hooks: ctx["_now"], ctx["_sleep_fn"]; a hard attempt cap keeps it finite
    under an injected clock that never advances."""
    import math
    import time as _time
    now = ctx.get("_now") or _time.monotonic
    sleep = ctx.get("_sleep_fn") or _time.sleep
    deadline = now() + timeout_s
    last = None
    for _ in range(math.ceil(timeout_s / OTG_POLL_S) + 2):
        try:
            value = fn()
        except Exception:
            value = None
        if value is not None:
            last = value
            if done_fn(value):
                return last, True
        if now() >= deadline:
            break
        sleep(OTG_POLL_S)
    return last, False


def _case_otg01(ctx: dict) -> CaseResult:
    """OT-G01: the sha256 the stager reports for a staged upload must equal
    the SHA-256 of the exact bytes sent; a mismatch (the bad-sha class) is a
    FAIL, a board that cannot report it is never a pass. Needs
    ``ota_image_path``. Replaces a previously staged image and clears the
    stage again afterwards. NOT covered: a manifest-vs-stream mismatch on a
    GitHub download, which needs a crafted release (the stager has no
    expected-sha input; the fetch task does that comparison)."""
    if not ctx.get("ota_image_path"):
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-G01")
    image, why = _read_image(ctx, "ota_image_path")
    if image is None:
        return CaseResult(Verdict.SKIP, reason=why)
    gated = _otg_gate(ctx, "OT-G01")
    if gated:
        return gated
    host = ctx.get("host")
    upd = _update_client(ctx)
    local = upd.sha256_hex(image)
    try:
        upd.upload_stage(host, image)
    except Exception as exc:
        # A client-side timeout does not stop the board: it may still finish
        # staging. Check, and clear anything left.
        return CaseResult(Verdict.FAIL, reason=(f"valid image refused by the stager: {type(exc).__name__}: {exc}"
                                                f"{_leftover_stage_note(ctx, host)}"),
                          observed={"error": f"{type(exc).__name__}: {exc}"})
    st = _read_stage(ctx, host)
    problem = _clear_stage_verified(ctx, host)
    cleanup = f"; stage NOT confirmed cleared afterwards: {problem}" if problem else ""
    if st is None:
        return CaseResult(Verdict.FAIL, reason=f"stage status unreadable after upload{cleanup}")
    observed = {"local_sha256": local, "board_sha256": st.get("sha256"), "staged": st.get("staged"),
                "state": st.get("state")}
    if st.get("staged") is not True:
        return CaseResult(Verdict.FAIL,
                          reason=f"upload accepted but stage not staged (reason={st.get('reason')!r}){cleanup}",
                          observed=observed)
    if str(st.get("sha256", "")).lower() != local:
        return CaseResult(Verdict.FAIL, reason=f"board sha256 {st.get('sha256')!r} != local {local}{cleanup}",
                          observed=observed)
    if problem:
        return CaseResult(Verdict.FAIL, reason=f"stage sha256 equals local digest{cleanup}", observed=observed)
    return CaseResult(Verdict.PASS, reason="stage sha256 equals local digest; stage cleared", observed=observed)


def _case_otg02(ctx: dict) -> CaseResult:
    """OT-G02: an upload cut off mid-body (full Content-Length declared, 60 %
    sent, socket half-closed) must NOT leave a staged image: the board must
    answer an error or close the connection, and afterwards GET
    /api/update/stage must read staged:false and not busy. A 2xx answer, or a
    staged image, is a FAIL. Needs ``ota_image_path``. Wipes any previous
    stage (an upload begins by erasing the header)."""
    if not ctx.get("ota_image_path"):
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-G02")
    image, why = _read_image(ctx, "ota_image_path")
    if image is None:
        return CaseResult(Verdict.SKIP, reason=why)
    gated = _otg_gate(ctx, "OT-G02")
    if gated:
        return gated
    host = ctx.get("host")
    def _real_cut():
        from .. import update_release_http_client as _cut
        return _cut.upload_stage_truncated(host, image, 0.6)

    cut_fn = ctx.get("_truncated_upload_fn") or _real_cut
    try:
        status, detail = cut_fn()
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"truncated upload could not be performed: {type(exc).__name__}: {exc}")
    if status is not None and 200 <= status < 300:
        return CaseResult(Verdict.FAIL, reason=(f"board answered {status} to a truncated upload"
                                                f"{_leftover_stage_note(ctx, host)}"),
                          observed={"status": status, "detail": detail})
    # Strict booleans: a reply missing busy/staged is not "idle, nothing staged".
    st, _settled = _poll_until(ctx, lambda: _read_stage(ctx, host), lambda s: s.get("busy") is False,
                               OTA_REFUSAL_DRAIN_SETTLE_S)
    if st is None:
        return CaseResult(Verdict.FAIL, reason="stage status unreadable after the truncated upload")
    observed = {"status": status, "detail": detail, "staged": st.get("staged"), "phase": st.get("phase"),
                "busy": st.get("busy")}
    if st.get("busy") is not False:
        return CaseResult(Verdict.FAIL, reason=f"stager not idle after the truncated upload (busy={st.get('busy')!r})",
                          observed=observed)
    if st.get("staged") is not False:
        return CaseResult(Verdict.FAIL, reason=(f"a truncated upload left staged={st.get('staged')!r}"
                                                f"{_leftover_stage_note(ctx, host)}"), observed=observed)
    return CaseResult(Verdict.PASS, reason="truncated upload refused/aborted, nothing staged", observed=observed)


class _OtgDone(Exception):
    """Internal early exit from _fetch_job_case; the result is in ``args[0]``."""


def _restore_repo(ctx: dict, host, restore_to: str) -> str:
    """Put the repo setting back (empty string resets to the default); returns
    "" or a problem description. Verifies by read-back."""
    rel = _release_client(ctx)
    try:
        rel.set_settings(host, restore_to)
        now = rel.get_settings(host)
    except Exception as exc:
        return f"repo restore failed: {type(exc).__name__}: {exc}"
    if restore_to and now.get("repo") != restore_to:
        return f"repo restore read back {now.get('repo')!r}, expected {restore_to!r}"
    if not restore_to and now.get("is_default") is not True:
        return f"repo restore to default read back {now.get('repo')!r}, is_default={now.get('is_default')!r}"
    return ""


def _fetch_job_case(ctx: dict, label: str, repo_key: str, start_fn, judge) -> CaseResult:
    """Shared body of G03/G05: point the repo setting at ``ctx[repo_key]``,
    start a check/download job, wait for it to finish, judge the job status
    (and that the stage did not change), and always restore the repo setting
    (a failed restore is a FAIL and taints the run: it would leave the board
    pointed at another repo)."""
    repo = ctx.get(repo_key)
    if not repo:
        return CaseResult(Verdict.SKIP, reason=f"{repo_key} not provided for {label}")
    gated = _otg_gate(ctx, label)
    if gated:
        return gated
    host = ctx.get("host")
    rel = _release_client(ctx)
    try:
        settings = rel.get_settings(host)
    except Exception as exc:
        return CaseResult(Verdict.SKIP, reason=f"cannot read the repo setting: {type(exc).__name__}: {exc}")
    # Only a definite is_default lets the restore reproduce the setting: a
    # missing/odd flag would restore a default repo as an explicit custom one
    # (or a custom one to the default).
    is_default = settings.get("is_default")
    repo_now = settings.get("repo")
    if not isinstance(is_default, bool) or not isinstance(repo_now, str) or (not is_default and not repo_now):
        return CaseResult(Verdict.SKIP, reason=(f"repo setting not definite (repo={repo_now!r}, "
                                                f"is_default={is_default!r}): cannot promise to restore it"))
    restore_to = "" if is_default else repo_now
    stage_before = _read_stage(ctx, host)
    if stage_before is None:
        return CaseResult(Verdict.SKIP, reason="stage status unreadable before the case")

    def _run() -> CaseResult:
        try:
            rel.set_settings(host, repo)
            if rel.get_settings(host).get("repo") != repo:
                return CaseResult(Verdict.FAIL, reason=f"repo setting did not read back as {repo!r}")
        except Exception as exc:
            return CaseResult(Verdict.FAIL, reason=f"could not set repo: {type(exc).__name__}: {exc}")
        try:
            start_fn(rel, host)
        except Exception as exc:
            if getattr(rel, "error_name", lambda e: "")(exc) == "clock_not_synced":
                return CaseResult(Verdict.SKIP, reason="board clock not synced (409 clock_not_synced): no TLS fetch possible")
            return CaseResult(Verdict.FAIL, reason=f"job start failed: {type(exc).__name__}: {exc}")
        status, finished = _poll_until(ctx, lambda: rel.get_fetch_status(host),
                                       lambda s: s.get("state") in ("done", "failed"), OTG_FETCH_WAIT_S)
        cancel_note = ""
        if not finished:
            # A job still running would keep downloading (into the stage) after
            # the repo is restored: cancel it and wait for it to end.
            try:
                rel.cancel_fetch(host)
            except Exception as exc:
                cancel_note = f"; cancel failed: {type(exc).__name__}: {exc}"
            _st, ended = _poll_until(ctx, lambda: rel.get_fetch_status(host),
                                     lambda s: s.get("busy") is False and s.get("state") in ("done", "failed", "idle"),
                                     OTG_FETCH_CANCEL_WAIT_S)
            if not ended:
                ctx["_tainted"] = True
                cancel_note += "; job could NOT be confirmed ended after a cancel -- run tainted"
        stage_after = _read_stage(ctx, host)
        changed = stage_after is None or _stage_key(stage_after) != _stage_key(stage_before)
        leftover = _leftover_stage_note(ctx, host) if changed else ""
        if status is None or not finished:
            return CaseResult(Verdict.FAIL, reason=f"fetch job did not finish or status unreadable{cancel_note}{leftover}",
                              observed={"status": status})
        if stage_after is None:
            return CaseResult(Verdict.FAIL, reason=f"stage status unreadable after the job{leftover}")
        if changed:
            return CaseResult(Verdict.FAIL, reason=f"stage changed during a case that must not stage anything{leftover}",
                              observed={"before": _stage_key(stage_before), "after": _stage_key(stage_after)})
        return judge(status, repo)

    try:
        result = _run()
    except BaseException:
        if _restore_repo(ctx, host, restore_to):
            ctx["_tainted"] = True
        raise
    problem = _restore_repo(ctx, host, restore_to)
    if problem:
        ctx["_tainted"] = True
        note = f"{problem} -- run tainted"
        return CaseResult(Verdict.FAIL, reason=f"{result.reason}; {note}" if result.reason else note,
                          observed=result.observed)
    return result


def _judge_otg03(status: dict, repo: str) -> CaseResult:
    observed = {k: status.get(k) for k in ("state", "error", "http_status", "verdict", "reason", "allowed",
                                           "tag", "running", "bytes_done", "repo")}
    if status.get("state") == "done" or status.get("allowed") is True:
        return CaseResult(Verdict.FAIL, reason="a downgrade download was allowed/completed without allow_downgrade",
                          observed=observed)
    if (status.get("verdict") in ("refuse_downgrade", "refuse_too_old") and status.get("state") == "failed"
            and not status.get("bytes_done")):
        return CaseResult(Verdict.PASS, reason=f"downgrade refused ({status.get('verdict')}), nothing downloaded",
                          observed=observed)
    return CaseResult(Verdict.INCONCLUSIVE,
                      reason=(f"job failed without a downgrade verdict (error={status.get('error')!r}, "
                              f"verdict={status.get('verdict')!r}): repo has no older release, or the fetch itself failed"),
                      observed=observed)


def _case_otg03(ctx: dict) -> CaseResult:
    """OT-G03: with ``update_downgrade_repo`` (a repo whose latest release is
    OLDER than the running build) configured, ``POST /api/update/download``
    without allow_downgrade must fail with verdict refuse_downgrade (or
    refuse_too_old), download no bytes and leave the stage untouched. The
    repo setting is restored afterwards. A failure for any other reason
    (network, no release) is INCONCLUSIVE, never a pass."""
    return _fetch_job_case(ctx, "OT-G03", "update_downgrade_repo",
                           lambda rel, host: rel.start_download(host), _judge_otg03)


#: update_release.c's release-parse error names: GitHub answered 200 but the
#: latest release is not a usable kilnCtl release.
OTG05_RELEASE_ERRORS = frozenset({
    "bad_json", "no_tag", "bad_tag", "draft_release", "no_app_asset", "no_manifest_asset",
    "duplicate_asset", "bad_asset_url", "bad_asset_size",
})


def _judge_otg05(status: dict, repo: str) -> CaseResult:
    observed = {k: status.get(k) for k in ("state", "error", "http_status", "tag", "allowed", "repo")}
    if status.get("state") == "failed" and status.get("allowed") is not True and not status.get("tag"):
        if status.get("repo") not in (None, "", repo):
            return CaseResult(Verdict.FAIL, reason=f"job ran against {status.get('repo')!r}, not the configured {repo!r}",
                              observed=observed)
        err = status.get("error")
        # Only GitHub's own answer proves "this repo has no release": a 404, or
        # a 200 whose release does not parse. A transport failure
        # (connect_failed, timeout, low_heap, ...) or a 403/429 rate limit says
        # nothing about the repo and must not read as a pass.
        if (err == "http_status" and status.get("http_status") == 404) or err in OTG05_RELEASE_ERRORS:
            return CaseResult(Verdict.PASS, reason=f"check against {repo!r} failed cleanly "
                              f"(error={err!r}, http={status.get('http_status')})", observed=observed)
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=(f"check failed for a reason that does not show the repo has no release "
                                  f"(error={err!r}, http={status.get('http_status')!r})"), observed=observed)
    if status.get("state") == "done":
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{repo!r} has a usable release; pick a repo with none",
                          observed=observed)
    return CaseResult(Verdict.FAIL, reason=f"unexpected job outcome for a wrong repo: {observed}", observed=observed)


def _case_otg05(ctx: dict) -> CaseResult:
    """OT-G05: with ``update_wrong_repo`` (a repo with no release, or that
    does not exist) configured, ``POST /api/update/check`` must end failed
    with no tag and nothing allowed, the stage untouched, and the repo
    setting restored afterwards. A repo that does have a release is
    INCONCLUSIVE."""
    return _fetch_job_case(ctx, "OT-G05", "update_wrong_repo",
                           lambda rel, host: rel.start_check(host), _judge_otg05)


class _Res:
    def __init__(self, ok):
        self.ok = ok


def _case_otg04(ctx: dict) -> CaseResult:
    """OT-G04: with a firing running (so the OTA interlock/mode gate refuses)
    a stage upload must be refused and the stage must not change; the firing
    must continue. Reuses OT-E07's machinery (opt-in ``allow_heat`` AND
    ``ota_allow_heat``, starts and stops its own run, taints on an
    unconfirmed stop) with the push replaced by ``POST /api/update/stage``.
    Deliberately does not gate on idle/interlock-ok: it tests the busy path.
    The download route's refusal is not exercised here (needs the clock and
    network)."""
    if not ctx.get("ota_image_path"):
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-G04")
    image, why = _read_image(ctx, "ota_image_path")
    if image is None:
        return CaseResult(Verdict.SKIP, reason=why)
    host = ctx.get("host")
    upd = _update_client(ctx)
    get_state = ctx.get("_exec_state_fn")
    if get_state is None:
        srv = _srv(ctx)
        get_state = lambda: getattr(srv._profiles.get_exec_status(), "state_name", None)
    box: dict = {}

    def push():
        box["before"] = _read_stage(ctx, host)
        err = None
        try:
            upd.upload_stage(host, image)
        except Exception as exc:
            err = exc
        box["after"] = _read_stage(ctx, host)
        changed = (box["before"] is not None and box["after"] is not None
                   and _stage_key(box["before"]) != _stage_key(box["after"]))
        if err is None or changed:
            return _Res(True)  # accepted, or the stage moved: not a refusal
        raise err

    inner = dict(ctx)
    inner["_push_fn"] = push
    result = _case_update_refused_during_state(inner, get_state, ctx.get("_ote07_expected_state", OTE07_DEFAULT_STATE),
                                               default_heat=_default_ote07_heat)
    if inner.get("_tainted"):
        ctx["_tainted"] = inner["_tainted"]
    # Keys the heat helpers added (e.g. cases_heat's firing-history snapshots)
    # belong to the run, not to this case's private copy.
    for key, value in inner.items():
        if key != "_push_fn" and key not in ctx:
            ctx[key] = value
    if result.verdict == Verdict.PASS and (box.get("before") is None or box.get("after") is None):
        return CaseResult(Verdict.FAIL, reason="stage status unreadable around the refused upload",
                          observed=result.observed)
    if result.verdict != Verdict.PASS and "after" in box:
        after = box["after"]
        if after is None or (after.get("staged") is not False
                             and (box.get("before") is None or _stage_key(box["before"]) != _stage_key(after))):
            # The upload was accepted (or the stage is unknown): the heat is
            # stopped by now, so the clear is no longer mode-gated.
            leftover = _leftover_stage_note(ctx, host)
            return CaseResult(result.verdict, reason=f"{result.reason}{leftover}", observed=result.observed,
                              expected=result.expected, evidence=list(result.evidence) + [leftover.lstrip("; ")])
    return result


def _case_otg06_body(ctx: dict, state: dict) -> CaseResult:
    """OT-G06: a stage holding a VERIFIED copy of the RUNNING image must be
    auto-cleared on the next boot (``update_http_stale_stage_check``), not
    left to be re-applied. Needs ``ota_image_path`` = the image currently
    running. Uploads it, soft-resets the board (``POST /api/sw_reset``;
    executor idle and interlock ok are re-checked right before), waits for it
    to come back and requires GET /api/update/stage to read staged:false with
    boot_auto_cleared true. SKIPs before staging unless the image's
    esp_app_desc build time equals the board's fw_build, and before the reset
    unless the safety relay reads de-energized (as OT-B01). If the stage
    survives anyway: INCONCLUSIVE, stage cleared and verified (FAIL if it
    cannot be). Every exit clears what it staged. The real
    power-cut-between-set_boot-and-header-erase variant stays an operator
    case. Resets the ESP and, through the link, the safety processor."""
    if not ctx.get("ota_image_path"):
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-G06")
    image, why = _read_image(ctx, "ota_image_path")
    if image is None:
        return CaseResult(Verdict.SKIP, reason=why)
    gated = _otg_gate(ctx, "OT-G06")
    if gated:
        return gated
    host = ctx.get("host")
    # Stage only a copy of the RUNNING build: any other image would survive
    # the reboot as a real pending update. Unreadable either side refuses.
    from .. import esp_app_desc as _desc
    try:
        image_desc = _desc.parse_app_desc(image)
    except Exception as exc:
        return CaseResult(Verdict.SKIP, reason=f"ota_image_path has no readable app descriptor: {exc}")
    running = _fw_build(ctx, host)
    if running is None:
        return CaseResult(Verdict.SKIP, reason="board fw_build unreadable: cannot confirm ota_image_path is the running build")
    if not _desc.build_timestamps_match(image_desc, running):
        return CaseResult(Verdict.SKIP, reason=(f"ota_image_path build {image_desc.build_timestamp!r} is not the "
                                                f"running build {running!r}; OT-G06 needs the running image"))
    upd = _update_client(ctx)
    try:
        upd.upload_stage(host, image)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=(f"could not stage the image: {type(exc).__name__}: {exc}"
                                                f"{_leftover_stage_note(ctx, host)}"))
    st = _read_stage(ctx, host)
    if st is None or st.get("staged") is not True:
        return CaseResult(Verdict.FAIL, reason=f"image not staged after upload (status={st!r}){_leftover_stage_note(ctx, host)}")
    gated = _otg_gate(ctx, "OT-G06 reset")
    if gated is None and _relays_energized(ctx, host) is not False:
        # Same dual-reset precondition OT-B01 uses: unreadable counts as energized.
        gated = CaseResult(Verdict.SKIP, reason="safety relay energized or unreadable: not resetting both processors")
    if gated:
        problem = _clear_stage_verified(ctx, host)
        if problem:
            return CaseResult(Verdict.FAIL, reason=f"{gated.reason}; staged image NOT cleared: {problem}")
        return CaseResult(Verdict.SKIP, reason=f"{gated.reason}; stage cleared")
    state["reset_sent"] = True
    try:
        _ota_client(ctx).sw_reset(host)
        reset_note = "sw_reset sent"
    except Exception as exc:
        # The board may drop the connection as it resets; the poll below decides.
        reset_note = f"sw_reset raised {type(exc).__name__}"
    after, _ok = _poll_until(ctx, lambda: upd.get_stage_status(host, **_read_kw(ctx, "update_http_client")),
                             lambda s: s.get("staged") is False, OTG_REBOOT_WAIT_S)
    if after is None:
        return CaseResult(Verdict.FAIL, reason=(f"board did not answer GET /api/update/stage after reset ({reset_note})"
                                                f"{_leftover_stage_note(ctx, host)}"))
    observed = {"staged": after.get("staged"), "boot_auto_clear": after.get("boot_auto_clear"),
                "boot_auto_cleared": after.get("boot_auto_cleared"), "reset": reset_note}
    if after.get("staged") is False and after.get("boot_auto_cleared") is True:
        return CaseResult(Verdict.PASS, reason="stale stage auto-cleared at boot", observed=observed)
    if after.get("staged") is False:
        return CaseResult(Verdict.FAIL, reason="stage empty after reset but boot_auto_cleared is not true "
                          "(board may not have rebooted)", observed=observed)
    problem = _clear_stage_verified(ctx, host)
    if problem:
        return CaseResult(Verdict.FAIL, reason=(f"stage survived the reboot (boot_auto_clear="
                                                f"{after.get('boot_auto_clear')!r}) and could NOT be cleared: {problem}"),
                          observed=observed)
    return CaseResult(Verdict.INCONCLUSIVE,
                      reason=f"stage survived the reboot (boot_auto_clear={after.get('boot_auto_clear')!r}); "
                             f"stage cleared", observed=observed)


OTG06_SAFETY_LINK_WAIT_S = 30.0
OTG06_SAFETY_SETTLE_S = 5.0
OTG06_SAFETY_CLEAR_WAIT_S = 10.0


def _otg06_safety_after_reset(ctx: dict) -> "tuple[str, bool]":
    """Safety state after OT-G06's dual reset. Returns (note, bad). Same rules
    and seams as OT-B01: link up and no trip is fine; a trip that is exactly
    S6a (trip_reason 6, trip_mask == 1 << (6 - 1), link up) is cleared and
    confirmed; any other trip, a link that never comes up, or an unreadable
    status is bad and is never cleared."""
    srv = _srv(ctx)
    get_status_fn = ctx.get("_get_safety_status_fn") or (lambda: srv._safety.get_status())
    get_diag_fn = ctx.get("_get_safety_diag_fn") or (lambda: srv._safety.get_diag())
    clear_trip_fn = ctx.get("_clear_trip_fn", lambda: _default_clear_trip_fn(ctx))

    def _link_and_diag():
        if not getattr(get_status_fn(), "link_up", False):
            return None
        d = get_diag_fn()
        return d if getattr(d, "ever_received", False) else None

    diag, ok = _poll_until(ctx, _link_and_diag, lambda d: True, OTG06_SAFETY_LINK_WAIT_S)
    if not ok:
        return "safety link not up or no DIAG frame after dual reset (status unreadable)", True
    reason, mask = getattr(diag, "trip_reason", None), getattr(diag, "trip_mask", None)
    if reason is None:
        return "safety trip_reason unreadable after dual reset", True
    if reason == 0:
        # S6a is debounced; look a few seconds longer before recording no trip.
        def _redo():
            d = get_diag_fn()
            return d if getattr(d, "ever_received", False) else None
        d2, _ = _poll_until(ctx, _redo, lambda d: bool(getattr(d, "trip_reason", 0)), OTG06_SAFETY_SETTLE_S)
        if d2 is None:
            return "no DIAG frame during safety settle window after dual reset", True
        reason, mask = getattr(d2, "trip_reason", None), getattr(d2, "trip_mask", None)
        if reason is None:
            return "safety trip_reason unreadable after dual reset", True
        if reason == 0:
            return "safety link up, no trip after dual reset", False
    if not (reason == 6 and mask == J.safety_trip_mask_for_reason(6)):
        return (f"safety trip after dual reset is not S6a alone (trip_reason={reason!r}, "
                f"trip_mask={mask!r}); NOT cleared"), True
    try:
        clear_trip_fn()
    except Exception as exc:
        return f"S6a latched after dual reset; clear raised {type(exc).__name__}: {exc}", True
    _st, cleared = _poll_until(ctx, lambda: get_status_fn(), lambda s: bool(getattr(s, "enabled", False)),
                               OTG06_SAFETY_CLEAR_WAIT_S)
    if not cleared:
        return "S6a latched after dual reset; cleared but safety never re-enabled", True
    return "S6a latched after dual reset (mask 0x%04X) and was cleared" % mask, False


def _case_otg06(ctx: dict) -> CaseResult:
    """OT-G06 (see ``_case_otg06_body``) plus a post-reset safety check: the
    dual reset can latch S6a. After a reset was actually sent, the safety
    state must read link-up with no trip, or S6a alone (cleared as OT-B01
    does); anything else tampers the verdict to FAIL."""
    state: dict = {}
    result = _case_otg06_body(ctx, state)
    if not state.get("reset_sent"):
        return result
    note, bad = _otg06_safety_after_reset(ctx)
    evidence = list(result.evidence or []) + [note]
    observed = dict(result.observed or {})
    observed["safety_after_reset"] = note
    if bad:
        return CaseResult(Verdict.FAIL, reason=f"{result.reason}; {note} (prior verdict {result.verdict})",
                          observed=observed, expected=result.expected, evidence=evidence)
    return CaseResult(result.verdict, reason=f"{result.reason}; {note}", observed=observed,
                      expected=result.expected, evidence=evidence)



def _case_otb02(ctx: dict) -> CaseResult:
    """OT-B02: summarize the OT-* results of the run in progress. Reads
    ``ctx["_run_results"]`` (the runner's live results dict); never re-runs
    anything. FAIL if any OT case is FAIL/ERROR. Otherwise PASS only if at
    least one OT case PASSed and none is INCONCLUSIVE (SKIP/NOT_RUN rows are
    not counted); any INCONCLUSIVE row, or no PASS row at all (all
    NOT_RUN/SKIP, or a mix with INCONCLUSIVE), gives INCONCLUSIVE; a ctx with no results is an ERROR (reported
    as FAIL, the runner's mapping for a case error)."""
    results = ctx.get("_run_results")
    if not isinstance(results, dict):
        return CaseResult(Verdict.FAIL, reason="ERROR: no run results in ctx")
    rows = {cid: str(getattr(r, "verdict", r)) for cid, r in results.items()
            if cid.startswith("OT-") and cid != "OT-B02"}
    if not rows:
        return CaseResult(Verdict.FAIL, reason="ERROR: no OT-* results in ctx")
    table = [f"{cid}: {v}" for cid, v in sorted(rows.items())]
    bad = sorted(c for c, v in rows.items() if v in ("FAIL", "ERROR"))
    observed = {"table": table}
    if bad:
        return CaseResult(Verdict.FAIL, reason="OT failures: " + ", ".join(bad),
                          observed=observed, evidence=table)
    inconc = sorted(c for c, v in rows.items() if v == "INCONCLUSIVE")
    if inconc or not any(v == "PASS" for v in rows.values()):
        why = ("INCONCLUSIVE OT cases: " + ", ".join(inconc)) if inconc else "no OT case ran"
        return CaseResult(Verdict.INCONCLUSIVE, reason=why,
                          observed=observed, evidence=table)
    return CaseResult(Verdict.PASS, reason=f"{len(rows)} OT cases, none FAIL/ERROR/INCONCLUSIVE",
                      observed=observed, evidence=table)

# ---------------------------------------------------------------------------
# OT-E11 -- recovery image receives an app image (BENCH_TEST_SYSTEM_PLAN 3.4).
# ---------------------------------------------------------------------------

OTE11_RECOVERY_WAIT_S = 60.0
OTE11_POLL_S = 2.0


def _tool_ok(text: object) -> bool:
    return isinstance(text, str) and text.lstrip().lower().startswith("ok")


def _ote11_tool(mod: str, name: str):
    import importlib
    return getattr(importlib.import_module(f"kilnctrl.{mod}"), name)


def _ote11_in_recovery(text: object) -> bool:
    return isinstance(text, str) and text.lstrip().startswith("recovery image")


def _ote11_wait_recovery(ctx: dict, status_fn, timeout_s: float) -> bool:
    import time
    sleep = ctx.get("_sleep") or time.sleep
    waited = 0.0
    while True:
        try:
            if _ote11_in_recovery(status_fn()):
                return True
        except Exception:
            pass
        if waited >= timeout_s:
            return False
        sleep(OTE11_POLL_S)
        waited += OTE11_POLL_S


def _case_ote11(ctx: dict) -> CaseResult:
    """OT-E11: recovery_enter, confirm the recovery image answers, run LCD-20's
    observation while there, push the app image with recovery_push_esp_image,
    confirm RUNNING == app. Teardown attempts recovery_exit whenever the board
    is not confirmed back in the application and reports if it is still stuck
    in recovery (observed["left_in_recovery"], run taint)."""
    host = ctx.get("host")
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E11")
    if not (ctx.get("_isfile_fn") or os.path.isfile)(image_path):
        return CaseResult(Verdict.SKIP, reason=f"ota_image_path is not a file: {image_path!r}")
    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to enter recovery: {ireason}")

    enter_fn = ctx.get("_recovery_enter_fn") or (
        lambda: _ote11_tool("mcp_server_ota", "recovery_enter")(host=host, confirm=True))
    status_fn = ctx.get("_recovery_status_fn") or (
        lambda: _ote11_tool("mcp_server_recovery", "recovery_status")(host=host))
    push_fn = ctx.get("_recovery_push_fn") or (
        lambda: _ote11_tool("mcp_server_recovery", "recovery_push_esp_image")(
            image_path=image_path, confirm=True, host=host))
    exit_fn = ctx.get("_recovery_exit_fn") or (
        lambda: _ote11_tool("mcp_server_recovery", "recovery_exit")(confirm=True, host=host))

    observed: dict = {}
    entered = False
    pushed_ok = False
    try:
        try:
            entered_text = enter_fn()
        except Exception as exc:
            return CaseResult(Verdict.FAIL, reason=f"recovery_enter raised {type(exc).__name__}: {exc}")
        observed["recovery_enter"] = str(entered_text)[:300]
        if not _tool_ok(entered_text):
            return CaseResult(Verdict.INCONCLUSIVE, reason="recovery_enter refused or failed; board unchanged",
                              observed=observed)
        entered = True
        if not _ote11_wait_recovery(ctx, status_fn, ctx.get("ote11_recovery_wait_s", OTE11_RECOVERY_WAIT_S)):
            return CaseResult(Verdict.FAIL, reason="recovery image never answered recovery_status after recovery_enter",
                              observed=observed)
        # LCD-20 only ever runs here, while the recovery image is up.
        lcd20 = ctx.get("_lcd20_fn")
        if lcd20 is None:
            from . import cases_lcd
            lcd20 = cases_lcd.observe_recovery_idle
        try:
            ctx["_lcd20_result"] = lcd20(ctx)
        except Exception as exc:
            ctx["_lcd20_result"] = CaseResult(Verdict.INCONCLUSIVE,
                                              reason=f"LCD-20 observation raised {type(exc).__name__}: {exc}")
        try:
            push_text = push_fn()
        except Exception as exc:
            observed["push_error"] = f"{type(exc).__name__}: {exc}"
            return CaseResult(Verdict.FAIL, reason="recovery_push_esp_image raised", observed=observed)
        observed["recovery_push"] = str(push_text)[:400]
        pushed_ok = _tool_ok(push_text)
        running = _settled_running(ctx, host)
        observed["running_after"] = running
        if not pushed_ok:
            return CaseResult(Verdict.FAIL, reason="recovery_push_esp_image did not report ok", observed=observed)
        if running != "app":
            return CaseResult(Verdict.FAIL, reason=f"after the push RUNNING is {running!r}, expected 'app'",
                              observed=observed)
        safety_fn = ctx.get("_safety_status_fn")
        if safety_fn is not None:
            try:
                observed["safety_after"] = str(safety_fn())[:300]
            except Exception as exc:
                observed["safety_after"] = f"unreadable: {type(exc).__name__}"
        return CaseResult(Verdict.PASS, reason="recovery image accepted the app image; RUNNING == app",
                          observed=observed)
    finally:
        if entered and not (pushed_ok and observed.get("running_after") == "app"):
            try:
                observed["teardown_exit"] = str(exit_fn())[:300]
            except Exception as exc:
                observed["teardown_exit"] = f"raised {type(exc).__name__}: {exc}"
            try:
                still = _ote11_in_recovery(status_fn())
            except Exception:
                still = False
            observed["left_in_recovery"] = still
            if still:
                ctx.setdefault("_taint", []).append("OT-E11: board left in the recovery image")


_CASE_FUNCS = {
    "OT-E11": _case_ote11,
    "OT-B01": _case_otb01,
    "OT-B02": _case_otb02,
    "OT-E01": _case_ote01,
    "OT-E02": _case_ote02,
    "OT-E03": _case_ote03,
    "OT-E04": _case_ote04,
    "OT-E05": _case_ote05,
    "OT-E06": _case_ote06,
    "OT-E07": _case_ote07,
    "OT-E08": _case_ote08,
    "OT-E09": _case_ote09,
    "OT-E10": _case_ote10,
    "OT-E12": _case_ote12,
    "OT-P01": _case_otp01,
    "OT-P02": _case_otp02,
    "OT-P03": _case_otp03,
    "OT-P04": _case_otp04,
    "OT-P05": _case_otp05,
    "OT-G01": _case_otg01,
    "OT-G02": _case_otg02,
    "OT-G03": _case_otg03,
    "OT-G04": _case_otg04,
    "OT-G05": _case_otg05,
    "OT-G06": _case_otg06,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
